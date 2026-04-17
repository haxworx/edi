#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "Edi.h"
#include "edi_aipanel.h"
#include "edi_agent.h"
#include "edi_theme.h"
#include "edi_config.h"

#include "edi_private.h"

static Evas_Object *_edi_ai_widget;
static Evas_Object *_edi_ai_button;
static Evas_Object *_edi_ai_copy_button;
static Evas_Object *_edi_ai_entry;
static Elm_Code *_edi_ai_code;
static Eina_Bool _edi_ai_busy = EINA_FALSE;
static Eina_Bool _edi_ai_follow_tail = EINA_TRUE;
static unsigned int _edi_ai_stream_row = 0;
static Eina_Strbuf *_edi_ai_stream_buf = NULL;
static Edi_Agent_Request *_edi_ai_request = NULL;
static char *_edi_ai_last_response = NULL;
static char *_edi_ai_active_prompt = NULL;
static char *_edi_ai_repo_inspection = NULL;
static char *_edi_ai_last_task_prompt = NULL;
static char *_edi_ai_last_stage_cmd = NULL;
static char *_edi_ai_last_stage_output = NULL;
static int _edi_ai_last_stage_exit_code = 0;
static unsigned int _edi_ai_last_stage_index = 0;
static Eina_Bool _edi_ai_last_stage_valid = EINA_FALSE;
static unsigned int _edi_ai_agent_steps = 0;
static Eina_Bool _edi_ai_agent_inspected_root = EINA_FALSE;
static Eina_Bool _edi_ai_agent_inspected_all_files = EINA_FALSE;
static char *_edi_ai_prompt_tmpl_initial = NULL;
static char *_edi_ai_prompt_tmpl_followup = NULL;
static char *_edi_ai_prompt_tmpl_resume = NULL;
static Eina_Bool _edi_ai_prompt_templates_loaded = EINA_FALSE;

#define EDI_AI_TAG_USER "[user]"
#define EDI_AI_TAG_AGENT "[agent]"
#define EDI_AI_TAG_ERROR "[error]"
#define EDI_AI_TAG_RUN "[run]"
#define EDI_AI_RUN_OPEN "<edi-run>"
#define EDI_AI_RUN_CLOSE "</edi-run>"
#define EDI_AI_RUN_MAX_STEPS_DEFAULT 256
#define EDI_AI_RUN_MAX_OUTPUT 120000

static void _edi_aipanel_append_line(const char *line);

static Eina_Bool
_edi_aipanel_edits_enabled_get(void)
{
   if (!_edi_project_config)
     return EINA_FALSE;

   if (!_edi_project_config->agent.enabled)
     return EINA_FALSE;

   return _edi_project_config->agent.edits_enabled;
}

static unsigned int
_edi_aipanel_steps_max_get(void)
{
   int configured;

   if (!_edi_project_config)
     return EDI_AI_RUN_MAX_STEPS_DEFAULT;

   configured = _edi_project_config->agent.steps_max;
   if (configured < 1)
     return EDI_AI_RUN_MAX_STEPS_DEFAULT;

   return (unsigned int) configured;
}

static char *
_edi_aipanel_shell_quote(const char *src)
{
   Eina_Strbuf *buf;
   const char *p;
   char *out;

   if (!src)
     return strdup("''");

   buf = eina_strbuf_new();
   eina_strbuf_append_char(buf, '\'');
   for (p = src; *p; p++)
     {
        if (*p == '\'')
          eina_strbuf_append(buf, "'\\''");
        else
          eina_strbuf_append_char(buf, *p);
     }
   eina_strbuf_append_char(buf, '\'');
   out = eina_strbuf_string_steal(buf);
   eina_strbuf_free(buf);
   return out;
}

static char *
_edi_aipanel_text_clip(const char *text, size_t max_len)
{
   Eina_Strbuf *buf;

   if (!text)
     return strdup("");

   if (strlen(text) <= max_len)
     return strdup(text);

   buf = eina_strbuf_new();
   eina_strbuf_append_length(buf, text, max_len);
   eina_strbuf_append(buf, "\n... (truncated)");
   return eina_strbuf_string_steal(buf);
}

static char *
_edi_aipanel_repo_state_get(void)
{
   const char *project;
   char *project_quoted;
   const char *script;
   char *script_quoted;
   const char *cmd;
   char *out;

   project = edi_project_get();
   if (!project || !project[0])
     return strdup("Project path is not set.");

   project_quoted = _edi_aipanel_shell_quote(project);
   script = eina_slstr_printf("cd %s && pwd && echo && git status --short --branch 2>&1",
                              project_quoted);
   script_quoted = _edi_aipanel_shell_quote(script);
   cmd = eina_slstr_printf("/bin/sh -lc %s",
                           script_quoted);
   out = edi_exe_response(cmd);
   free(script_quoted);
   free(project_quoted);

   if (!out)
     return strdup("Unable to gather repo state.");
   return out;
}

static char *
_edi_aipanel_repo_inspection_get(void)
{
   const char *project;
   char *project_quoted;
   const char *script;
   char *script_quoted;
   const char *cmd;
   char *out;

   project = edi_project_get();
   if (!project || !project[0])
     return NULL;

   project_quoted = _edi_aipanel_shell_quote(project);
   script = eina_slstr_printf("cd %s && pwd && echo && ls -la && echo && "
                              "find . -type f | sed 's#^\\./##' | sort",
                              project_quoted);
   script_quoted = _edi_aipanel_shell_quote(script);
   cmd = eina_slstr_printf("/bin/sh -lc %s", script_quoted);
   out = edi_exe_response(cmd);
   free(script_quoted);
   free(project_quoted);

   if (!out || !out[0])
     {
        free(out);
        return NULL;
     }

   if (strncmp(out, project, strlen(project)) != 0)
     {
        free(out);
        return NULL;
     }

   return out;
}

static char *
_edi_aipanel_file_read_all(const char *path)
{
   FILE *f;
   long len;
   char *buf;
   size_t got;

   if (!path || !path[0])
     return NULL;

   f = fopen(path, "rb");
   if (!f)
     return NULL;

   if (fseek(f, 0, SEEK_END) != 0)
     {
        fclose(f);
        return NULL;
     }

   len = ftell(f);
   if (len < 0)
     {
        fclose(f);
        return NULL;
     }
   rewind(f);

   buf = calloc(1, (size_t) len + 1);
   if (!buf)
     {
        fclose(f);
        return NULL;
     }

   got = fread(buf, 1, (size_t) len, f);
   fclose(f);
   if (got != (size_t) len)
     {
        free(buf);
        return NULL;
     }

   buf[len] = '\0';
   return buf;
}

static void
_edi_aipanel_prompt_templates_ensure(void)
{
   const char *path;

   if (_edi_ai_prompt_templates_loaded)
     return;
   _edi_ai_prompt_templates_loaded = EINA_TRUE;

   path = eina_slstr_printf(PACKAGE_DATA_DIR "/prompts/agent_prompt_initial.txt");
   _edi_ai_prompt_tmpl_initial = _edi_aipanel_file_read_all(path);

   path = eina_slstr_printf(PACKAGE_DATA_DIR "/prompts/agent_prompt_followup.txt");
   _edi_ai_prompt_tmpl_followup = _edi_aipanel_file_read_all(path);

   path = eina_slstr_printf(PACKAGE_DATA_DIR "/prompts/agent_prompt_resume.txt");
   _edi_ai_prompt_tmpl_resume = _edi_aipanel_file_read_all(path);
}

static char *
_edi_aipanel_prompt_template_render(const char *tmpl,
                                    const char **keys,
                                    const char **vals,
                                    unsigned int count)
{
   Eina_Strbuf *buf;
   const char *p;
   char *out;

   if (!tmpl)
     return NULL;

   buf = eina_strbuf_new();
   p = tmpl;
   while (p && *p)
     {
        const char *start = strstr(p, "{{");
        const char *end;
        char *key;
        unsigned int i;
        const char *value = "";

        if (!start)
          {
             eina_strbuf_append(buf, p);
             break;
          }

        eina_strbuf_append_length(buf, p, start - p);
        end = strstr(start + 2, "}}");
        if (!end)
          {
             eina_strbuf_append(buf, start);
             break;
          }

        key = strndup(start + 2, end - (start + 2));
        for (i = 0; i < count; i++)
          {
             if (!strcmp(keys[i], key))
               {
                  value = vals[i] ?: "";
                  break;
               }
          }
        eina_strbuf_append(buf, value);
        free(key);
        p = end + 2;
     }

   out = eina_strbuf_string_steal(buf);
   eina_strbuf_free(buf);
   return out;
}

static char *
_edi_aipanel_prompt_build_initial(const char *user_prompt)
{
   char *state;
   char *out;
   const char *keys[5];
   const char *vals[5];
   const char *inspection;

   _edi_aipanel_prompt_templates_ensure();

   state = _edi_aipanel_repo_state_get();
   inspection = (_edi_ai_repo_inspection && _edi_ai_repo_inspection[0])
                ? _edi_ai_repo_inspection : "(inspection unavailable)";
   keys[0] = "RUN_OPEN"; vals[0] = EDI_AI_RUN_OPEN;
   keys[1] = "RUN_CLOSE"; vals[1] = EDI_AI_RUN_CLOSE;
   keys[2] = "REPO_INSPECTION"; vals[2] = inspection;
   keys[3] = "STATE"; vals[3] = state ?: "";
   keys[4] = "USER_REQUEST"; vals[4] = user_prompt ?: "";
   out = _edi_aipanel_prompt_template_render(_edi_ai_prompt_tmpl_initial, keys, vals, 5);

   free(state);
   if (out)
     return out;

   return strdup(user_prompt ?: "");
}

static char *
_edi_aipanel_prompt_build_followup(const char *executed_cmd, int exit_code,
                                   const char *output, const char *file_preview)
{
   char *state;
   char *output_clip;
   char *out;
   char exit_code_buf[32];
   const char *keys[8];
   const char *vals[8];

   _edi_aipanel_prompt_templates_ensure();

   state = _edi_aipanel_repo_state_get();
   output_clip = _edi_aipanel_text_clip(output, EDI_AI_RUN_MAX_OUTPUT);
   snprintf(exit_code_buf, sizeof(exit_code_buf), "%d", exit_code);

   keys[0] = "RUN_OPEN"; vals[0] = EDI_AI_RUN_OPEN;
   keys[1] = "RUN_CLOSE"; vals[1] = EDI_AI_RUN_CLOSE;
   keys[2] = "ORIGINAL_USER_REQUEST"; vals[2] = _edi_ai_active_prompt ?: "";
   keys[3] = "EXECUTED_COMMAND"; vals[3] = executed_cmd ?: "";
   keys[4] = "EXIT_CODE"; vals[4] = exit_code_buf;
   keys[5] = "COMMAND_OUTPUT"; vals[5] = output_clip ?: "";
   keys[6] = "FILE_RESULT"; vals[6] = file_preview ?: "";
   keys[7] = "STATE"; vals[7] = state ?: "";
   out = _edi_aipanel_prompt_template_render(_edi_ai_prompt_tmpl_followup, keys, vals, 8);

   free(state);
   free(output_clip);
   if (out)
     return out;

   return strdup(_edi_ai_active_prompt ?: "");
}

static char *
_edi_aipanel_prompt_build_resume(const char *user_prompt)
{
   char *state;
   char *out;
   char stage_buf[32];
   char exit_buf[32];
   const char *keys[9];
   const char *vals[9];

   _edi_aipanel_prompt_templates_ensure();

   state = _edi_aipanel_repo_state_get();
   snprintf(stage_buf, sizeof(stage_buf), "%u", _edi_ai_last_stage_index);
   snprintf(exit_buf, sizeof(exit_buf), "%d", _edi_ai_last_stage_exit_code);

   keys[0] = "RUN_OPEN"; vals[0] = EDI_AI_RUN_OPEN;
   keys[1] = "RUN_CLOSE"; vals[1] = EDI_AI_RUN_CLOSE;
   keys[2] = "ORIGINAL_USER_REQUEST"; vals[2] = _edi_ai_last_task_prompt ?: "";
   keys[3] = "LAST_STAGE_INDEX"; vals[3] = stage_buf;
   keys[4] = "LAST_COMMAND"; vals[4] = _edi_ai_last_stage_cmd ?: "";
   keys[5] = "LAST_EXIT_CODE"; vals[5] = exit_buf;
   keys[6] = "LAST_COMMAND_OUTPUT"; vals[6] = _edi_ai_last_stage_output ?: "";
   keys[7] = "STATE"; vals[7] = state ?: "";
   keys[8] = "RESUME_REQUEST"; vals[8] = user_prompt ?: "";
   out = _edi_aipanel_prompt_template_render(_edi_ai_prompt_tmpl_resume, keys, vals, 9);

   free(state);
   if (out)
     return out;

   return strdup(user_prompt ?: "");
}

static void
_edi_aipanel_stage_memory_clear(void)
{
   free(_edi_ai_last_task_prompt);
   _edi_ai_last_task_prompt = NULL;
   free(_edi_ai_last_stage_cmd);
   _edi_ai_last_stage_cmd = NULL;
   free(_edi_ai_last_stage_output);
   _edi_ai_last_stage_output = NULL;
   _edi_ai_last_stage_exit_code = 0;
   _edi_ai_last_stage_index = 0;
   _edi_ai_last_stage_valid = EINA_FALSE;
}

static Eina_Bool
_edi_aipanel_prompt_is_continue_request(const char *prompt)
{
   char *lower;
   char *p;
   Eina_Bool out = EINA_FALSE;

   if (!prompt || !prompt[0])
     return EINA_FALSE;

   lower = strdup(prompt);
   if (!lower)
     return EINA_FALSE;

   for (p = lower; *p; p++)
     *p = tolower((unsigned char)*p);

   if (strstr(lower, "continue") || strstr(lower, "resume") ||
       strstr(lower, "carry on") || strstr(lower, "keep going") ||
       strstr(lower, "proceed"))
     out = EINA_TRUE;

   free(lower);
   return out;
}

static char *
_edi_aipanel_response_command_extract(const char *response)
{
   const char *start;
   const char *end;
   char *cmd;
   char *trim;
   char *p;

   if (!response)
     return NULL;

   start = strstr(response, EDI_AI_RUN_OPEN);
   if (!start)
     return NULL;

   start += strlen(EDI_AI_RUN_OPEN);
   end = strstr(start, EDI_AI_RUN_CLOSE);
   if (!end || end <= start)
     return NULL;

   cmd = strndup(start, end - start);
   if (!cmd)
     return NULL;

   trim = cmd;
   while (*trim && isspace((unsigned char)*trim))
     trim++;
   if (trim != cmd)
     memmove(cmd, trim, strlen(trim) + 1);

   p = cmd + strlen(cmd);
   while (p > cmd && isspace((unsigned char)*(p - 1)))
     *--p = '\0';

   if (!cmd[0])
     {
        free(cmd);
        return NULL;
     }
   return cmd;
}

static Eina_Bool
_edi_aipanel_command_token_get(const char *cmd, char *token, size_t token_len)
{
   const char *p;
   size_t i = 0;

   if (!cmd || !token || token_len < 2)
     return EINA_FALSE;

   p = cmd;
   while (*p && isspace((unsigned char)*p))
     p++;

   while (*p && !isspace((unsigned char)*p) && i < token_len - 1)
     token[i++] = *p++;
   token[i] = '\0';

   return token[0] != '\0';
}

static Eina_Bool
_edi_aipanel_command_is_inspection(const char *cmd)
{
   char token[64];

   if (!_edi_aipanel_command_token_get(cmd, token, sizeof(token)))
     return EINA_FALSE;

   if (!strcmp(token, "pwd") || !strcmp(token, "ls") || !strcmp(token, "find") ||
       !strcmp(token, "grep") || !strcmp(token, "sed") || !strcmp(token, "awk") ||
       !strcmp(token, "cat") || !strcmp(token, "head") || !strcmp(token, "tail") ||
       !strcmp(token, "wc") || !strcmp(token, "cut") || !strcmp(token, "sort") ||
       !strcmp(token, "uniq") || !strcmp(token, "tr") || !strcmp(token, "test"))
     return EINA_TRUE;

   if (!strcmp(token, "git"))
     {
        if (strstr(cmd, "git status") || strstr(cmd, "git diff") ||
            strstr(cmd, "git show") || strstr(cmd, "git log"))
          return EINA_TRUE;
     }

   return EINA_FALSE;
}

static Eina_Bool
_edi_aipanel_command_is_mutating(const char *cmd)
{
   char token[64];

   if (!_edi_aipanel_command_token_get(cmd, token, sizeof(token)))
     return EINA_FALSE;

   if (!strcmp(token, "mkdir") || !strcmp(token, "touch") || !strcmp(token, "cp") ||
       !strcmp(token, "mv") || !strcmp(token, "tee") || !strcmp(token, "chmod") ||
       !strcmp(token, "make") || !strcmp(token, "ninja") || !strcmp(token, "cmake") ||
       !strcmp(token, "meson") || !strcmp(token, "sh") || !strcmp(token, "set"))
     return EINA_TRUE;

   if (!strcmp(token, "git"))
     {
        if (strstr(cmd, "git add") || strstr(cmd, "git rm") || strstr(cmd, "git mv") ||
            strstr(cmd, "git commit") || strstr(cmd, "git checkout ") || strstr(cmd, "git restore"))
          return EINA_TRUE;
     }

   return EINA_FALSE;
}

static char *
_edi_aipanel_token_unquote(const char *token)
{
   size_t len;
   char *out;

   if (!token)
     return NULL;

   len = strlen(token);
   if (len >= 2 &&
       ((token[0] == '\'' && token[len - 1] == '\'') ||
        (token[0] == '"' && token[len - 1] == '"')))
     return strndup(token + 1, len - 2);

   out = strdup(token);
   return out;
}

static char *
_edi_aipanel_command_primary_line_get(const char *cmd)
{
   const char *start;
   const char *end;

   if (!cmd)
     return strdup("");

   start = cmd;
   while (*start == '\r' || *start == '\n')
     start++;
   end = strchr(start, '\n');
   if (!end)
     return strdup(start);

   return strndup(start, end - start);
}

static char *
_edi_aipanel_last_token_get(const char *cmd)
{
   char *line;
   const char *p;
   const char *start;
   char *raw;
   char *out;

   if (!cmd || !cmd[0])
     return NULL;

   line = _edi_aipanel_command_primary_line_get(cmd);
   if (!line)
     return NULL;

   p = line + strlen(line);
   while (p > line && isspace((unsigned char)*(p - 1)))
     p--;
   if (p == line)
     {
        free(line);
        return NULL;
     }

   start = p;
   while (start > line && !isspace((unsigned char)*(start - 1)))
     start--;

   raw = strndup(start, p - start);
   out = _edi_aipanel_token_unquote(raw);
   free(raw);
   free(line);
   return out;
}

static Eina_Bool
_edi_aipanel_path_is_safe_repo_target(const char *path)
{
   const char *p;
   const char *seg;

   if (!path || !path[0])
     return EINA_FALSE;

   if (path[0] == '/' || path[0] == '~')
     return EINA_FALSE;

   if (!strcmp(path, ".") || !strcmp(path, ".."))
     return EINA_FALSE;

   p = path;
   while (*p)
     {
        if (!isalnum((unsigned char)*p) &&
            *p != '/' && *p != '.' && *p != '_' && *p != '-')
          return EINA_FALSE;
        p++;
     }

   if (strstr(path, "//"))
     return EINA_FALSE;
   if (!strncmp(path, "../", 3) || strstr(path, "/../") ||
       !strcmp(path, "..") || !strcmp(path, "."))
     return EINA_FALSE;
   if (!strncmp(path, "./../", 5))
     return EINA_FALSE;
   if (path[strlen(path) - 1] == '/')
     return EINA_FALSE;

   seg = path;
   for (p = path; ; p++)
     {
        if (*p == '/' || *p == '\0')
          {
             size_t len = p - seg;

             if (len == 0)
               return EINA_FALSE;
             if ((len == 1 && seg[0] == '.') ||
                 (len == 2 && seg[0] == '.' && seg[1] == '.'))
               return EINA_FALSE;

             if (*p == '\0')
               break;
             seg = p + 1;
          }
     }

   return EINA_TRUE;
}

static Eina_Bool
_edi_aipanel_redirections_validate(const char *cmd, Eina_Bool *has_redirection,
                                   char **target_out)
{
   char *line;
   const char *p;
   Eina_Bool in_single = EINA_FALSE;
   Eina_Bool in_double = EINA_FALSE;
   Eina_Bool escaped = EINA_FALSE;
   char *selected_target = NULL;

   if (has_redirection)
     *has_redirection = EINA_FALSE;
   if (target_out)
     *target_out = NULL;
   if (!cmd || !cmd[0])
     return EINA_TRUE;

   line = _edi_aipanel_command_primary_line_get(cmd);
   if (!line)
     return EINA_FALSE;

   for (p = line; *p; p++)
     {
        const char *t;
        const char *end;
        const char *prev;
        char *target;

        if (escaped)
          {
             escaped = EINA_FALSE;
             continue;
          }

        if (*p == '\\' && !in_single)
          {
             escaped = EINA_TRUE;
             continue;
          }

        if (*p == '\'' && !in_double)
          {
             in_single = !in_single;
             continue;
          }
        if (*p == '"' && !in_single)
          {
             in_double = !in_double;
             continue;
          }

        if (in_single || in_double || *p != '>')
          continue;

        if (has_redirection)
          *has_redirection = EINA_TRUE;

        prev = p;
        while (prev > line && isspace((unsigned char)*(prev - 1)))
          prev--;
        if (prev > line &&
            (isdigit((unsigned char)*(prev - 1)) || *(prev - 1) == '&'))
          goto invalid;

        t = p + 1;
        if (*t == '>')
          t++;
        while (*t && isspace((unsigned char)*t))
          t++;
        if (!*t)
          goto invalid;

        if (*t == '&' || *t == '<' || *t == '>' || *t == '|' || *t == ';' || *t == '(')
          goto invalid;

        end = t;
        while (*end && !isspace((unsigned char)*end) &&
               *end != ';' && *end != '|' && *end != '&' &&
               *end != '<' && *end != '>')
          end++;
        if (end == t)
          goto invalid;

        target = strndup(t, end - t);
        if (!target)
          goto invalid;
        if (!_edi_aipanel_path_is_safe_repo_target(target))
          {
             free(target);
             goto invalid;
          }

        free(selected_target);
        selected_target = target;
     }

   if (target_out)
     *target_out = selected_target;
   else
     free(selected_target);
   free(line);
   return EINA_TRUE;

invalid:
   free(selected_target);
   free(line);
   return EINA_FALSE;
}

static char *
_edi_aipanel_created_path_guess(const char *cmd)
{
   char token[64];
   char *last;

   if (!_edi_aipanel_command_token_get(cmd, token, sizeof(token)))
     return NULL;

   if (!strcmp(token, "touch") || !strcmp(token, "cp") ||
       !strcmp(token, "mv") || !strcmp(token, "tee"))
     {
        last = _edi_aipanel_last_token_get(cmd);
        if (!last || !last[0] || last[0] == '-' || strchr(last, '|') || strchr(last, ';') ||
            !_edi_aipanel_path_is_safe_repo_target(last))
          {
             free(last);
             return NULL;
          }
        return last;
     }

   if (_edi_aipanel_redirections_validate(cmd, NULL, &last))
     return last;

   return NULL;
}

static char *
_edi_aipanel_precreate_path_guess(const char *cmd)
{
   char token[64];
   char *last;

   if (!cmd || !cmd[0])
     return NULL;

   if (_edi_aipanel_redirections_validate(cmd, NULL, &last))
     return last;

   if (!_edi_aipanel_command_token_get(cmd, token, sizeof(token)))
     return NULL;

   if (!strcmp(token, "tee"))
     {
        last = _edi_aipanel_last_token_get(cmd);
        if (!last || !last[0] || last[0] == '-' || strchr(last, '|') || strchr(last, ';') ||
            !_edi_aipanel_path_is_safe_repo_target(last))
          {
             free(last);
             return NULL;
          }
        return last;
     }

   return NULL;
}

static Eina_Bool
_edi_aipanel_file_preview_indicates_failure(const char *file_preview)
{
   if (!file_preview || !file_preview[0])
     return EINA_FALSE;

   if (strstr(file_preview, "status: missing"))
     return EINA_TRUE;
   if (strstr(file_preview, "status: exists_not_file"))
     return EINA_TRUE;
   if (strstr(file_preview, "status: unknown"))
     return EINA_TRUE;

   return EINA_FALSE;
}

static char *
_edi_aipanel_file_preview_widget_summary_get(const char *file_preview)
{
   const char *preview;

   if (!file_preview || !file_preview[0])
     return strdup("");

   preview = strstr(file_preview, "\npreview:\n");
   if (!preview)
     return strdup(file_preview);

   return strndup(file_preview, preview - file_preview);
}

static char *
_edi_aipanel_created_file_preview_get(const char *cmd)
{
   char *path;
   char *project_quoted;
   char *path_quoted;
   const char *script;
   char *script_quoted;
   const char *full_cmd;
   char *out;
   char *clip;

   path = _edi_aipanel_created_path_guess(cmd);
   if (!path)
     return NULL;

   project_quoted = _edi_aipanel_shell_quote(edi_project_get() ?: ".");
   path_quoted = _edi_aipanel_shell_quote(path);
   script = eina_slstr_printf(
      "cd %s && if [ -f %s ]; then "
      "printf 'status: file_exists\\npath: %s\\nsize_bytes: '; wc -c < %s; "
      "printf 'preview:\\n'; sed -n '1,120p' %s; "
      "elif [ -e %s ]; then "
      "printf 'status: exists_not_file\\npath: %s\\n'; "
      "else "
      "printf 'status: missing\\npath: %s\\n'; "
      "fi",
      project_quoted, path_quoted, path_quoted, path_quoted, path_quoted,
      path_quoted, path_quoted, path_quoted);
   script_quoted = _edi_aipanel_shell_quote(script);
   full_cmd = eina_slstr_printf("/bin/sh -lc %s", script_quoted);
   out = edi_exe_response(full_cmd);

   free(script_quoted);
   free(path_quoted);
   free(project_quoted);
   free(path);

   if (!out)
     return strdup("status: unknown\nreason: preview command failed");

   clip = _edi_aipanel_text_clip(out, 8000);
   free(out);
   return clip;
}

static void
_edi_aipanel_inspection_state_update(const char *cmd, int exit_code)
{
   char token[64];

   if (!cmd || exit_code != 0)
     return;

   if (strstr(cmd, "pwd"))
     _edi_ai_agent_inspected_root = EINA_TRUE;

   if (_edi_aipanel_command_token_get(cmd, token, sizeof(token)))
     {
        if (!strcmp(token, "ls"))
          _edi_ai_agent_inspected_root = EINA_TRUE;
        if (!strcmp(token, "find") && strstr(cmd, "-type f") &&
            (strstr(cmd, "find .") || strstr(cmd, "find ./")))
          _edi_ai_agent_inspected_all_files = EINA_TRUE;
     }
}

static Eina_Bool
_edi_aipanel_command_allowed(const char *cmd, const char **reason)
{
   static const char *allowed[] =
   {
      "pwd", "ls", "find", "grep", "sed", "awk", "cat", "head", "tail", "wc",
      "cut", "sort", "uniq", "tr", "xargs", "mkdir", "touch", "cp", "mv",
      "git", "printf", "echo", "meson", "ninja", "make", "cmake", "sh", "set",
      "cd", "tee", "chmod", "test"
   };
   static const char *blocked[] =
   {
      "rm -rf", "sudo ", "git reset --hard", "git checkout --",
   };
   char token[64];
   char *redir_target = NULL;
   Eina_Bool has_redirection = EINA_FALSE;
   unsigned int i;

   if (!cmd || !cmd[0])
     {
        *reason = "Empty command.";
        return EINA_FALSE;
     }

   for (i = 0; i < sizeof(blocked) / sizeof(blocked[0]); i++)
     {
        if (strstr(cmd, blocked[i]))
          {
             *reason = "Blocked dangerous command.";
             return EINA_FALSE;
          }
     }

   if (!_edi_aipanel_command_token_get(cmd, token, sizeof(token)))
     {
        *reason = "Missing command token.";
        return EINA_FALSE;
     }

   if (!_edi_aipanel_redirections_validate(cmd, &has_redirection, &redir_target))
     {
        *reason = "Unsafe redirection target.";
        return EINA_FALSE;
     }
   if (has_redirection && !redir_target)
     {
        *reason = "Missing redirection target.";
        return EINA_FALSE;
     }
   free(redir_target);

   for (i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++)
     {
        if (!strcmp(token, allowed[i]))
          return EINA_TRUE;
     }

   *reason = "Command not in allow list.";
   return EINA_FALSE;
}

static char *
_edi_aipanel_command_script_build(const char *project_quoted,
                                  const char *command,
                                  const char *precreate_quoted,
                                  const char *tag)
{
   Eina_Strbuf *script;
   char *out;

   script = eina_strbuf_new();
   if (!script)
     return NULL;

   if (precreate_quoted)
     {
        eina_strbuf_append_printf(script,
                                  "cd %s && { "
                                  "parent=$(dirname %s); "
                                  "[ -d \"$parent\" ] || mkdir -p \"$parent\"; "
                                  "if [ ! -d \"$parent\" ]; then "
                                  "printf 'ERROR: failed to ensure parent directory %s\\n'; "
                                  "exit 96; "
                                  "fi; "
                                  "[ -e %s ] || : > %s; "
                                  "if [ ! -e %s ]; then "
                                  "printf 'ERROR: failed to precreate file %s\\n'; "
                                  "exit 97; "
                                  "fi; "
                                  "if [ ! -f %s ]; then "
                                  "printf 'ERROR: failed to precreate regular file %s\\n'; "
                                  "exit 98; "
                                  "fi; "
                                  "}; { %s; } 2>&1; "
                                  "printf '\\n%s%%s\\n' \"$?\"",
                                  project_quoted, precreate_quoted, precreate_quoted,
                                  precreate_quoted, precreate_quoted, precreate_quoted,
                                  precreate_quoted, precreate_quoted, precreate_quoted,
                                  command, tag);
     }
   else
     {
        eina_strbuf_append_printf(script,
                                  "cd %s && { %s; } 2>&1; "
                                  "printf '\\n%s%%s\\n' \"$?\"",
                                  project_quoted, command, tag);
     }

   out = eina_strbuf_string_steal(script);
   eina_strbuf_free(script);
   return out;
}

static char *
_edi_aipanel_command_run(const char *command, int *exit_code)
{
   char *precreate_path;
   char *precreate_quoted = NULL;
   char *project_quoted;
   char *script;
   char *script_quoted;
   const char *full_cmd;
   char *raw;
   char *marker;
   char *last = NULL;
   char *out;
   const char *tag = "__EDI_EXIT_CODE__:";

   if (exit_code)
     *exit_code = -1;

   precreate_path = _edi_aipanel_precreate_path_guess(command);
   if (precreate_path)
     precreate_quoted = _edi_aipanel_shell_quote(precreate_path);

   project_quoted = _edi_aipanel_shell_quote(edi_project_get() ?: ".");
   script = _edi_aipanel_command_script_build(project_quoted, command,
                                              precreate_quoted, tag);
   free(project_quoted);
   free(precreate_quoted);
   free(precreate_path);
   if (!script)
     return strdup("Failed to build command script.");

   script_quoted = _edi_aipanel_shell_quote(script);
   full_cmd = eina_slstr_printf("/bin/sh -lc %s", script_quoted);
   raw = edi_exe_response(full_cmd);
   free(script_quoted);
   free(script);

   if (!raw)
     return strdup("Failed to run command.");

   marker = strstr(raw, tag);
   while (marker)
     {
        last = marker;
        marker = strstr(marker + 1, tag);
     }

   if (last)
     {
        if (exit_code)
          *exit_code = atoi(last + strlen(tag));
        *last = '\0';
     }

   out = _edi_aipanel_text_clip(raw, EDI_AI_RUN_MAX_OUTPUT);
   free(raw);
   return out;
}

static char *
_edi_aipanel_prompt_normalize(const char *src)
{
   char *out;
   char *w;
   const char *r;

   if (!src)
     return strdup("");

   out = calloc(1, strlen(src) + 1);
   if (!out)
     return strdup("");

   w = out;
   for (r = src; *r; r++)
     {
        char c = *r;
        if (c == '\r' || c == '\n')
          c = ' ';

        *w++ = c;
     }
   *w = '\0';

   while (*out == ' ')
     memmove(out, out + 1, strlen(out));
   while (w > out && *(w - 1) == ' ')
     *--w = '\0';

   return out;
}

static void
_edi_aipanel_agent_text_apply(unsigned int row, const char *text)
{
   Elm_Code_Line *line;
   const char *start, *end;
   int len;

   if (!_edi_ai_code || !text || !row)
     return;

   start = text;
   end = strchr(start, '\n');
   if (!end)
     {
        line = elm_code_file_line_get(_edi_ai_code->file, row);
        if (!line)
          return;
        elm_code_line_text_set(line, start, strlen(start));
        elm_code_widget_line_refresh(_edi_ai_widget, line);
        return;
     }

   line = elm_code_file_line_get(_edi_ai_code->file, row);
   if (!line)
     return;

   len = end - start;
   elm_code_line_text_set(line, start, len);
   elm_code_widget_line_refresh(_edi_ai_widget, line);

   start = end + 1;
   while (start)
     {
        end = strchr(start, '\n');
        if (end)
          {
             len = end - start;
             elm_code_file_line_append(_edi_ai_code->file, start, len, NULL);
             start = end + 1;
          }
        else
          {
             elm_code_file_line_append(_edi_ai_code->file, start, strlen(start), NULL);
             break;
          }
     }
}

static void
_edi_aipanel_follow_tail(void)
{
   unsigned int lines;

   if (!_edi_ai_follow_tail || !_edi_ai_widget || !_edi_ai_code)
     return;

   lines = elm_code_file_lines_get(_edi_ai_code->file);
   if (lines == 0)
     return;

   elm_code_widget_cursor_position_set(_edi_ai_widget, lines, 1);
}

static void
_edi_aipanel_append_line(const char *line)
{
   if (!_edi_ai_code || !line)
     return;

   elm_code_file_line_append(_edi_ai_code->file, line, strlen(line), NULL);
   _edi_aipanel_follow_tail();
}

static void
_edi_aipanel_append_multiline(const char *text)
{
   const char *start;
   const char *end;

   if (!text || !text[0])
     return;

   start = text;
   while (start)
     {
        end = strchr(start, '\n');
        if (end)
          {
             _edi_aipanel_append_line(eina_slstr_printf("%.*s", (int)(end - start), start));
             start = end + 1;
          }
        else
          {
             if (*start)
               _edi_aipanel_append_line(start);
             break;
          }
     }
}

static void
_edi_aipanel_send_state_set(Eina_Bool busy)
{
   _edi_ai_busy = busy;
   elm_object_text_set(_edi_ai_button, busy ? _("Stop") : _("Send"));
   elm_object_disabled_set(_edi_ai_button, EINA_FALSE);
   if (_edi_ai_copy_button)
     elm_object_disabled_set(_edi_ai_copy_button, busy);
   elm_object_disabled_set(_edi_ai_entry, busy);
}

static void
_edi_aipanel_response_cb(const char *response, const char *error, void *data EINA_UNUSED)
{
   Elm_Code_Line *line;
   char *run_cmd = NULL;
   Eina_Bool apply_response = EINA_TRUE;

   if (error && error[0])
     {
        free(_edi_ai_last_response);
        _edi_ai_last_response = NULL;

        if (_edi_ai_stream_row)
          {
             const char *msg = eina_slstr_printf("Error: %s", error);
             line = elm_code_file_line_get(_edi_ai_code->file, _edi_ai_stream_row);
             if (line)
               {
                  elm_code_line_text_set(line, msg, strlen(msg));
                  elm_code_widget_line_refresh(_edi_ai_widget, line);
               }
          }
        else
          _edi_aipanel_append_line(eina_slstr_printf("%s %s", EDI_AI_TAG_ERROR, error));
     }
   else if (response && response[0])
     {
        run_cmd = _edi_aipanel_edits_enabled_get()
                  ? _edi_aipanel_response_command_extract(response)
                  : NULL;
        if (run_cmd && _edi_ai_agent_steps < _edi_aipanel_steps_max_get())
          {
	             const char *deny_reason = NULL;
	             char *output;
	             char *file_preview;
	             char *followup_prompt;
	             int exit_code;
	             Eina_Bool allowed;

             allowed = _edi_aipanel_command_allowed(run_cmd, &deny_reason);
             if (!allowed)
               {
                  if (_edi_ai_stream_row)
                    {
                       const char *msg = eina_slstr_printf("Error: %s", deny_reason);
                       line = elm_code_file_line_get(_edi_ai_code->file, _edi_ai_stream_row);
                       if (line)
                         {
                            elm_code_line_text_set(line, msg, strlen(msg));
                            elm_code_widget_line_refresh(_edi_ai_widget, line);
                         }
                    }
                  else
                    _edi_aipanel_append_line(eina_slstr_printf("Error: %s", deny_reason));
                  apply_response = EINA_FALSE;
                  free(run_cmd);
                  run_cmd = NULL;
               }
             else
               {
                  Eina_Bool inspect_cmd = _edi_aipanel_command_is_inspection(run_cmd);
                  Eina_Bool mutating_cmd = _edi_aipanel_command_is_mutating(run_cmd);

                  if (mutating_cmd &&
                      (!_edi_ai_agent_inspected_root || !_edi_ai_agent_inspected_all_files))
                    {
                       const char *msg = NULL;
                       if (!_edi_ai_agent_inspected_root && !_edi_ai_agent_inspected_all_files)
                         msg = "Error: Inspect repo root and run find . -type f before edits.";
                       else if (!_edi_ai_agent_inspected_root)
                         msg = "Error: Inspect repo root (pwd/ls) before edits.";
                       else
                         msg = "Error: Run find . -type f before edits.";

                       if (_edi_ai_stream_row)
                         {
                            line = elm_code_file_line_get(_edi_ai_code->file, _edi_ai_stream_row);
                            if (line)
                              {
                                 elm_code_line_text_set(line, msg, strlen(msg));
                                 elm_code_widget_line_refresh(_edi_ai_widget, line);
                              }
                         }
                       else
                         _edi_aipanel_append_line(msg);
                       apply_response = EINA_FALSE;
                       free(run_cmd);
                       run_cmd = NULL;
                    }
                  else
                    {
	                  _edi_ai_agent_steps++;
	                  output = _edi_aipanel_command_run(run_cmd, &exit_code);
	                  file_preview = _edi_aipanel_created_file_preview_get(run_cmd);
	                  free(_edi_ai_last_stage_cmd);
	                  _edi_ai_last_stage_cmd = strdup(run_cmd ?: "");
	                  free(_edi_ai_last_stage_output);
	                  _edi_ai_last_stage_output = strdup(output ?: "");
                  _edi_ai_last_stage_exit_code = exit_code;
                  _edi_ai_last_stage_index = _edi_ai_agent_steps;
                  _edi_ai_last_stage_valid = EINA_TRUE;
		                  if (inspect_cmd)
		                    _edi_aipanel_inspection_state_update(run_cmd, exit_code);

                  if (file_preview && file_preview[0])
                    {
                       char *file_summary;

                       _edi_aipanel_append_line("[run] File result (content hidden):");
                       file_summary = _edi_aipanel_file_preview_widget_summary_get(file_preview);
                       _edi_aipanel_append_multiline(file_summary ?: file_preview);
                       free(file_summary);
                    }
                  if (_edi_aipanel_file_preview_indicates_failure(file_preview))
                    {
                       Eina_Strbuf *combined;
                       char *combined_out;

                       combined = eina_strbuf_new();
                       eina_strbuf_append_printf(combined, "%s\n\nFile verification failed:\n%s",
                                                 output ?: "",
                                                 file_preview ?: "status: unknown");
                       combined_out = eina_strbuf_string_steal(combined);
                       eina_strbuf_free(combined);
                       free(output);
                       output = combined_out;
                       exit_code = 98;
                    }
                  followup_prompt = _edi_aipanel_prompt_build_followup(run_cmd, exit_code,
                                                                       output ?: "",
                                                                       file_preview ?: "");
	                  free(output);
	                  free(file_preview);
	                  _edi_ai_request = edi_agent_request_send(followup_prompt, _edi_aipanel_response_cb, NULL);
                  free(followup_prompt);
                  free(run_cmd);
                  run_cmd = NULL;

                  if (_edi_ai_request)
                    return;

                  if (_edi_ai_stream_row)
                    {
                       line = elm_code_file_line_get(_edi_ai_code->file, _edi_ai_stream_row);
                       if (line)
                         {
                            elm_code_line_text_set(line, "Error: Failed to continue agent request.",
                                                   strlen("Error: Failed to continue agent request."));
                            elm_code_widget_line_refresh(_edi_ai_widget, line);
                         }
                    }
                  else
                    _edi_aipanel_append_line("Error: Failed to continue agent request.");
                  apply_response = EINA_FALSE;
               }
               }
          }
        else if (run_cmd)
          {
             if (_edi_ai_stream_row)
               {
                  line = elm_code_file_line_get(_edi_ai_code->file, _edi_ai_stream_row);
                  if (line)
                    {
                       elm_code_line_text_set(line, "Error: AI edits step limit reached.",
                                              strlen("Error: AI edits step limit reached."));
                       elm_code_widget_line_refresh(_edi_ai_widget, line);
                    }
               }
             else
               _edi_aipanel_append_line("Error: AI edits step limit reached.");
             apply_response = EINA_FALSE;
             free(run_cmd);
             run_cmd = NULL;
          }

        free(_edi_ai_last_response);
        _edi_ai_last_response = apply_response ? strdup(response) : NULL;

        if (apply_response && _edi_ai_stream_row)
          {
             _edi_aipanel_agent_text_apply(_edi_ai_stream_row, response);
          }
     }
   else
     {
        if (_edi_ai_stream_row)
          {
             line = elm_code_file_line_get(_edi_ai_code->file, _edi_ai_stream_row);
             if (line)
               {
                  elm_code_line_text_set(line, "Error: Empty response from agent.",
                                         strlen("Error: Empty response from agent."));
                  elm_code_widget_line_refresh(_edi_ai_widget, line);
               }
          }
     }

   _edi_aipanel_follow_tail();
   _edi_ai_follow_tail = EINA_FALSE;
   _edi_ai_stream_row = 0;
   _edi_ai_request = NULL;
   _edi_ai_agent_steps = 0;
   _edi_ai_agent_inspected_root = EINA_FALSE;
   _edi_ai_agent_inspected_all_files = EINA_FALSE;
   free(_edi_ai_repo_inspection);
   _edi_ai_repo_inspection = NULL;
   free(_edi_ai_active_prompt);
   _edi_ai_active_prompt = NULL;
   if (_edi_ai_stream_buf)
     {
        eina_strbuf_free(_edi_ai_stream_buf);
        _edi_ai_stream_buf = NULL;
     }
   free(run_cmd);
   _edi_aipanel_send_state_set(EINA_FALSE);
}

static void
_edi_aipanel_send(Evas_Object *entry)
{
   const char *text_markup;
   char *text, *prompt, *agent_prompt;
   Eina_Bool edits_enabled;
   Eina_Bool continue_mode = EINA_FALSE;

   if (_edi_ai_busy)
     return;

   text_markup = elm_object_part_text_get(entry, NULL);
   text = elm_entry_markup_to_utf8(text_markup);
   if (!text)
     return;

   prompt = _edi_aipanel_prompt_normalize(text);
   if (!prompt || !prompt[0])
     {
        free(text);
        free(prompt);
        return;
     }

   edits_enabled = _edi_aipanel_edits_enabled_get();
   if (!edits_enabled &&
       (strstr(prompt, EDI_AI_RUN_OPEN) || strstr(prompt, EDI_AI_RUN_CLOSE)))
     {
        _edi_aipanel_append_line(eina_slstr_printf("Error: AI edits are disabled. "
                                                   "Enable \"AI Edits (beta)\" in "
                                                   "Project Settings to use %s tags.",
                                                   EDI_AI_RUN_OPEN));
        free(text);
        free(prompt);
        return;
     }

   _edi_ai_follow_tail = EINA_TRUE;
   _edi_aipanel_append_line(eina_slstr_printf("%s %s", EDI_AI_TAG_USER, prompt));
   _edi_aipanel_append_line("Working...");
   _edi_ai_stream_row = elm_code_file_lines_get(_edi_ai_code->file);
   if (_edi_ai_stream_buf)
     {
        eina_strbuf_free(_edi_ai_stream_buf);
        _edi_ai_stream_buf = NULL;
     }

   continue_mode = edits_enabled &&
                   _edi_ai_last_stage_valid &&
                   _edi_aipanel_prompt_is_continue_request(prompt);

   free(_edi_ai_active_prompt);
   if (edits_enabled)
     _edi_ai_active_prompt = continue_mode
                           ? strdup(_edi_ai_last_task_prompt ?: prompt)
                           : strdup(prompt);
   else
     _edi_ai_active_prompt = NULL;

   _edi_ai_agent_steps = continue_mode ? _edi_ai_last_stage_index : 0;
   if (edits_enabled && !continue_mode)
     _edi_aipanel_stage_memory_clear();
   if (edits_enabled && !continue_mode)
     _edi_ai_last_task_prompt = strdup(prompt);

   _edi_ai_agent_inspected_root = EINA_FALSE;
   _edi_ai_agent_inspected_all_files = EINA_FALSE;
   free(_edi_ai_repo_inspection);
   _edi_ai_repo_inspection = NULL;
   if (edits_enabled)
     {
        _edi_ai_repo_inspection = _edi_aipanel_repo_inspection_get();
        if (!_edi_ai_repo_inspection)
          {
             _edi_aipanel_append_line("Error: Mandatory repository inspection failed.");
             _edi_ai_follow_tail = EINA_FALSE;
             _edi_ai_stream_row = 0;
             _edi_ai_agent_steps = 0;
             free(_edi_ai_active_prompt);
             _edi_ai_active_prompt = NULL;
             free(text);
             free(prompt);
             return;
          }
        _edi_ai_agent_inspected_root = EINA_TRUE;
        _edi_ai_agent_inspected_all_files = EINA_TRUE;
     }
   if (edits_enabled && continue_mode)
     agent_prompt = _edi_aipanel_prompt_build_resume(prompt);
   else if (edits_enabled)
     agent_prompt = _edi_aipanel_prompt_build_initial(prompt);
   else
     agent_prompt = strdup(prompt);

   _edi_ai_request = edi_agent_request_send(agent_prompt, _edi_aipanel_response_cb, NULL);
   free(agent_prompt);
   if (!_edi_ai_request)
     {
        _edi_aipanel_append_line("Error: Agent is not configured. Check Settings -> AI.");
        _edi_ai_follow_tail = EINA_FALSE;
        _edi_ai_stream_row = 0;
        _edi_ai_agent_steps = 0;
        free(_edi_ai_active_prompt);
        _edi_ai_active_prompt = NULL;
        free(text);
        free(prompt);
        return;
     }

   elm_object_part_text_set(entry, NULL, "");
   _edi_aipanel_send_state_set(EINA_TRUE);
   free(text);
   free(prompt);
}

static void
_edi_aipanel_copy_clicked_cb(void *data EINA_UNUSED, Evas_Object *obj EINA_UNUSED,
                             void *event_info EINA_UNUSED)
{
   if (!_edi_ai_widget || !_edi_ai_last_response || !_edi_ai_last_response[0])
     return;

   elm_cnp_selection_set(_edi_ai_widget, ELM_SEL_TYPE_CLIPBOARD, ELM_SEL_FORMAT_TEXT,
                         _edi_ai_last_response, strlen(_edi_ai_last_response));
}

static void
_edi_aipanel_keypress_cb(void *data EINA_UNUSED, Evas *e EINA_UNUSED,
                         Evas_Object *obj, void *event_info)
{
   Evas_Event_Key_Down *event = event_info;

   if (!event || !event->key)
     return;

   if (!strcmp(event->key, "Return"))
     _edi_aipanel_send(obj);
}

static void
_edi_aipanel_button_clicked_cb(void *data EINA_UNUSED, Evas_Object *obj EINA_UNUSED,
                               void *event_info EINA_UNUSED)
{
   if (_edi_ai_busy)
     {
        if (_edi_ai_request)
          {
             edi_agent_request_cancel(_edi_ai_request);
             _edi_ai_request = NULL;
          }
        return;
     }

   _edi_aipanel_send(_edi_ai_entry);
}

static Eina_Bool
_edi_aipanel_config_changed_cb(void *data EINA_UNUSED, int type EINA_UNUSED,
                               void *event EINA_UNUSED)
{
   elm_code_widget_font_set(_edi_ai_widget, _edi_project_config->font.name, _edi_project_config->font.size);
   edi_theme_elm_code_set(_edi_ai_widget, _edi_project_config->gui.theme);
   edi_theme_elm_code_alpha_set(_edi_ai_widget);

   return ECORE_CALLBACK_RENEW;
}

void
edi_aipanel_add(Evas_Object *parent)
{
   Evas_Object *frame, *box, *hbox, *entry, *button, *copy_button;
   Elm_Code_Widget *widget;
   Elm_Code *code;

   frame = elm_frame_add(parent);
   elm_object_text_set(frame, _("AI Agent"));
   evas_object_size_hint_weight_set(frame, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
   evas_object_size_hint_align_set(frame, EVAS_HINT_FILL, EVAS_HINT_FILL);
   evas_object_show(frame);

   box = elm_box_add(parent);
   evas_object_size_hint_weight_set(box, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
   evas_object_size_hint_align_set(box, EVAS_HINT_FILL, EVAS_HINT_FILL);
   evas_object_show(box);

   hbox = elm_box_add(parent);
   elm_box_horizontal_set(hbox, EINA_TRUE);
   evas_object_size_hint_weight_set(hbox, EVAS_HINT_EXPAND, 0.0);
   evas_object_size_hint_align_set(hbox, EVAS_HINT_FILL, EVAS_HINT_FILL);
   evas_object_show(hbox);

   entry = elm_entry_add(parent);
   elm_entry_single_line_set(entry, EINA_TRUE);
   elm_entry_scrollable_set(entry, EINA_TRUE);
   elm_entry_editable_set(entry, EINA_TRUE);
   elm_object_part_text_set(entry, "guide", _("Ask your configured AI agent..."));
   evas_object_size_hint_weight_set(entry, EVAS_HINT_EXPAND, 0.0);
   evas_object_size_hint_align_set(entry, EVAS_HINT_FILL, EVAS_HINT_FILL);
   evas_object_event_callback_add(entry, EVAS_CALLBACK_KEY_DOWN, _edi_aipanel_keypress_cb, NULL);
   evas_object_show(entry);

   button = elm_button_add(parent);
   elm_object_text_set(button, _("Send"));
   evas_object_size_hint_weight_set(button, 0.05, EVAS_HINT_EXPAND);
   evas_object_size_hint_align_set(button, EVAS_HINT_FILL, EVAS_HINT_FILL);
   evas_object_smart_callback_add(button, "clicked", _edi_aipanel_button_clicked_cb, NULL);
   evas_object_show(button);

   copy_button = elm_button_add(parent);
   elm_object_text_set(copy_button, _("Copy"));
   evas_object_size_hint_weight_set(copy_button, 0.05, EVAS_HINT_EXPAND);
   evas_object_size_hint_align_set(copy_button, EVAS_HINT_FILL, EVAS_HINT_FILL);
   evas_object_smart_callback_add(copy_button, "clicked", _edi_aipanel_copy_clicked_cb, NULL);
   evas_object_show(copy_button);

   code = elm_code_create();
   widget = elm_code_widget_add(parent, code);
   edi_theme_elm_code_set(widget, _edi_project_config->gui.theme);
   elm_code_widget_font_set(widget, _edi_project_config->font.name, _edi_project_config->font.size);
   elm_code_widget_gravity_set(widget, 0.0, 0.0);
   elm_code_widget_policy_set(widget, ELM_SCROLLER_POLICY_AUTO, ELM_SCROLLER_POLICY_AUTO);
   elm_code_widget_editable_set(widget, EINA_FALSE);
   evas_object_size_hint_weight_set(widget, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
   evas_object_size_hint_align_set(widget, EVAS_HINT_FILL, EVAS_HINT_FILL);
   evas_object_show(widget);

   _edi_ai_code = code;
   _edi_ai_widget = widget;
   _edi_ai_entry = entry;
   _edi_ai_button = button;
   _edi_ai_copy_button = copy_button;

   _edi_aipanel_append_line("AI panel ready.");
   _edi_aipanel_append_line("Configure provider/auth in Settings -> AI.");
   if (_edi_aipanel_edits_enabled_get())
     _edi_aipanel_append_line("Agent can request local commands with <edi-run>...</edi-run>.");
   _edi_aipanel_append_line("");

   elm_box_pack_end(hbox, entry);
   elm_box_pack_end(hbox, button);
   elm_box_pack_end(hbox, copy_button);

   elm_box_pack_end(box, widget);
   elm_box_pack_end(box, hbox);

   elm_object_content_set(frame, box);
   elm_box_pack_end(parent, frame);

   ecore_event_handler_add(EDI_EVENT_CONFIG_CHANGED, _edi_aipanel_config_changed_cb, NULL);
}

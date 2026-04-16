#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <ctype.h>
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
static unsigned int _edi_ai_stream_row = 0;
static Eina_Strbuf *_edi_ai_stream_buf = NULL;
static Edi_Agent_Request *_edi_ai_request = NULL;
static char *_edi_ai_last_response = NULL;
static char *_edi_ai_active_prompt = NULL;
static unsigned int _edi_ai_agent_steps = 0;

#define EDI_AI_TAG_USER "[user]"
#define EDI_AI_TAG_AGENT "[agent]"
#define EDI_AI_TAG_ERROR "[error]"
#define EDI_AI_TAG_RUN "[run]"
#define EDI_AI_RUN_OPEN "<edi-run>"
#define EDI_AI_RUN_CLOSE "</edi-run>"
#define EDI_AI_RUN_MAX_STEPS 8
#define EDI_AI_RUN_MAX_OUTPUT 12000

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

static void
_edi_aipanel_append_multiline(const char *text, const char *prefix)
{
   const char *start;
   const char *end;
   Eina_Strbuf *line;

   if (!text || !text[0])
     return;

   start = text;
   while (start)
     {
        end = strchr(start, '\n');
        line = eina_strbuf_new();
        if (prefix && prefix[0])
          eina_strbuf_append_printf(line, "%s ", prefix);
        if (end)
          eina_strbuf_append_length(line, start, end - start);
        else
          eina_strbuf_append(line, start);
        _edi_aipanel_append_line(eina_strbuf_string_get(line));
        eina_strbuf_free(line);
        if (!end)
          break;
        start = end + 1;
     }
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
_edi_aipanel_prompt_build_initial(const char *user_prompt)
{
   char *state;
   Eina_Strbuf *buf;
   char *out;

   state = _edi_aipanel_repo_state_get();
   buf = eina_strbuf_new();
   eina_strbuf_append(buf, "You are editing this local repository.\n");
   eina_strbuf_append(buf, "Use UNIX commands and keep repo state awareness.\n");
   eina_strbuf_append(buf, "If you need a command run, reply with exactly ");
   eina_strbuf_append(buf, EDI_AI_RUN_OPEN);
   eina_strbuf_append(buf, "COMMAND");
   eina_strbuf_append(buf, EDI_AI_RUN_CLOSE);
   eina_strbuf_append(buf, " and no other text.\n");
   eina_strbuf_append(buf, "Run one command at a time.\n");
   eina_strbuf_append(buf, "Avoid destructive actions.\n\n");
   eina_strbuf_append(buf, "Current state:\n");
   eina_strbuf_append(buf, state);
   eina_strbuf_append(buf, "\n\nUser request:\n");
   eina_strbuf_append(buf, user_prompt);
   out = eina_strbuf_string_steal(buf);
   eina_strbuf_free(buf);
   free(state);
   return out;
}

static char *
_edi_aipanel_prompt_build_followup(const char *executed_cmd, int exit_code,
                                   const char *output)
{
   char *state;
   char *output_clip;
   Eina_Strbuf *buf;
   char *out;

   state = _edi_aipanel_repo_state_get();
   output_clip = _edi_aipanel_text_clip(output, EDI_AI_RUN_MAX_OUTPUT);

   buf = eina_strbuf_new();
   eina_strbuf_append(buf, "Continue solving the same user request.\n");
   eina_strbuf_append(buf, "If another command is needed, reply only with ");
   eina_strbuf_append(buf, EDI_AI_RUN_OPEN);
   eina_strbuf_append(buf, "COMMAND");
   eina_strbuf_append(buf, EDI_AI_RUN_CLOSE);
   eina_strbuf_append(buf, ".\n");
   eina_strbuf_append(buf, "Otherwise provide the final answer.\n\n");
   eina_strbuf_append_printf(buf, "Original user request:\n%s\n\n",
                             _edi_ai_active_prompt ?: "");
   eina_strbuf_append_printf(buf, "Executed command:\n%s\n\n", executed_cmd ?: "");
   eina_strbuf_append_printf(buf, "Exit code: %d\n\n", exit_code);
   eina_strbuf_append_printf(buf, "Command output:\n%s\n\n", output_clip ?: "");
   eina_strbuf_append_printf(buf, "Updated state:\n%s\n", state ?: "");

   out = eina_strbuf_string_steal(buf);
   eina_strbuf_free(buf);
   free(state);
   free(output_clip);
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
_edi_aipanel_command_allowed(const char *cmd, const char **reason)
{
   static const char *allowed[] =
   {
      "pwd", "ls", "find", "grep", "sed", "awk", "cat", "head", "tail", "wc",
      "cut", "sort", "uniq", "tr", "xargs", "mkdir", "touch", "cp", "mv",
      "git", "printf", "echo", "meson", "ninja", "make", "cmake", "sh", "set"
   };
   static const char *blocked[] =
   {
      "rm -rf", "sudo ", "git reset --hard", "git checkout --",
      "shutdown", "reboot", "mkfs", "dd if=", "sudo", "doas"
   };
   const char *p;
   char token[64];
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

   p = cmd;
   while (*p && isspace((unsigned char)*p))
     p++;

   i = 0;
   while (*p && !isspace((unsigned char)*p) && i < sizeof(token) - 1)
     token[i++] = *p++;
   token[i] = '\0';

   if (!token[0])
     {
        *reason = "Missing command token.";
        return EINA_FALSE;
     }

   for (i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++)
     {
        if (!strcmp(token, allowed[i]))
          return EINA_TRUE;
     }

   *reason = "Command not in allow list.";
   return EINA_FALSE;
}

static char *
_edi_aipanel_command_run(const char *command, int *exit_code)
{
   char *project_quoted;
   Eina_Strbuf *script;
   char *script_quoted;
   const char *full_cmd;
   char *raw;
   char *marker;
   char *last = NULL;
   char *out;
   const char *tag = "__EDI_EXIT_CODE__:";

   if (exit_code)
     *exit_code = -1;

   project_quoted = _edi_aipanel_shell_quote(edi_project_get() ?: ".");
   script = eina_strbuf_new();
   eina_strbuf_append_printf(script,
                             "cd %s && { %s; } 2>&1; "
                             "printf '\\n%s%%s\\n' \"$?\"",
                             project_quoted, command, tag);
   free(project_quoted);

   script_quoted = _edi_aipanel_shell_quote(eina_strbuf_string_get(script));
   full_cmd = eina_slstr_printf("/bin/sh -lc %s", script_quoted);
   raw = edi_exe_response(full_cmd);
   free(script_quoted);
   eina_strbuf_free(script);

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

   if (!_edi_ai_widget || !_edi_ai_code)
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

   if (error && error[0])
     {
        free(_edi_ai_last_response);
        _edi_ai_last_response = NULL;

        if (_edi_ai_stream_row)
          {
             const char *msg = eina_slstr_printf("%s %s", EDI_AI_TAG_ERROR, error);
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
        if (run_cmd && _edi_ai_agent_steps < EDI_AI_RUN_MAX_STEPS)
          {
             const char *deny_reason = NULL;
             char *output;
             char *followup_prompt;
             int exit_code;
             Eina_Bool allowed;

             allowed = _edi_aipanel_command_allowed(run_cmd, &deny_reason);
             if (!allowed)
               {
                  _edi_aipanel_append_line(eina_slstr_printf("%s %s",
                                         EDI_AI_TAG_ERROR, deny_reason));
                  free(run_cmd);
                  run_cmd = NULL;
               }
             else
               {
                  _edi_ai_agent_steps++;

                  if (_edi_ai_stream_row)
                    {
                       const char *msg = eina_slstr_printf("%s Executing command...", EDI_AI_TAG_AGENT);
                       line = elm_code_file_line_get(_edi_ai_code->file, _edi_ai_stream_row);
                       if (line)
                         {
                            elm_code_line_text_set(line, msg, strlen(msg));
                            elm_code_widget_line_refresh(_edi_ai_widget, line);
                         }
                    }

                  _edi_aipanel_append_line(eina_slstr_printf("%s $ %s", EDI_AI_TAG_RUN, run_cmd));
                  output = _edi_aipanel_command_run(run_cmd, &exit_code);
                  if (output && output[0])
                    _edi_aipanel_append_multiline(output, EDI_AI_TAG_RUN);
                  _edi_aipanel_append_line(eina_slstr_printf("%s exit=%d", EDI_AI_TAG_RUN, exit_code));
                  _edi_aipanel_append_line("");

                  followup_prompt = _edi_aipanel_prompt_build_followup(run_cmd, exit_code, output ?: "");
                  free(output);

                  _edi_aipanel_append_line(EDI_AI_TAG_AGENT);
                  _edi_aipanel_append_line("");
                  _edi_ai_stream_row = elm_code_file_lines_get(_edi_ai_code->file);
                  _edi_ai_request = edi_agent_request_send(followup_prompt, _edi_aipanel_response_cb, NULL);
                  free(followup_prompt);
                  free(run_cmd);
                  run_cmd = NULL;

                  if (_edi_ai_request)
                    return;

                  _edi_aipanel_append_line("[error] Failed to continue agent request.");
               }
          }
        else if (run_cmd)
          {
             _edi_aipanel_append_line("[error] AI edits step limit reached.");
             free(run_cmd);
             run_cmd = NULL;
          }

        free(_edi_ai_last_response);
        _edi_ai_last_response = strdup(response);

        if (_edi_ai_stream_row)
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
                  elm_code_line_text_set(line, "[error] Empty response from agent.",
                                         strlen("[error] Empty response from agent."));
                  elm_code_widget_line_refresh(_edi_ai_widget, line);
               }
          }
     }

   _edi_aipanel_follow_tail();
   _edi_aipanel_append_line("");
   _edi_ai_stream_row = 0;
   _edi_ai_request = NULL;
   _edi_ai_agent_steps = 0;
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
        _edi_aipanel_append_line(eina_slstr_printf("%s %s", EDI_AI_TAG_USER, text));
        _edi_aipanel_append_line(eina_slstr_printf("%s AI edits are disabled. "
                                                   "Enable \"AI Edits (beta)\" in "
                                                   "Project Settings to use %s tags.",
                                                   EDI_AI_TAG_ERROR, EDI_AI_RUN_OPEN));
        _edi_aipanel_append_line("");
        free(text);
        free(prompt);
        return;
     }

   _edi_aipanel_append_line(eina_slstr_printf("%s %s", EDI_AI_TAG_USER, text));
   _edi_aipanel_append_line(EDI_AI_TAG_AGENT);
   _edi_aipanel_append_line("");
   _edi_ai_stream_row = elm_code_file_lines_get(_edi_ai_code->file);
   if (_edi_ai_stream_buf)
     {
        eina_strbuf_free(_edi_ai_stream_buf);
        _edi_ai_stream_buf = NULL;
     }

   free(_edi_ai_active_prompt);
   _edi_ai_active_prompt = edits_enabled ? strdup(prompt) : NULL;
   _edi_ai_agent_steps = 0;
   if (edits_enabled)
     agent_prompt = _edi_aipanel_prompt_build_initial(prompt);
   else
     agent_prompt = strdup(prompt);

   _edi_ai_request = edi_agent_request_send(agent_prompt, _edi_aipanel_response_cb, NULL);
   free(agent_prompt);
   if (!_edi_ai_request)
     {
        _edi_aipanel_append_line("[error] Agent is not configured. Check Project Settings -> AI Agents.");
        _edi_aipanel_append_line("");
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
   elm_code_widget_gravity_set(widget, 0.0, 1.0);
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
   _edi_aipanel_append_line("Configure provider/auth in Settings -> Project -> AI Agents.");
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

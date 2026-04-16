#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <string.h>
#include <ctype.h>
#include <stdio.h>

#include <Ecore.h>
#include <Ecore_Con.h>

#include "edi_agent.h"
#include "edi_agent_parse.h"

#define EDI_AGENT_DEFAULT_TIMEOUT 30.0

struct _Edi_Agent_Request
{
   int kind;
   Ecore_Con_Url *url;
   char *provider;
   char *endpoint;
   char *model;
   char *payload;
   long payload_len;
   Eina_Bool stream;

   Eina_Strbuf *response_buf;
   Eina_Strbuf *sse_buf;

   Edi_Agent_Token_Cb token_cb;
   Edi_Agent_Response_Cb done_cb;
   void *cb_data;
};

struct _Edi_Agent_Models_Request
{
   int kind;
   Ecore_Con_Url *url;
   char *provider;
   Eina_Strbuf *response_buf;
   Edi_Agent_Models_Cb cb;
   void *cb_data;
};

enum
{
   EDI_AGENT_URL_KIND_REQUEST = 1,
   EDI_AGENT_URL_KIND_MODEL_LIST
};

static const Edi_Agent_Provider _edi_agent_providers[] =
{
   { "google_codex", "Google Codex", "https://generativelanguage.googleapis.com/v1beta/models", "gemini-2.5-flash" },
   { "local_http", "Local HTTP Agent", "http://127.0.0.1:11434/v1/responses", "qwen2.5-coder:latest" },
   { "microsoft_copilot", "Microsoft Copilot", "https://api.githubcopilot.com/v1/responses", "gpt-4.1-mini" },
   { "openai_compatible", "OpenAI Compatible", "https://api.openai.com/v1/responses", "gpt-4.1-mini" }
};

static const char *_edi_agent_models_google_codex[] =
{
   "gemini-2.0-flash",
   "gemini-2.0-flash-lite",
   "gemini-2.5-flash",
   "gemini-2.5-pro"
};

static const char *_edi_agent_models_local_http[] =
{
   "deepseek-r1:latest",
   "gemma3:latest",
   "llama3.2:latest",
   "qwen2.5-coder:latest"
};

static const char *_edi_agent_models_microsoft_copilot[] =
{
   "claude-3.5-sonnet",
   "claude-3.7-sonnet",
   "gemini-2.0-flash",
   "gpt-4.1",
   "gpt-4.1-mini",
   "o3-mini"
};

static const char *_edi_agent_models_openai_compatible[] =
{
   "gpt-4.1",
   "gpt-4.1-mini",
   "gpt-4.1-nano",
   "o3",
   "o4-mini"
};

static Ecore_Event_Handler *_url_data_hdl = NULL;
static Ecore_Event_Handler *_url_complete_hdl = NULL;
static Eina_Bool _url_inited = EINA_FALSE;
static char *_edi_agent_prompt_suffix = NULL;
static Eina_Bool _edi_agent_prompt_suffix_loaded = EINA_FALSE;

static char *
_edi_agent_file_read_all(const char *path)
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

static const char *
_edi_agent_prompt_suffix_get(void)
{
   if (_edi_agent_prompt_suffix_loaded)
     return _edi_agent_prompt_suffix ?: "";

   _edi_agent_prompt_suffix_loaded = EINA_TRUE;
   _edi_agent_prompt_suffix =
      _edi_agent_file_read_all(PACKAGE_DATA_DIR "/prompts/agent_prompt_suffix.txt");

   return _edi_agent_prompt_suffix ?: "";
}

static Edi_Agent_Model_List
_edi_agent_provider_models_for_id_get(const char *provider_id)
{
   Edi_Agent_Model_List list = {0};

   if (!provider_id || !provider_id[0] || !strcmp(provider_id, "google_codex"))
     {
        list.models = _edi_agent_models_google_codex;
        list.count = sizeof(_edi_agent_models_google_codex) / sizeof(_edi_agent_models_google_codex[0]);
     }
   else if (!strcmp(provider_id, "local_http"))
     {
        list.models = _edi_agent_models_local_http;
        list.count = sizeof(_edi_agent_models_local_http) / sizeof(_edi_agent_models_local_http[0]);
     }
   else if (!strcmp(provider_id, "microsoft_copilot"))
     {
        list.models = _edi_agent_models_microsoft_copilot;
        list.count = sizeof(_edi_agent_models_microsoft_copilot) / sizeof(_edi_agent_models_microsoft_copilot[0]);
     }
   else if (!strcmp(provider_id, "openai_compatible"))
     {
        list.models = _edi_agent_models_openai_compatible;
        list.count = sizeof(_edi_agent_models_openai_compatible) / sizeof(_edi_agent_models_openai_compatible[0]);
     }

   return list;
}

static int
_edi_agent_model_cmp_cb(const void *d1, const void *d2)
{
   const char *a = d1;
   const char *b = d2;

   return strcmp(a, b);
}

static Eina_Bool
_edi_agent_model_list_has(Eina_List *models, const char *model)
{
   Eina_List *l;
   char *item;

   EINA_LIST_FOREACH(models, l, item)
     if (!strcmp(item, model))
       return EINA_TRUE;

   return EINA_FALSE;
}

static Eina_Bool
_edi_agent_model_keep_for_provider(const char *provider_id, const char *model)
{
   if (!model || !model[0])
     return EINA_FALSE;

   if (!strcmp(model, "gpt-4") || !strncmp(model, "gpt-4-", 6))
     return EINA_FALSE;

   if (!strcmp(provider_id, "google_codex"))
     return !strncmp(model, "gemini", 6);

   if (!strcmp(provider_id, "openai_compatible"))
     return !strncmp(model, "gpt-", 4) || !strncmp(model, "o", 1);

   if (!strcmp(provider_id, "microsoft_copilot"))
     return !strncmp(model, "claude-", 7) || !strncmp(model, "gemini-", 7) ||
            !strncmp(model, "gpt-", 4) || !strncmp(model, "o", 1);

   return EINA_TRUE;
}

static char *
_edi_agent_json_read_string(const char **cursor)
{
   const char *p;
   Eina_Strbuf *buf;
   char *out;

   p = *cursor;
   if (!p || *p != '"')
     return NULL;
   p++;

   buf = eina_strbuf_new();
   while (*p)
     {
        if (*p == '\\' && *(p + 1))
          {
             p++;
             eina_strbuf_append_char(buf, *p);
             p++;
             continue;
          }
        if (*p == '"')
          break;
        eina_strbuf_append_char(buf, *p);
        p++;
     }

   if (*p == '"')
     p++;
   *cursor = p;
   out = eina_strbuf_string_steal(buf);
   eina_strbuf_free(buf);
   return out;
}

static void
_edi_agent_model_name_add(Eina_List **models, const char *provider_id, const char *raw)
{
   char *name;

   if (!raw || !raw[0])
     return;

   name = strdup(raw);
   if (!name)
     return;

   if (!strcmp(provider_id, "google_codex") && !strncmp(name, "models/", 7))
     memmove(name, name + 7, strlen(name + 7) + 1);

   if (!_edi_agent_model_keep_for_provider(provider_id, name) ||
       _edi_agent_model_list_has(*models, name))
     {
        free(name);
        return;
     }

   *models = eina_list_append(*models, name);
}

static Eina_Bool
_edi_agent_models_parse_json(const char *provider_id, const char *json,
                             char ***models_out, unsigned int *count_out)
{
   Eina_List *models = NULL;
   Eina_List *l;
   const char *p;
   char **out;
   char *name;
   unsigned int i, count;

   if (!json || !json[0])
     return EINA_FALSE;

   p = json;
   while ((p = strstr(p, "\"id\"")))
     {
        while (*p && *p != ':') p++;
        if (!*p) break;
        p++;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p != '"') continue;
        name = _edi_agent_json_read_string(&p);
        _edi_agent_model_name_add(&models, provider_id, name);
        free(name);
     }

   p = json;
   while ((p = strstr(p, "\"name\"")))
     {
        while (*p && *p != ':') p++;
        if (!*p) break;
        p++;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p != '"') continue;
        name = _edi_agent_json_read_string(&p);
        _edi_agent_model_name_add(&models, provider_id, name);
        free(name);
     }

   if (!models)
     return EINA_FALSE;

   models = eina_list_sort(models, 0, _edi_agent_model_cmp_cb);
   count = eina_list_count(models);
   out = calloc(count, sizeof(char *));
   if (!out)
     {
        EINA_LIST_FREE(models, name) free(name);
        return EINA_FALSE;
     }

   i = 0;
   EINA_LIST_FOREACH(models, l, name)
     out[i++] = name;
   eina_list_free(models);

   *models_out = out;
   *count_out = count;
   return EINA_TRUE;
}

static Eina_Bool
_edi_agent_models_clone(const char *provider_id, const char **models, unsigned int count,
                        char ***models_out, unsigned int *count_out)
{
   char **out;
   unsigned int i, used = 0;

   if (!models || count == 0)
     return EINA_FALSE;

   out = calloc(count, sizeof(char *));
   if (!out)
     return EINA_FALSE;

   for (i = 0; i < count; i++)
     {
        if (!_edi_agent_model_keep_for_provider(provider_id, models[i]))
          continue;
        out[used] = strdup(models[i]);
        if (!out[used])
          continue;
        used++;
     }

   if (!used)
     {
        for (i = 0; i < count; i++)
          free(out[i]);
        free(out);
        return EINA_FALSE;
     }

   *models_out = out;
   *count_out = used;
   return EINA_TRUE;
}

static char *
_edi_agent_json_escape(const char *src)
{
   Eina_Strbuf *buf;
   const char *c;
   char *out;

   if (!src) return strdup("");

   buf = eina_strbuf_new();
   for (c = src; *c; c++)
     {
        switch (*c)
          {
           case '\\': eina_strbuf_append(buf, "\\\\"); break;
           case '"': eina_strbuf_append(buf, "\\\""); break;
           case '\n': eina_strbuf_append(buf, "\\n"); break;
           case '\r': eina_strbuf_append(buf, "\\r"); break;
           case '\t': eina_strbuf_append(buf, "\\t"); break;
           default: eina_strbuf_append_char(buf, *c); break;
          }
     }

   out = eina_strbuf_string_steal(buf);
   eina_strbuf_free(buf);
   return out;
}

static char *
_edi_agent_config_value_clean(const char *src)
{
   char *out;
   char *start;
   char *end;
   char *tag;

   if (!src)
     return strdup("");

   out = strdup(src);
   if (!out)
     return strdup("");

   start = out;
   while (*start && isspace((unsigned char)*start))
     start++;

   tag = strchr(start, '<');
   if (tag)
     *tag = '\0';

   end = start + strlen(start);
   while (end > start && isspace((unsigned char)*(end - 1)))
     end--;
   *end = '\0';

   if (start != out)
     memmove(out, start, end - start + 1);

   return out;
}

void
edi_agent_models_free(const char **models, unsigned int count)
{
   unsigned int i;

   if (!models)
     return;

   for (i = 0; i < count; i++)
     free((char *)models[i]);
   free((char **)models);
}

static void
_edi_agent_request_free(Edi_Agent_Request *req)
{
   if (!req) return;
   if (req->url) ecore_con_url_free(req->url);
   if (req->response_buf) eina_strbuf_free(req->response_buf);
   if (req->sse_buf) eina_strbuf_free(req->sse_buf);
   free(req->provider);
   free(req->endpoint);
   free(req->model);
   free(req->payload);
   free(req);
}

static void
_edi_agent_models_request_free(Edi_Agent_Models_Request *req)
{
   if (!req) return;
   if (req->url) ecore_con_url_free(req->url);
   if (req->response_buf) eina_strbuf_free(req->response_buf);
   free(req->provider);
   free(req);
}

static char *
_edi_agent_models_url_build(const Edi_Project_Config *config, const char *provider_id,
                            const Edi_Agent_Provider *provider, const char *api_key)
{
   char *endpoint_clean;
   const char *endpoint;
   Eina_Strbuf *url;
   const char *marker;
   char *out;

   endpoint_clean = _edi_agent_config_value_clean(config ? config->agent.endpoint : NULL);
   endpoint = (endpoint_clean && endpoint_clean[0]) ? endpoint_clean : provider->default_endpoint;
   if (!endpoint || !endpoint[0])
     {
        free(endpoint_clean);
        return NULL;
     }

   url = eina_strbuf_new();
   eina_strbuf_append(url, endpoint);

   if (!strcmp(provider_id, "google_codex"))
     {
        marker = strstr(eina_strbuf_string_get(url), ":generateContent");
        if (marker)
          eina_strbuf_remove(url, marker - eina_strbuf_string_get(url), eina_strbuf_length_get(url));
        if (api_key && api_key[0])
          eina_strbuf_append_printf(url, "%ckey=%s",
                                    strstr(eina_strbuf_string_get(url), "?") ? '&' : '?',
                                    api_key);
     }
   else if (!strcmp(provider_id, "local_http"))
     {
        marker = strstr(eina_strbuf_string_get(url), "/v1/chat/completions");
        if (!marker)
          marker = strstr(eina_strbuf_string_get(url), "/chat/completions");
        if (!marker)
          marker = strstr(eina_strbuf_string_get(url), "/v1/responses");
        if (!marker)
          marker = strstr(eina_strbuf_string_get(url), "/responses");
        if (marker)
          eina_strbuf_remove(url, marker - eina_strbuf_string_get(url), eina_strbuf_length_get(url));
        if (eina_strbuf_length_get(url) == 0 ||
            eina_strbuf_string_get(url)[eina_strbuf_length_get(url) - 1] != '/')
          eina_strbuf_append(url, "/");
        eina_strbuf_append(url, "api/tags");
     }
   else
     {
        marker = strstr(eina_strbuf_string_get(url), "/chat/completions");
        if (!marker)
          marker = strstr(eina_strbuf_string_get(url), "/responses");
        if (marker)
          {
             eina_strbuf_remove(url, marker - eina_strbuf_string_get(url), eina_strbuf_length_get(url));
             eina_strbuf_append(url, "/models");
          }
        else if (!strstr(eina_strbuf_string_get(url), "/models"))
          {
             if (eina_strbuf_length_get(url) == 0 ||
                 eina_strbuf_string_get(url)[eina_strbuf_length_get(url) - 1] != '/')
               eina_strbuf_append(url, "/");
             eina_strbuf_append(url, "models");
          }
     }

   out = eina_strbuf_string_steal(url);
   eina_strbuf_free(url);
   free(endpoint_clean);
   return out;
}

static void
_edi_agent_emit_stream_tokens(Edi_Agent_Request *req, const char *text)
{
   const char *p, *start;
   int space_run;

   if (!req->token_cb || !text || !text[0]) return;

   p = text;
   while (*p)
     {
        start = p;
        space_run = !!isspace((unsigned char)*p);
        while (*p && (!!isspace((unsigned char)*p) == space_run))
          p++;
        req->token_cb(eina_slstr_printf("%.*s", (int)(p - start), start), req->cb_data);
     }
}

static void
_edi_agent_parse_sse(Edi_Agent_Request *req)
{
   const char *all;
   const char *line_start;
   const char *newline;

   if (!req->sse_buf || !req->token_cb)
     return;

   all = eina_strbuf_string_get(req->sse_buf);
   line_start = all;
   while ((newline = strchr(line_start, '\n')))
     {
        int len = newline - line_start;
        const char *line;

        while (len > 0 && (line_start[len - 1] == '\r'))
          len--;

        line = line_start;
        while (len > 0 && (*line == ' ' || *line == '\r'))
          {
             line++;
             len--;
          }

        if ((len > 5) && !strncmp(line, "data:", 5))
          {
             const char *payload = line + 5;
             int payload_len = len - 5;
             char *payload_copy;
             char *delta = NULL;

             while (payload_len > 0 && *payload == ' ')
               {
                  payload++;
                  payload_len--;
               }

             if ((payload_len == 6) && !strncmp(payload, "[DONE]", 6))
               goto next_line;

             payload_copy = strndup(payload, payload_len);
             if (!payload_copy)
               goto next_line;

             delta = edi_agent_response_parse_for_provider(req->provider, payload_copy);
             if (delta && delta[0] && strcmp(delta, payload_copy))
               {
                  _edi_agent_emit_stream_tokens(req, delta);
                  eina_strbuf_append(req->response_buf, delta);
               }
             free(payload_copy);
             free(delta);
          }
next_line:
        line_start = newline + 1;
     }

   if (line_start != all)
     eina_strbuf_remove(req->sse_buf, 0, line_start - all);
}

static Eina_Bool
_edi_agent_url_data_cb(void *data EINA_UNUSED, int type EINA_UNUSED, void *event)
{
   Ecore_Con_Event_Url_Data *ev = event;
   int *kind_ptr;
   Edi_Agent_Request *req;
   Edi_Agent_Models_Request *models_req;

   if (!ev || !ev->url_con) return ECORE_CALLBACK_RENEW;

   kind_ptr = ecore_con_url_data_get(ev->url_con);
   if (!kind_ptr) return ECORE_CALLBACK_RENEW;

   if (*kind_ptr == EDI_AGENT_URL_KIND_MODEL_LIST)
     {
        models_req = (Edi_Agent_Models_Request *)kind_ptr;
        if (!models_req->response_buf)
          models_req->response_buf = eina_strbuf_new();
        eina_strbuf_append_length(models_req->response_buf, (const char *)ev->data, ev->size);
        return ECORE_CALLBACK_RENEW;
     }

   req = (Edi_Agent_Request *)kind_ptr;
   if (req->stream && req->token_cb)
     {
        if (!req->sse_buf) req->sse_buf = eina_strbuf_new();
        eina_strbuf_append_length(req->sse_buf, (const char *)ev->data, ev->size);
        _edi_agent_parse_sse(req);
     }
   else
     {
        eina_strbuf_append_length(req->response_buf, (const char *)ev->data, ev->size);
     }

   return ECORE_CALLBACK_RENEW;
}

static Eina_Bool
_edi_agent_url_complete_cb(void *data EINA_UNUSED, int type EINA_UNUSED, void *event)
{
   Ecore_Con_Event_Url_Complete *ev = event;
   int *kind_ptr;
   Edi_Agent_Request *req;
   Edi_Agent_Models_Request *models_req;
   const char *raw;
   char *response = NULL;
   char *error = NULL;
   char **models = NULL;
   unsigned int model_count = 0;
   Edi_Agent_Model_List fallback;
   Eina_Bool from_remote = EINA_FALSE;

   if (!ev || !ev->url_con) return ECORE_CALLBACK_RENEW;

   kind_ptr = ecore_con_url_data_get(ev->url_con);
   if (!kind_ptr) return ECORE_CALLBACK_RENEW;

   if (*kind_ptr == EDI_AGENT_URL_KIND_MODEL_LIST)
     {
        models_req = (Edi_Agent_Models_Request *)kind_ptr;
        raw = models_req->response_buf ? eina_strbuf_string_get(models_req->response_buf) : "";

        if (ev->status < 400)
          from_remote = _edi_agent_models_parse_json(models_req->provider, raw, &models, &model_count);

        if (!from_remote)
          {
             fallback = _edi_agent_provider_models_for_id_get(models_req->provider);
             _edi_agent_models_clone(models_req->provider, fallback.models, fallback.count,
                                     &models, &model_count);
          }

        if (models_req->cb)
          {
             if (!from_remote && ev->status >= 400)
               error = edi_agent_error_parse_for_provider(models_req->provider, raw, ev->status);
             models_req->cb(models_req->provider, (const char **)models, model_count,
                            from_remote, error, models_req->cb_data);
          }
        else
          {
             edi_agent_models_free((const char **)models, model_count);
          }

        free(error);
        _edi_agent_models_request_free(models_req);
        return ECORE_CALLBACK_RENEW;
     }

   req = (Edi_Agent_Request *)kind_ptr;

   raw = eina_strbuf_string_get(req->response_buf);
   if (ev->status >= 400)
     {
        Eina_Strbuf *buf;
        char *parsed;

        parsed = edi_agent_error_parse_for_provider(req->provider, raw, ev->status);
        buf = eina_strbuf_new();
        eina_strbuf_append_printf(buf, "%s\n\nProvider: %s\nEndpoint: %s\nModel: %s\nPayload-Length: %ld\nPayload: %s",
                                  parsed ? parsed : "Request failed.",
                                  req->provider ? req->provider : "(none)",
                                  req->endpoint ? req->endpoint : "(none)",
                                  req->model ? req->model : "(none)",
                                  req->payload_len,
                                  req->payload ? req->payload : "(none)");
        error = eina_strbuf_string_steal(buf);
        eina_strbuf_free(buf);
        free(parsed);
     }
   else
     response = edi_agent_response_parse_for_provider(req->provider, raw);

   if (!response && !error)
     error = strdup("Empty response from agent.");

   if (req->done_cb)
     req->done_cb(response, error, req->cb_data);

   free(response);
   free(error);
   _edi_agent_request_free(req);
   return ECORE_CALLBACK_RENEW;
}

static void
_edi_agent_runtime_ensure(void)
{
   if (_url_inited) return;
   ecore_con_url_init();
   _url_data_hdl = ecore_event_handler_add(ECORE_CON_EVENT_URL_DATA, _edi_agent_url_data_cb, NULL);
   _url_complete_hdl = ecore_event_handler_add(ECORE_CON_EVENT_URL_COMPLETE, _edi_agent_url_complete_cb, NULL);
   _url_inited = EINA_TRUE;
}

const Edi_Agent_Provider *
edi_agent_providers_get(unsigned int *count)
{
   if (count)
     *count = sizeof(_edi_agent_providers) / sizeof(_edi_agent_providers[0]);
   return _edi_agent_providers;
}

const Edi_Agent_Provider *
edi_agent_provider_by_id(const char *id)
{
   unsigned int i, count;

   if (!id || !id[0]) return &_edi_agent_providers[0];
   count = sizeof(_edi_agent_providers) / sizeof(_edi_agent_providers[0]);
   for (i = 0; i < count; i++)
     if (!strcmp(_edi_agent_providers[i].id, id))
       return &_edi_agent_providers[i];
   return &_edi_agent_providers[0];
}

const Edi_Agent_Provider *
edi_agent_provider_current_get(void)
{
   return edi_agent_provider_by_id(_edi_project_config->agent.provider);
}

Edi_Agent_Model_List
edi_agent_provider_models_get(const char *provider_id)
{
   return _edi_agent_provider_models_for_id_get(provider_id);
}

Eina_Bool
edi_agent_provider_model_supported(const char *provider_id, const char *model)
{
   Edi_Agent_Model_List list;
   unsigned int i;

   if (!model || !model[0])
     return EINA_FALSE;

   list = _edi_agent_provider_models_for_id_get(provider_id);
   for (i = 0; i < list.count; i++)
     {
        if (!strcmp(list.models[i], model))
          return EINA_TRUE;
     }

   return _edi_agent_model_keep_for_provider(provider_id, model);
}

const char *
edi_agent_provider_model_default_get(const char *provider_id)
{
   const Edi_Agent_Provider *provider;

   provider = edi_agent_provider_by_id(provider_id);
   return provider->default_model;
}

Edi_Agent_Models_Request *
edi_agent_provider_models_fetch(const Edi_Project_Config *config, const char *provider_id,
                                Edi_Agent_Models_Cb cb, void *data)
{
   Edi_Agent_Models_Request *req;
   const Edi_Agent_Provider *provider;
   char *api_key_clean;
   const char *api_key = NULL;
   char *url;

   provider = edi_agent_provider_by_id(provider_id);
   _edi_agent_runtime_ensure();

   api_key_clean = _edi_agent_config_value_clean(config ? config->agent.api_key : NULL);
   if (api_key_clean && api_key_clean[0])
     api_key = api_key_clean;

   url = _edi_agent_models_url_build(config, provider->id, provider, api_key);
   if (!url)
     {
        free(api_key_clean);
        return NULL;
     }

   req = calloc(1, sizeof(*req));
   if (!req)
     {
        free(url);
        free(api_key_clean);
        return NULL;
     }

   req->kind = EDI_AGENT_URL_KIND_MODEL_LIST;
   req->provider = strdup(provider->id);
   req->cb = cb;
   req->cb_data = data;
   req->response_buf = eina_strbuf_new();

   req->url = ecore_con_url_new(url);
   if (!req->url)
     {
        _edi_agent_models_request_free(req);
        free(url);
        free(api_key_clean);
        return NULL;
     }

   ecore_con_url_data_set(req->url, req);
   ecore_con_url_timeout_set(req->url,
                             (config && config->agent.timeout_seconds > 0.0)
                             ? config->agent.timeout_seconds
                             : EDI_AGENT_DEFAULT_TIMEOUT);
   ecore_con_url_additional_header_add(req->url, "Accept", "application/json");

   if (strcmp(provider->id, "google_codex") && api_key)
     ecore_con_url_additional_header_add(req->url, "Authorization",
                                         eina_slstr_printf("Bearer %s", api_key));

   if (!ecore_con_url_get(req->url))
     {
        _edi_agent_models_request_free(req);
        req = NULL;
     }

   free(url);
   free(api_key_clean);
   return req;
}

Eina_Bool
edi_agent_provider_models_fetch_cancel(Edi_Agent_Models_Request *request)
{
   if (!request) return EINA_FALSE;
   if (!request->url) return EINA_FALSE;

   ecore_con_url_data_set(request->url, NULL);
   ecore_con_url_free(request->url);
   request->url = NULL;
   _edi_agent_models_request_free(request);
   return EINA_TRUE;
}

Eina_Bool
edi_agent_provider_configured_get(const Edi_Project_Config *config)
{
   const Edi_Agent_Provider *provider;

   if (!config || !config->agent.enabled) return EINA_FALSE;
   provider = edi_agent_provider_by_id(config->agent.provider);
   if (!strcmp(provider->id, "local_http"))
     return EINA_TRUE;
   return config->agent.api_key && config->agent.api_key[0];
}

char *
edi_agent_provider_validate(const Edi_Project_Config *config)
{
   const Edi_Agent_Provider *provider;
   char *err;

   if (!config)
     return edi_agent_provider_validate_for_provider(config, NULL, NULL, NULL);

   provider = edi_agent_provider_by_id(config->agent.provider);
   err = edi_agent_provider_validate_for_provider(config, provider->id,
                                                  provider->default_endpoint,
                                                  provider->default_model);
   if (err)
     return err;

   if (config->agent.model && config->agent.model[0] &&
       !edi_agent_provider_model_supported(provider->id, config->agent.model))
     {
        return strdup("Model does not match selected provider.");
     }

   return NULL;
}

Edi_Agent_Request *
edi_agent_request_send(const char *prompt, Edi_Agent_Response_Cb cb, void *data)
{
   return edi_agent_request_send_stream(prompt, NULL, cb, data);
}

Edi_Agent_Request *
edi_agent_request_send_stream(const char *prompt, Edi_Agent_Token_Cb token_cb,
                              Edi_Agent_Response_Cb done_cb, void *data)
{
   Edi_Agent_Request *req;
   const Edi_Agent_Provider *provider;
   const char *provider_id;
   const char *endpoint, *model, *api_key, *project_id;
   char *endpoint_clean, *model_clean, *api_key_clean, *project_id_clean;
   char *prompt_esc, *model_esc;
   const char *prompt_with_rules;
   Eina_Strbuf *url, *payload;
   char *validation_error;

   if (!prompt || !prompt[0] || !done_cb)
     return NULL;

   validation_error = edi_agent_provider_validate(_edi_project_config);
   if (validation_error)
     {
        done_cb(NULL, validation_error, data);
        free(validation_error);
        return NULL;
     }

   _edi_agent_runtime_ensure();

   provider = edi_agent_provider_current_get();
   endpoint_clean = _edi_agent_config_value_clean(_edi_project_config->agent.endpoint);
   model_clean = _edi_agent_config_value_clean(_edi_project_config->agent.model);
   api_key_clean = _edi_agent_config_value_clean(_edi_project_config->agent.api_key);
   project_id_clean = _edi_agent_config_value_clean(_edi_project_config->agent.project_id);

   endpoint = (endpoint_clean && endpoint_clean[0]) ? endpoint_clean : provider->default_endpoint;
   model = (model_clean && model_clean[0]) ? model_clean : provider->default_model;
   api_key = (api_key_clean && api_key_clean[0]) ? api_key_clean : NULL;
   project_id = (project_id_clean && project_id_clean[0]) ? project_id_clean : NULL;
   provider_id = provider->id;

   if (!strcmp(provider_id, "google_codex") && endpoint &&
      (strstr(endpoint, "api.openai.com/") ||
       strstr(endpoint, "/chat/completions") ||
       strstr(endpoint, "/responses")))
     provider_id = "openai_compatible";

   if (!model || !model[0] || !edi_agent_provider_model_supported(provider_id, model))
     model = edi_agent_provider_model_default_get(provider_id);

   req = calloc(1, sizeof(*req));
   req->kind = EDI_AGENT_URL_KIND_REQUEST;
   req->provider = strdup(provider_id);
   req->endpoint = strdup(endpoint ? endpoint : "");
   req->model = strdup(model ? model : "");
   req->token_cb = token_cb;
   req->done_cb = done_cb;
   req->cb_data = data;
   req->stream = (token_cb && strcmp(provider_id, "google_codex"));
   req->response_buf = eina_strbuf_new();

   url = eina_strbuf_new();
   payload = eina_strbuf_new();
   prompt_with_rules = eina_slstr_printf("%s%s", prompt,
                                         _edi_agent_prompt_suffix_get());
   prompt_esc = _edi_agent_json_escape(prompt_with_rules);
   model_esc = _edi_agent_json_escape(model);

   if (!strcmp(provider_id, "google_codex"))
     {
        eina_strbuf_append(url, endpoint);
        if (!strstr(endpoint, ":generateContent"))
          eina_strbuf_append_printf(url, "/%s:generateContent", model_esc);
        if (api_key)
          eina_strbuf_append_printf(url, "%ckey=%s",
                                    strstr(eina_strbuf_string_get(url), "?") ? '&' : '?',
                                    api_key);

        eina_strbuf_append_printf(payload,
                                  "{\"contents\":[{\"parts\":[{\"text\":\"%s\"}]}]}",
                                  prompt_esc);
     }
   else
     {
        const char *marker;

        eina_strbuf_append(url, endpoint);
        marker = strstr(eina_strbuf_string_get(url), "/v1/chat/completions");
        if (marker)
          {
             eina_strbuf_remove(url, marker - eina_strbuf_string_get(url), eina_strbuf_length_get(url));
             eina_strbuf_append(url, "/v1/responses");
          }
        else
          {
             marker = strstr(eina_strbuf_string_get(url), "/chat/completions");
             if (marker)
               {
                  eina_strbuf_remove(url, marker - eina_strbuf_string_get(url), eina_strbuf_length_get(url));
                  eina_strbuf_append(url, "/responses");
               }
          }
        eina_strbuf_append_printf(payload,
                                  "{\"model\":\"%s\",\"stream\":%s,\"input\":\"%s\"}",
                                  model_esc, req->stream ? "true" : "false", prompt_esc);
     }

   req->url = ecore_con_url_new(eina_strbuf_string_get(url));
   if (!req->url)
     {
        _edi_agent_request_free(req);
        req = NULL;
        goto done;
     }

   ecore_con_url_data_set(req->url, req);
   ecore_con_url_timeout_set(req->url,
                             (_edi_project_config->agent.timeout_seconds > 0.0)
                             ? _edi_project_config->agent.timeout_seconds
                             : EDI_AGENT_DEFAULT_TIMEOUT);

   if (strcmp(provider_id, "google_codex") && api_key)
     {
        ecore_con_url_additional_header_add(req->url, "Authorization",
                                            eina_slstr_printf("Bearer %s", api_key));
     }
   else if (!strcmp(provider_id, "google_codex") && project_id)
     {
        ecore_con_url_additional_header_add(req->url, "x-goog-user-project",
                                            project_id);
     }

   req->payload = strdup(eina_strbuf_string_get(payload));
   if (!req->payload)
     {
        done_cb(NULL, "Failed to prepare request payload.", data);
        _edi_agent_request_free(req);
        req = NULL;
     }
   else if (!ecore_con_url_post(req->url, req->payload,
                                strlen(req->payload), "application/json"))
     {
        done_cb(NULL, "Failed to start request.", data);
        _edi_agent_request_free(req);
        req = NULL;
     }
   else
     {
        req->payload_len = strlen(req->payload);
     }

done:
   free(prompt_esc);
   free(model_esc);
   free(endpoint_clean);
   free(model_clean);
   free(api_key_clean);
   free(project_id_clean);
   eina_strbuf_free(url);
   eina_strbuf_free(payload);
   return req;
}

Eina_Bool
edi_agent_request_cancel(Edi_Agent_Request *request)
{
   if (!request) return EINA_FALSE;
   if (!request->url) return EINA_FALSE;

   ecore_con_url_data_set(request->url, NULL);
   ecore_con_url_free(request->url);
   request->url = NULL;
   if (request->done_cb)
     request->done_cb(NULL, "Request cancelled.", request->cb_data);
   _edi_agent_request_free(request);
   return EINA_TRUE;
}

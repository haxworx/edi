#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <string.h>

#include <Eina.h>

#include "edi_agent_parse.h"

static char *
_edi_agent_json_unescape(const char *src)
{
   Eina_Strbuf *buf;
   const char *c;
   char *out;

   if (!src) return strdup("");

   buf = eina_strbuf_new();
   for (c = src; *c; c++)
     {
        if (*c == '\\' && *(c + 1))
          {
             c++;
             switch (*c)
               {
                case 'n': eina_strbuf_append_char(buf, '\n'); break;
                case 'r': eina_strbuf_append_char(buf, '\r'); break;
                case 't': eina_strbuf_append_char(buf, '\t'); break;
                case '\\': eina_strbuf_append_char(buf, '\\'); break;
                case '"': eina_strbuf_append_char(buf, '"'); break;
                default:
                   eina_strbuf_append_char(buf, '\\');
                   eina_strbuf_append_char(buf, *c);
               }
          }
        else
          eina_strbuf_append_char(buf, *c);
     }

   out = eina_strbuf_string_steal(buf);
   eina_strbuf_free(buf);
   return out;
}

static char *
_edi_agent_json_first_string_value_get(const char *json, const char *key)
{
   const char *p, *q;
   Eina_Strbuf *buf;
   char *escaped, *out;

   if (!json || !key) return NULL;

   p = json;
   while ((p = strstr(p, key)))
     {
        q = p + strlen(key);
        while (*q && *q != ':') q++;
        if (!*q) break;
        q++;
        while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
        if (*q != '"')
          {
             p = q;
             continue;
          }

        q++;
        buf = eina_strbuf_new();
        while (*q)
          {
             if (*q == '"' && *(q - 1) != '\\')
               break;
             eina_strbuf_append_char(buf, *q);
             q++;
          }
        if (!*q)
          {
             eina_strbuf_free(buf);
             return NULL;
          }

        escaped = eina_strbuf_string_steal(buf);
        eina_strbuf_free(buf);
        out = _edi_agent_json_unescape(escaped);
        free(escaped);
        return out;
     }

   return NULL;
}

char *
edi_agent_error_parse_for_provider(const char *provider, const char *json, int http_code)
{
   char *msg = NULL, *status = NULL, *type = NULL, *code = NULL;
   Eina_Strbuf *buf;
   char *out;

   if (!json || !json[0])
     return strdup(eina_slstr_printf("%s request failed (HTTP %d).",
                                     provider ? provider : "Agent", http_code));

   msg = _edi_agent_json_first_string_value_get(json, "\"message\"");
   status = _edi_agent_json_first_string_value_get(json, "\"status\"");
   type = _edi_agent_json_first_string_value_get(json, "\"type\"");
   code = _edi_agent_json_first_string_value_get(json, "\"code\"");

   if (!msg || !msg[0])
     {
        free(msg);
        free(status);
        free(type);
        free(code);
        return strdup(eina_slstr_printf("%s request failed (HTTP %d).",
                                        provider ? provider : "Agent", http_code));
     }

   buf = eina_strbuf_new();
   eina_strbuf_append_printf(buf, "%s request failed (HTTP %d): %s",
                             provider ? provider : "Agent", http_code, msg);
   if (status && status[0])
     eina_strbuf_append_printf(buf, " [status=%s]", status);
   if (type && type[0])
     eina_strbuf_append_printf(buf, " [type=%s]", type);
   if (code && code[0])
     eina_strbuf_append_printf(buf, " [code=%s]", code);

   out = eina_strbuf_string_steal(buf);
   eina_strbuf_free(buf);
   free(msg);
   free(status);
   free(type);
   free(code);
   return out;
}

char *
edi_agent_response_parse_for_provider(const char *provider, const char *json)
{
   char *text = NULL;

   if (!json || !json[0])
     return NULL;

   if (provider && !strcmp(provider, "google_codex"))
     text = _edi_agent_json_first_string_value_get(json, "\"text\"");
   else
     {
        text = _edi_agent_json_first_string_value_get(json, "\"delta\"");
        if (!text || !text[0])
          {
             free(text);
             text = _edi_agent_json_first_string_value_get(json, "\"output_text\"");
          }
        if (!text || !text[0])
          {
             free(text);
             text = _edi_agent_json_first_string_value_get(json, "\"text\"");
          }
        if (!text || !text[0])
          {
             free(text);
             text = _edi_agent_json_first_string_value_get(json, "\"content\"");
          }
     }

   if (text && text[0])
     return text;

   free(text);
   return strdup(json);
}

char *
edi_agent_provider_validate_for_provider(const Edi_Project_Config *config,
                                         const char *provider_id,
                                         const char *default_endpoint,
                                         const char *default_model)
{
   if (!config) return strdup("Missing configuration.");
   if (!config->agent.enabled) return strdup("Agent support is disabled.");

   if ((!config->agent.endpoint || !config->agent.endpoint[0]) &&
       (!default_endpoint || !default_endpoint[0]))
     return strdup("Endpoint is required.");
   if ((!config->agent.model || !config->agent.model[0]) &&
       (!default_model || !default_model[0]))
     return strdup("Model is required.");
   if ((!provider_id || strcmp(provider_id, "local_http")) &&
       (!config->agent.api_key || !config->agent.api_key[0]))
     return strdup("API key is required for this provider.");

   return NULL;
}

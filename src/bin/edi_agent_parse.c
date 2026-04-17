#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <string.h>

#include <Eina.h>

#include "edi_agent_parse.h"

static int
_edi_agent_json_hex_to_int(char c)
{
   if (c >= '0' && c <= '9')
     return c - '0';
   if (c >= 'a' && c <= 'f')
     return c - 'a' + 10;
   if (c >= 'A' && c <= 'F')
     return c - 'A' + 10;
   return -1;
}

static Eina_Bool
_edi_agent_json_u16_read(const char *src, unsigned int *value)
{
   int n0, n1, n2, n3;

   if (!src || !value)
     return EINA_FALSE;

   n0 = _edi_agent_json_hex_to_int(src[0]);
   n1 = _edi_agent_json_hex_to_int(src[1]);
   n2 = _edi_agent_json_hex_to_int(src[2]);
   n3 = _edi_agent_json_hex_to_int(src[3]);
   if (n0 < 0 || n1 < 0 || n2 < 0 || n3 < 0)
     return EINA_FALSE;

   *value = ((unsigned int)n0 << 12) |
            ((unsigned int)n1 << 8) |
            ((unsigned int)n2 << 4) |
            (unsigned int)n3;
   return EINA_TRUE;
}

static void
_edi_agent_json_utf8_append(Eina_Strbuf *buf, unsigned int cp)
{
   if (!buf)
     return;

   if (cp <= 0x7F)
     {
        eina_strbuf_append_char(buf, (char) cp);
     }
   else if (cp <= 0x7FF)
     {
        eina_strbuf_append_char(buf, (char)(0xC0 | ((cp >> 6) & 0x1F)));
        eina_strbuf_append_char(buf, (char)(0x80 | (cp & 0x3F)));
     }
   else if (cp <= 0xFFFF)
     {
        eina_strbuf_append_char(buf, (char)(0xE0 | ((cp >> 12) & 0x0F)));
        eina_strbuf_append_char(buf, (char)(0x80 | ((cp >> 6) & 0x3F)));
        eina_strbuf_append_char(buf, (char)(0x80 | (cp & 0x3F)));
     }
   else if (cp <= 0x10FFFF)
     {
        eina_strbuf_append_char(buf, (char)(0xF0 | ((cp >> 18) & 0x07)));
        eina_strbuf_append_char(buf, (char)(0x80 | ((cp >> 12) & 0x3F)));
        eina_strbuf_append_char(buf, (char)(0x80 | ((cp >> 6) & 0x3F)));
        eina_strbuf_append_char(buf, (char)(0x80 | (cp & 0x3F)));
     }
   else
     eina_strbuf_append_char(buf, '?');
}

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
               case 'u':
                 {
                    unsigned int hi;

                    if (!_edi_agent_json_u16_read(c + 1, &hi))
                      {
                         eina_strbuf_append(buf, "\\u");
                         break;
                      }

                    if (hi >= 0xD800 && hi <= 0xDBFF &&
                        c[5] == '\\' && c[6] == 'u')
                      {
                         unsigned int lo;

                         if (_edi_agent_json_u16_read(c + 7, &lo) &&
                             lo >= 0xDC00 && lo <= 0xDFFF)
                           {
                              unsigned int cp;

                              cp = 0x10000 + (((hi - 0xD800) << 10) | (lo - 0xDC00));
                              _edi_agent_json_utf8_append(buf, cp);
                              c += 10;
                              break;
                           }
                      }

                    _edi_agent_json_utf8_append(buf, hi);
                    c += 4;
                    break;
                 }
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

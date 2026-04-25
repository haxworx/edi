#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdlib.h>
#include <string.h>

#include "Edi.h"
#include "edi_aipanel.h"
#include "edi_agent.h"
#include "edi_theme.h"
#include "edi_config.h"

#include "edi_private.h"

typedef struct
{
   Evas_Object *widget;
   Evas_Object *button;
   Evas_Object *copy_button;
   Evas_Object *entry;
   Elm_Code *code;
   Eina_Bool busy;
   Eina_Bool follow_tail;
   unsigned int stream_row;
   Edi_Agent_Request *request;
   char *last_response;
} Edi_Ai_Panel_State;

static Edi_Ai_Panel_State _edi_ai_panel =
{
   .follow_tail = EINA_TRUE
};

#define EDI_AI_TAG_USER "[user]"

static void _edi_aipanel_append_line(const char *line);

static void
_edi_aipanel_request_state_clear(void)
{
   _edi_ai_panel.follow_tail = EINA_FALSE;
   _edi_ai_panel.stream_row = 0;
   _edi_ai_panel.request = NULL;
}

static char *
_edi_aipanel_widget_text_normalize(const char *text)
{
   Eina_Strbuf *buf;
   const unsigned char *p;
   char *out;

   if (!text)
     return strdup("");

   buf = eina_strbuf_new();
   if (!buf)
     return strdup(text);

   for (p = (const unsigned char *)text; *p; p++)
     {
        if (*p == '\r')
          {
             if (*(p + 1) != '\n')
               eina_strbuf_append_char(buf, '\n');
             continue;
          }

        if (*p < 0x20 && *p != '\n' && *p != '\t')
          {
             eina_strbuf_append_char(buf, ' ');
             continue;
          }

        if (*p == 0x7f)
          {
             eina_strbuf_append_char(buf, ' ');
             continue;
          }

        eina_strbuf_append_char(buf, (char)*p);
     }

   out = eina_strbuf_string_steal(buf);
   eina_strbuf_free(buf);
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
   char *clean;
   const char *start;
   const char *end;
   int len;

   if (!_edi_ai_panel.code || !text || !row)
     return;

   clean = _edi_aipanel_widget_text_normalize(text);
   start = clean;
   end = strchr(start, '\n');
   if (!end)
     {
        line = elm_code_file_line_get(_edi_ai_panel.code->file, row);
        if (!line)
          {
             free(clean);
             return;
          }
        elm_code_line_text_set(line, start, strlen(start));
        elm_code_widget_line_refresh(_edi_ai_panel.widget, line);
        free(clean);
        return;
     }

   line = elm_code_file_line_get(_edi_ai_panel.code->file, row);
   if (!line)
     {
        free(clean);
        return;
     }

   len = end - start;
   elm_code_line_text_set(line, start, len);
   elm_code_widget_line_refresh(_edi_ai_panel.widget, line);

   start = end + 1;
   while (start)
     {
        end = strchr(start, '\n');
        if (end)
          {
             len = end - start;
             elm_code_file_line_append(_edi_ai_panel.code->file, start, len, NULL);
             start = end + 1;
          }
        else
          {
             if (*start)
               elm_code_file_line_append(_edi_ai_panel.code->file, start, strlen(start), NULL);
             break;
          }
     }

   free(clean);
}

static void
_edi_aipanel_follow_tail(void)
{
   unsigned int lines;

   if (!_edi_ai_panel.follow_tail || !_edi_ai_panel.widget || !_edi_ai_panel.code)
     return;

   lines = elm_code_file_lines_get(_edi_ai_panel.code->file);
   if (lines == 0)
     return;

   elm_code_widget_cursor_position_set(_edi_ai_panel.widget, lines, 1);
}

static void
_edi_aipanel_append_line(const char *line)
{
   char *clean;

   if (!_edi_ai_panel.code || !line)
     return;

   clean = _edi_aipanel_widget_text_normalize(line);
   elm_code_file_line_append(_edi_ai_panel.code->file, clean, strlen(clean), NULL);
   free(clean);
   _edi_aipanel_follow_tail();
}

static void
_edi_aipanel_append_multiline(const char *text)
{
   char *clean;
   const char *start;
   const char *end;

   if (!text || !text[0])
     return;

   clean = _edi_aipanel_widget_text_normalize(text);
   start = clean;
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

   free(clean);
}

static void
_edi_aipanel_stream_error_set(const char *error)
{
   Elm_Code_Line *line;
   char *msg;

   if (!_edi_ai_panel.stream_row || !_edi_ai_panel.code || !_edi_ai_panel.widget)
     return;

   msg = _edi_aipanel_widget_text_normalize(error ?: "");
   line = elm_code_file_line_get(_edi_ai_panel.code->file, _edi_ai_panel.stream_row);
   if (line)
     {
        const char *first;
        const char *newline;
        int len;

        first = msg;
        newline = strchr(first, '\n');
        if (newline)
          len = newline - first;
        else
          len = strlen(first);

        elm_code_line_text_set(line, first, len);
        elm_code_widget_line_refresh(_edi_ai_panel.widget, line);

        if (newline && *(newline + 1))
          _edi_aipanel_append_multiline(newline + 1);
     }
   free(msg);
}

static void
_edi_aipanel_send_state_set(Eina_Bool busy)
{
   _edi_ai_panel.busy = busy;
   if (_edi_ai_panel.button)
     {
        elm_object_text_set(_edi_ai_panel.button, busy ? _("Stop") : _("Send"));
        elm_object_disabled_set(_edi_ai_panel.button, EINA_FALSE);
     }
   if (_edi_ai_panel.copy_button)
     elm_object_disabled_set(_edi_ai_panel.copy_button, busy);
   if (_edi_ai_panel.entry)
     elm_object_disabled_set(_edi_ai_panel.entry, busy);
}

static void
_edi_aipanel_obj_del_cb(void *data EINA_UNUSED, Evas *e EINA_UNUSED,
                        Evas_Object *obj, void *event_info EINA_UNUSED)
{
   if (obj == _edi_ai_panel.widget)
     _edi_ai_panel.widget = NULL;
   else if (obj == _edi_ai_panel.button)
     _edi_ai_panel.button = NULL;
   else if (obj == _edi_ai_panel.copy_button)
     _edi_ai_panel.copy_button = NULL;
   else if (obj == _edi_ai_panel.entry)
     _edi_ai_panel.entry = NULL;

   if (_edi_ai_panel.request)
     {
        edi_agent_request_cancel(_edi_ai_panel.request);
        _edi_ai_panel.request = NULL;
     }
}

static void
_edi_aipanel_response_cb(const char *response, const char *error, void *data EINA_UNUSED)
{
   Eina_Bool ui_ready = (_edi_ai_panel.code && _edi_ai_panel.widget);

   if (!ui_ready)
     {
        _edi_aipanel_request_state_clear();
        _edi_aipanel_send_state_set(EINA_FALSE);
        return;
     }

   if (error && error[0])
     {
        free(_edi_ai_panel.last_response);
        _edi_ai_panel.last_response = NULL;

        if (_edi_ai_panel.stream_row)
          _edi_aipanel_stream_error_set(eina_slstr_printf("Error: %s", error));
        else
          _edi_aipanel_append_line(eina_slstr_printf("Error: %s", error));
     }
   else if (response && response[0])
     {
        free(_edi_ai_panel.last_response);
        _edi_ai_panel.last_response = strdup(response);

        if (_edi_ai_panel.stream_row)
          _edi_aipanel_agent_text_apply(_edi_ai_panel.stream_row, response);
        else
          _edi_aipanel_append_multiline(response);
     }
   else
     {
        if (_edi_ai_panel.stream_row)
          _edi_aipanel_stream_error_set("Error: Empty response from agent.");
        else
          _edi_aipanel_append_line("Error: Empty response from agent.");
     }

   _edi_aipanel_follow_tail();
   _edi_aipanel_request_state_clear();
   _edi_aipanel_send_state_set(EINA_FALSE);
}

static void
_edi_aipanel_send(Evas_Object *entry)
{
   const char *text_markup;
   char *text;
   char *prompt;

   if (_edi_ai_panel.busy)
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

   _edi_ai_panel.follow_tail = EINA_TRUE;
   _edi_aipanel_append_line(eina_slstr_printf("%s %s", EDI_AI_TAG_USER, prompt));
   _edi_aipanel_append_line("Working...");
   _edi_ai_panel.stream_row = elm_code_file_lines_get(_edi_ai_panel.code->file);

   _edi_ai_panel.request = edi_agent_request_send(prompt, _edi_aipanel_response_cb, NULL);
   if (!_edi_ai_panel.request)
     {
        _edi_aipanel_append_line("Error: Agent is not configured. Check Settings -> AI.");
        _edi_ai_panel.follow_tail = EINA_FALSE;
        _edi_ai_panel.stream_row = 0;
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
   if (!_edi_ai_panel.widget || !_edi_ai_panel.last_response || !_edi_ai_panel.last_response[0])
     return;

   elm_cnp_selection_set(_edi_ai_panel.widget, ELM_SEL_TYPE_CLIPBOARD, ELM_SEL_FORMAT_TEXT,
                         _edi_ai_panel.last_response, strlen(_edi_ai_panel.last_response));
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
   if (_edi_ai_panel.busy)
     {
        if (_edi_ai_panel.request)
          {
             edi_agent_request_cancel(_edi_ai_panel.request);
             _edi_ai_panel.request = NULL;
          }
        return;
     }

   _edi_aipanel_send(_edi_ai_panel.entry);
}

static Eina_Bool
_edi_aipanel_config_changed_cb(void *data EINA_UNUSED, int type EINA_UNUSED,
                               void *event EINA_UNUSED)
{
   if (!_edi_ai_panel.widget || !_edi_project_config)
     return ECORE_CALLBACK_RENEW;

   elm_code_widget_font_set(_edi_ai_panel.widget, _edi_project_config->font.name,
                            _edi_project_config->font.size);
   edi_theme_elm_code_set(_edi_ai_panel.widget, _edi_project_config->gui.theme);
   edi_theme_elm_code_alpha_set(_edi_ai_panel.widget);

   return ECORE_CALLBACK_RENEW;
}

void
edi_aipanel_add(Evas_Object *parent)
{
   Evas_Object *frame;
   Evas_Object *box;
   Evas_Object *hbox;
   Evas_Object *entry;
   Evas_Object *button;
   Evas_Object *copy_button;
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
   evas_object_event_callback_add(entry, EVAS_CALLBACK_KEY_DOWN,
                                  _edi_aipanel_keypress_cb, NULL);
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
   elm_code_widget_font_set(widget, _edi_project_config->font.name,
                            _edi_project_config->font.size);
   elm_code_widget_gravity_set(widget, 0.0, 0.0);
   elm_code_widget_policy_set(widget, ELM_SCROLLER_POLICY_AUTO,
                              ELM_SCROLLER_POLICY_AUTO);
   elm_code_widget_editable_set(widget, EINA_FALSE);
   evas_object_size_hint_weight_set(widget, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
   evas_object_size_hint_align_set(widget, EVAS_HINT_FILL, EVAS_HINT_FILL);
   evas_object_show(widget);

   _edi_ai_panel.code = code;
   _edi_ai_panel.widget = widget;
   _edi_ai_panel.entry = entry;
   _edi_ai_panel.button = button;
   _edi_ai_panel.copy_button = copy_button;

   evas_object_event_callback_add(widget, EVAS_CALLBACK_DEL, _edi_aipanel_obj_del_cb, NULL);
   evas_object_event_callback_add(entry, EVAS_CALLBACK_DEL, _edi_aipanel_obj_del_cb, NULL);
   evas_object_event_callback_add(button, EVAS_CALLBACK_DEL, _edi_aipanel_obj_del_cb, NULL);
   evas_object_event_callback_add(copy_button, EVAS_CALLBACK_DEL, _edi_aipanel_obj_del_cb, NULL);

   _edi_aipanel_append_line("Configure provider/auth in Settings -> AI.");
   _edi_aipanel_append_line("AI panel ready.");

   elm_box_pack_end(hbox, entry);
   elm_box_pack_end(hbox, button);
   elm_box_pack_end(hbox, copy_button);

   elm_box_pack_end(box, widget);
   elm_box_pack_end(box, hbox);

   elm_object_content_set(frame, box);
   elm_box_pack_end(parent, frame);

   ecore_event_handler_add(EDI_EVENT_CONFIG_CHANGED, _edi_aipanel_config_changed_cb, NULL);
}

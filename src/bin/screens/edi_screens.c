#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <Elementary.h>
#include <Ecore.h>
#include <Ecore_File.h>

#include "edi_private.h"
#include "edi_screens.h"

static Evas_Object *_edi_screens_popup = NULL;

static char *
_edi_screens_message_markup_build(const char *message)
{
   Eina_Strbuf *buf;
   char *escaped;
   const char *p;
   char *out;

   escaped = elm_entry_utf8_to_markup(message ? message : "");
   if (!escaped)
     return strdup("");

   buf = eina_strbuf_new();
   for (p = escaped; *p; p++)
     {
        if (*p == '\n')
          eina_strbuf_append(buf, "<br/>");
        else
          eina_strbuf_append_char(buf, *p);
     }

   out = eina_strbuf_string_steal(buf);
   eina_strbuf_free(buf);
   free(escaped);

   return out;
}

static Evas_Object *
_edi_screens_message_label_add(Evas_Object *parent, const char *message)
{
   Evas_Object *label;
   char *markup;

   label = elm_label_add(parent);
   elm_label_line_wrap_set(label, ELM_WRAP_MIXED);
   elm_label_wrap_width_set(label, 520 * elm_config_scale_get());
   evas_object_size_hint_weight_set(label, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
   evas_object_size_hint_align_set(label, EVAS_HINT_FILL, EVAS_HINT_FILL);
   markup = _edi_screens_message_markup_build(message);
   elm_object_text_set(label, markup);
   free(markup);
   evas_object_show(label);

   return label;
}

static Evas_Object *
_edi_screens_message_code_add(Evas_Object *parent, const char *message)
{
   const char *start, *end;
   Evas_Object *scroller;
   Elm_Code *code;
   Elm_Code_Widget *widget;

   code = elm_code_create();
   start = message ? message : "";

   while ((end = strchr(start, '\n')))
     {
        elm_code_file_line_append(code->file, start, end - start, NULL);
        start = end + 1;
     }

   if (*start || !message || !message[0])
     elm_code_file_line_append(code->file, start, strlen(start), NULL);

   widget = elm_code_widget_add(parent, code);
   elm_code_widget_line_numbers_set(widget, EINA_FALSE);
   elm_code_widget_editable_set(widget, EINA_FALSE);
   elm_code_widget_policy_set(widget, ELM_SCROLLER_POLICY_OFF, ELM_SCROLLER_POLICY_OFF);
   evas_object_size_hint_weight_set(widget, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
   evas_object_size_hint_align_set(widget, EVAS_HINT_FILL, EVAS_HINT_FILL);
   evas_object_show(widget);

   scroller = elm_scroller_add(parent);
   elm_scroller_policy_set(scroller, ELM_SCROLLER_POLICY_AUTO, ELM_SCROLLER_POLICY_AUTO);
   elm_scroller_bounce_set(scroller, EINA_FALSE, EINA_FALSE);
   evas_object_size_hint_weight_set(scroller, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
   evas_object_size_hint_align_set(scroller, EVAS_HINT_FILL, EVAS_HINT_FILL);
   evas_object_size_hint_min_set(scroller, 420 * elm_config_scale_get(), 120 * elm_config_scale_get());
   evas_object_size_hint_max_set(scroller, 760 * elm_config_scale_get(), 300 * elm_config_scale_get());
   elm_object_content_set(scroller, widget);
   evas_object_show(scroller);

   return scroller;
}

static void
_edi_screens_popup_cancel_cb(void *data, Evas_Object *obj EINA_UNUSED,
                             void *event_info EINA_UNUSED)
{
   Edi_Settings_Tab *default_tab;
   char *copy_message;

   default_tab = evas_object_data_get((Evas_Object *) data, "default_tab");
   if (default_tab)
     free(default_tab);

   copy_message = evas_object_data_get((Evas_Object *) data, "copy_message");
   if (copy_message)
     free(copy_message);

   evas_object_del((Evas_Object *) data);
}

static void
_edi_screens_message_confirm_cb(void *data, Evas_Object *obj,
                                void *event_info EINA_UNUSED)
{
   void ((*confirm_fn)(void *)) = evas_object_data_get(obj, "callback");

   confirm_fn(data);

   if (_edi_screens_popup)
     {
        evas_object_del(_edi_screens_popup);
        _edi_screens_popup = NULL;
     }
}

static void
_edi_screens_message_copy_cb(void *data, Evas_Object *obj EINA_UNUSED,
                             void *event_info EINA_UNUSED)
{
   Evas_Object *popup;
   char *message;

   popup = data;
   message = evas_object_data_get(popup, "copy_message");
   if (!message || !message[0])
     return;

   elm_cnp_selection_set(popup, ELM_SEL_TYPE_CLIPBOARD, ELM_SEL_FORMAT_TEXT,
                         message, strlen(message));
}

void edi_screens_message_confirm(Evas_Object *parent, const char *message, void ((*confirm_cb)(void *)), void *data)
{
   Evas_Object *popup, *frame, *table, *label, *button, *icon, *box, *sep;

   _edi_screens_popup = popup = elm_popup_add(parent);
   elm_object_part_text_set(popup, "title,text", _("Confirmation required"));

   table = elm_table_add(popup);

   icon = elm_icon_add(table);
   elm_icon_standard_set(icon, "dialog-question");
   evas_object_size_hint_min_set(icon, 48 * elm_config_scale_get(), 48 * elm_config_scale_get());
   evas_object_size_hint_weight_set(icon, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
   evas_object_size_hint_align_set(icon, EVAS_HINT_FILL, EVAS_HINT_FILL);
   evas_object_show(icon);
   elm_table_pack(table, icon, 0, 0, 1, 1);

   label = _edi_screens_message_label_add(table, message);

   elm_table_pack(table, label, 1, 0, 1, 1);
   evas_object_show(table);

   box = elm_box_add(popup);
   elm_box_pack_end(box, table);
   sep = elm_separator_add(box);
   elm_separator_horizontal_set(sep, EINA_TRUE);
   evas_object_show(sep);
   elm_box_pack_end(box, sep);

   frame = elm_frame_add(popup);
   evas_object_show(frame);
   elm_object_content_set(frame, box);
   elm_object_content_set(popup, frame);

   button = elm_button_add(popup);
   elm_object_text_set(button, _("Yes"));
   elm_object_part_content_set(popup, "button1", button);
   evas_object_data_set(button, "callback", confirm_cb);
   evas_object_smart_callback_add(button, "clicked", _edi_screens_message_confirm_cb, data);

   button = elm_button_add(popup);
   elm_object_text_set(button, _("No"));
   elm_object_part_content_set(popup, "button2", button);
   evas_object_smart_callback_add(button, "clicked", _edi_screens_popup_cancel_cb, popup);

   evas_object_show(popup);
}

void edi_screens_message_icon(Evas_Object *parent, const char *title, const char *message,
                              const char *icon_name)
{
   Evas_Object *popup, *table, *box, *icon, *sep, *label, *button;
   char *copy_message;

   popup = elm_popup_add(parent);
   elm_object_part_text_set(popup, "title,text", title);

   table = elm_table_add(popup);
   icon = elm_icon_add(table);
   elm_icon_standard_set(icon, (icon_name && icon_name[0]) ? icon_name : "dialog-information");
   evas_object_size_hint_min_set(icon, 48 * elm_config_scale_get(), 48 * elm_config_scale_get());
   evas_object_size_hint_weight_set(icon, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
   evas_object_size_hint_align_set(icon, EVAS_HINT_FILL, EVAS_HINT_FILL);
   evas_object_show(icon);
   elm_table_pack(table, icon, 0, 0, 1, 1);

   label = _edi_screens_message_code_add(popup, message);
   elm_table_pack(table, label, 1, 0, 1, 1);
   evas_object_show(table);

   box = elm_box_add(popup);
   sep = elm_separator_add(box);
   elm_separator_horizontal_set(sep, EINA_TRUE);
   evas_object_show(sep);
   elm_box_pack_end(box, sep);
   elm_box_pack_end(box, table);
   sep = elm_separator_add(box);
   elm_separator_horizontal_set(sep, EINA_TRUE);
   evas_object_show(sep);
   elm_box_pack_end(box, sep);

   elm_object_content_set(popup, box);

   button = elm_button_add(popup);
   elm_object_text_set(button, _("OK"));
   elm_object_part_content_set(popup, "button1", button);
   evas_object_smart_callback_add(button, "clicked", _edi_screens_popup_cancel_cb, popup);

    copy_message = strdup(message ? message : "");
    if (copy_message)
      {
         evas_object_data_set(popup, "copy_message", copy_message);
         button = elm_button_add(popup);
         elm_object_text_set(button, _("Copy Error"));
         elm_object_part_content_set(popup, "button2", button);
         evas_object_smart_callback_add(button, "clicked", _edi_screens_message_copy_cb, popup);
      }

   evas_object_show(popup);
}

void edi_screens_message(Evas_Object *parent, const char *title, const char *message)
{
   edi_screens_message_icon(parent, title, message, "dialog-information");
}

static void
_edi_screens_settings_display_cb(void *data, Evas_Object *obj,
                             void *event_info EINA_UNUSED)
{
   Evas_Object *parent;
   Edi_Settings_Tab type;

   parent = evas_object_data_get(obj, "parent");
   type = *((Edi_Settings_Tab *) evas_object_data_get((Evas_Object *) data, "default_tab"));
   evas_object_del((Evas_Object *) data);

   edi_settings_show(parent, type);
}

void edi_screens_settings_message(Evas_Object *parent, Edi_Settings_Tab type, const char *title, const char *message)
{
   Evas_Object *popup, *frame, *table, *box, *icon, *sep, *label, *button;
   Edi_Settings_Tab *default_tab;

   popup = elm_popup_add(parent);
   elm_object_part_text_set(popup, "title,text", title);

   table = elm_table_add(popup);
   elm_table_padding_set(table, 10, 10);
   icon = elm_icon_add(table);
   elm_icon_standard_set(icon, "dialog-information");
   evas_object_size_hint_min_set(icon, 48 * elm_config_scale_get(), 48 * elm_config_scale_get());
   evas_object_size_hint_weight_set(icon, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
   evas_object_size_hint_align_set(icon, EVAS_HINT_FILL, EVAS_HINT_FILL);
   evas_object_show(icon);
   elm_table_pack(table, icon, 0, 0, 1, 1);

   frame = elm_frame_add(popup);
   elm_object_content_set(frame, table);
   evas_object_show(frame);

   box = elm_box_add(popup);
   label = _edi_screens_message_label_add(popup, message);
   elm_box_pack_end(box, label);
   evas_object_show(table);

   sep = elm_separator_add(box);
   elm_separator_horizontal_set(sep, EINA_TRUE);
   evas_object_show(sep);
   elm_box_pack_end(box, sep);
   evas_object_show(box);
   elm_table_pack(table, box, 1, 0, 1, 1);
   elm_object_content_set(popup, frame);
   evas_object_show(table);

   button = elm_button_add(popup);
   elm_object_text_set(button, _("OK"));
   elm_object_part_content_set(popup, "button1", button);
   evas_object_smart_callback_add(button, "clicked", _edi_screens_popup_cancel_cb, popup);

   button = elm_button_add(popup);
   elm_object_text_set(button, _("Settings"));
   elm_object_part_content_set(popup, "button2", button);
   evas_object_data_set(button, "parent", parent);

   default_tab = malloc(sizeof(Edi_Settings_Tab));
   if (default_tab)
     {
        *default_tab = type;
        evas_object_data_set(popup, "default_tab", default_tab);
     }

   evas_object_smart_callback_add(button, "clicked", _edi_screens_settings_display_cb, popup);

   evas_object_show(popup);
}

static void
_edi_screens_notify_close_cb(void *data EINA_UNUSED, Evas_Object *obj,
                             void *event_info EINA_UNUSED)
{
   evas_object_del(obj);
}

static void
_edi_screens_desktop_notify_external(const char *title, const char *message, int status)
{
   Eina_Strbuf *command;

   if (!ecore_file_app_installed("notify-send"))
     return;

   command = eina_strbuf_new();
   eina_strbuf_append_printf(command, "notify-send -t 10000 -i edi -u %s '%s' '%s'",
                             status == 0 ? "normal" : "critical", title, message);
   ecore_exe_run(eina_strbuf_string_get(command), NULL);
   eina_strbuf_free(command);
}

void edi_screens_desktop_notify(Evas_Object *parent, const char *title, const char *message, int status)
{
   Evas_Object *notify, *outer, *header, *title_box, *content, *message_label;
   Evas_Object *edi_icon, *title_label, *sizer;

   if (!parent || !title || !message)
     return;

   notify = elm_notify_add(parent);
   elm_notify_align_set(notify, 1.0, 0.02);
   elm_notify_timeout_set(notify, 8.0);
   elm_notify_allow_events_set(notify, EINA_TRUE);
   evas_object_smart_callback_add(notify, "timeout", _edi_screens_notify_close_cb, NULL);
   evas_object_smart_callback_add(notify, "block,clicked", _edi_screens_notify_close_cb, NULL);

   outer = elm_box_add(notify);
   elm_box_padding_set(outer, 0, 4);
   evas_object_size_hint_weight_set(outer, 0.0, 0.0);
   evas_object_size_hint_align_set(outer, EVAS_HINT_FILL, 0.0);

   sizer = evas_object_rectangle_add(evas_object_evas_get(notify));
   evas_object_color_set(sizer, 0, 0, 0, 0);
   evas_object_size_hint_weight_set(sizer, EVAS_HINT_EXPAND, 0.0);
   evas_object_size_hint_align_set(sizer, EVAS_HINT_FILL, 0.0);
   evas_object_size_hint_min_set(sizer,
                                 300 * elm_config_scale_get(),
                                 1);
   elm_box_pack_end(outer, sizer);
   evas_object_show(sizer);

   header = elm_box_add(outer);
   elm_box_horizontal_set(header, EINA_TRUE);
   elm_box_padding_set(header, 4, 0);
   evas_object_size_hint_weight_set(header, EVAS_HINT_EXPAND, 0.0);
   evas_object_size_hint_align_set(header, EVAS_HINT_FILL, 0.0);

   edi_icon = elm_icon_add(header);
   elm_icon_standard_set(edi_icon, "edi");
   evas_object_size_hint_min_set(edi_icon, 28 * elm_config_scale_get(), 28 * elm_config_scale_get());
   evas_object_size_hint_align_set(edi_icon, 0.0, 0.5);
   elm_box_pack_end(header, edi_icon);
   evas_object_show(edi_icon);

   title_box = elm_box_add(header);
   evas_object_size_hint_weight_set(title_box, EVAS_HINT_EXPAND, 0.0);
   evas_object_size_hint_align_set(title_box, EVAS_HINT_FILL, 0.5);
   title_label = elm_label_add(title_box);
   evas_object_size_hint_align_set(title_label, EVAS_HINT_FILL, 0.5);
   elm_object_text_set(title_label, eina_slstr_printf("<b>%s</b>", title));
   elm_box_pack_end(title_box, title_label);
   evas_object_show(title_label);
   elm_box_pack_end(header, title_box);
   evas_object_show(title_box);

   elm_box_pack_end(outer, header);
   evas_object_show(header);

   content = elm_box_add(outer);
   elm_box_padding_set(content, 8, 0);
   evas_object_size_hint_weight_set(content, EVAS_HINT_EXPAND, 0.0);
   evas_object_size_hint_align_set(content, EVAS_HINT_FILL, 0.0);

   message_label = elm_label_add(content);
   elm_label_line_wrap_set(message_label, ELM_WRAP_WORD);
   evas_object_size_hint_weight_set(message_label, EVAS_HINT_EXPAND, 0.0);
   evas_object_size_hint_align_set(message_label, EVAS_HINT_FILL, 0.0);
   elm_object_text_set(message_label, eina_slstr_printf("<align=left>%s</align>", message));
   elm_box_pack_end(content, message_label);
   evas_object_show(message_label);

   elm_box_pack_end(outer, content);
   evas_object_show(content);

   elm_object_content_set(notify, outer);
   evas_object_show(outer);
   evas_object_show(notify);

   _edi_screens_desktop_notify_external(title, message, status);
}

void
edi_screens_scm_binary_missing(Evas_Object *parent, const char *binary)
{
   Evas_Object *popup, *label, *button;
   Eina_Strbuf *text = eina_strbuf_new();

   eina_strbuf_append_printf(text, _("No %s binary found, please install %s."), binary, binary);

   popup = elm_popup_add(parent);
   elm_object_part_text_set(popup, "title,text", _("Unable to launch SCM binary"));
   label = elm_label_add(popup);
   elm_object_text_set(label, eina_strbuf_string_get(text));
   evas_object_show(label);
   elm_object_content_set(popup, label);

   eina_strbuf_free(text);

   button = elm_button_add(popup);
   elm_object_text_set(button, _("OK"));
   elm_object_part_content_set(popup, "button1", button);
   evas_object_smart_callback_add(button, "clicked", _edi_screens_popup_cancel_cb, popup);

   evas_object_show(popup);
}

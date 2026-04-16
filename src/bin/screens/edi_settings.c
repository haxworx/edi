#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <Elementary.h>
#include <Ecore.h>
#include <ctype.h>
#include <stdint.h>
#include <string.h>

#include "Edi.h"
#include "edi_agent.h"
#include "edi_screens.h"
#include "edi_config.h"
#include "edi_debug.h"
#include "edi_filepanel.h"
#include "edi_theme.h"

#include "edi_private.h"

static Evas_Object *_edi_settings_win;
static Elm_Object_Item *_edi_settings_display, *_edi_settings_builds,
                       *_edi_settings_behaviour, *_edi_settings_project;
static Ecore_Event_Handler *_edi_settings_config_handler;
static Evas_Object *_edi_settings_font_preview_code;
static Evas_Object *_edi_settings_agent_model_combobox;
static Edi_Agent_Models_Request *_edi_settings_agent_models_request;

#define EDI_SETTINGS_TABLE_PADDING 5

static char *
_edi_settings_entry_text_get(Evas_Object *obj)
{
   const char *markup;
   char *text;
   char *start;
   char *end;

   markup = elm_object_text_get(obj);
   text = elm_entry_markup_to_utf8(markup ? markup : "");
   if (!text)
     return strdup("");

   start = text;
   while (*start && isspace((unsigned char)*start))
     start++;

   end = start + strlen(start);
   while (end > start && isspace((unsigned char)*(end - 1)))
     end--;
   *end = '\0';

   if (start != text)
     memmove(text, start, end - start + 1);

   return text;
}

static void
_edi_settings_project_agent_model_items_clear(Evas_Object *combobox)
{
   char **old_models;
   unsigned int old_count;
   unsigned int i;

   if (!combobox)
     return;

   old_models = evas_object_data_get(combobox, "agent_model_items");
   old_count = (unsigned int)(uintptr_t)evas_object_data_get(combobox, "agent_model_items_count");
   if (old_models)
     {
        for (i = 0; i < old_count; i++)
          free(old_models[i]);
        free(old_models);
     }
   evas_object_data_set(combobox, "agent_model_items", NULL);
   evas_object_data_set(combobox, "agent_model_items_count", NULL);
}

static void
_edi_settings_exit(void *data, Evas_Object *obj EINA_UNUSED, void *event_info EINA_UNUSED)
{
   if (_edi_settings_config_handler)
     {
        ecore_event_handler_del(_edi_settings_config_handler);
        _edi_settings_config_handler = NULL;
     }
   if (_edi_settings_agent_models_request)
     {
        edi_agent_provider_models_fetch_cancel(_edi_settings_agent_models_request);
        _edi_settings_agent_models_request = NULL;
     }
   _edi_settings_project_agent_model_items_clear(_edi_settings_agent_model_combobox);
   _edi_settings_agent_model_combobox = NULL;
   _edi_settings_font_preview_code = NULL;
   _edi_settings_win = NULL;

   evas_object_del(data);
}

static void
_edi_settings_category_cb(void *data, Evas_Object *obj EINA_UNUSED, void *event_info)
{
   Elm_Object_Item *item, *toolbar_item;

   item = (Elm_Object_Item *)data;
   toolbar_item = (Elm_Object_Item *)event_info;
   if (toolbar_item && !elm_toolbar_item_selected_get(toolbar_item))
     return;

   elm_naviframe_item_promote(item);
}

static void
_edi_settings_toolbar_single_select(Evas_Object *tb, Elm_Object_Item *selected)
{
   Elm_Object_Item *it;

   if (!tb || !selected)
     return;

   for (it = elm_toolbar_first_item_get(tb); it; it = elm_toolbar_item_next_get(it))
     {
        if (it != selected && elm_toolbar_item_selected_get(it))
          elm_toolbar_item_selected_set(it, EINA_FALSE);
     }
}

static Evas_Object *
_edi_settings_panel_create(Evas_Object *parent, const char *title)
{
   Evas_Object *box, *frame;

   box = elm_box_add(parent);
   elm_box_horizontal_set(box, EINA_FALSE);
   evas_object_size_hint_weight_set(box, EVAS_HINT_EXPAND, 0.0);
   evas_object_size_hint_align_set(box, EVAS_HINT_FILL, 0.0);
   evas_object_show(box);

   frame = elm_frame_add(parent);
   elm_object_text_set(frame, title);
   elm_object_style_set(frame, "outdent_top");
   elm_object_part_content_set(frame, "default", box);
   evas_object_size_hint_weight_set(frame, EVAS_HINT_EXPAND, 0.0);
   evas_object_size_hint_align_set(frame, EVAS_HINT_FILL, 0.0);
   evas_object_show(frame);

   return frame;
}

static void
_edi_settings_display_whitespace_cb(void *data EINA_UNUSED, Evas_Object *obj,
                                    void *event EINA_UNUSED)
{
   Evas_Object *check;

   check = (Evas_Object *)obj;
   _edi_project_config->gui.show_whitespace = elm_check_state_get(check);
   _edi_project_config_save();
}

static void
_edi_settings_display_tab_inserts_spaces_cb(void *data EINA_UNUSED, Evas_Object *obj,
                                            void *event EINA_UNUSED)
{
   Evas_Object *check;

   check = (Evas_Object *)obj;
   _edi_project_config->gui.tab_inserts_spaces = elm_check_state_get(check);
   _edi_project_config_save();
}

static void
_edi_settings_display_show_width_marker_cb(void *data EINA_UNUSED, Evas_Object *obj,
                                           void *event EINA_UNUSED)
{
   Evas_Object *check = (Evas_Object *)obj;
   _edi_project_config->gui.show_width_marker = elm_check_state_get(check);
   _edi_project_config_save();
}

static void
_edi_settings_display_line_numbers_cb(void *data EINA_UNUSED, Evas_Object *obj,
                                      void *event EINA_UNUSED)
{
   Evas_Object *check = (Evas_Object *)obj;
   _edi_project_config->gui.show_line_numbers = elm_check_state_get(check);
   _edi_project_config_save();
}

static void
_edi_settings_display_width_marker_cb(void *data EINA_UNUSED, Evas_Object *obj,
                                     void *event EINA_UNUSED)
{
   Evas_Object *spinner;

   spinner = (Evas_Object *)obj;
   _edi_project_config->gui.width_marker = (int) elm_spinner_value_get(spinner);
   _edi_project_config_save();
}

static void
_edi_settings_display_tabstop_cb(void *data EINA_UNUSED, Evas_Object *obj,
                                     void *event EINA_UNUSED)
{
   Evas_Object *spinner;

   spinner = (Evas_Object *)obj;
   _edi_project_config->gui.tabstop = (int) elm_spinner_value_get(spinner);
   _edi_project_config_save();
}

static void
_edi_settings_toolbar_hidden_cb(void *data EINA_UNUSED, Evas_Object *obj,
                                void *event EINA_UNUSED)
{
   Evas_Object *check;

   check = (Evas_Object *)obj;
   _edi_project_config->gui.toolbar_hidden = elm_check_state_get(check);
   _edi_project_config_save();
}

static void
_edi_settings_toolbar_horizontal_cb(void *data EINA_UNUSED, Evas_Object *obj,
                                    void *event EINA_UNUSED)
{
   Evas_Object *check;

   check = (Evas_Object *)obj;
   _edi_project_config->gui.toolbar_horizontal = elm_check_state_get(check);
   _edi_project_config_save();
}

static void
_edi_settings_toolbar_text_visible_cb(void *data EINA_UNUSED, Evas_Object *obj,
                              void *event EINA_UNUSED)
{
   Evas_Object *check;

   check = (Evas_Object *)obj;
   _edi_project_config->gui.toolbar_text_visible = elm_check_state_get(check);
   _edi_project_config_save();
}

static void
_edi_settings_font_choose_cb(void *data EINA_UNUSED, Evas_Object *obj EINA_UNUSED, void *event_info EINA_UNUSED)
{
   Evas_Object *naviframe, *box;

   naviframe = (Evas_Object *)data;
   box = elm_box_add(naviframe);
   elm_box_horizontal_set(box, EINA_FALSE);
   evas_object_size_hint_weight_set(box, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
   evas_object_size_hint_align_set(box, EVAS_HINT_FILL, EVAS_HINT_FILL);
   evas_object_show(box);

   edi_settings_font_add(box);
   elm_naviframe_item_push(naviframe, _("Font"), NULL, NULL, box, NULL);
}

static Evas_Object *
_edi_settings_font_preview_add(Evas_Object *parent, const char *font_name, int font_size)
{
   Elm_Code_Widget *widget, *table, *rect;
   Elm_Code *code;
   Evas_Coord cx, cy, cw, ch, preview_h;
   const char *preview_lines[] =
     {
        " int main(void) {",
        "   return 0;",
        " }"
     };

   table = elm_table_add(parent);
   evas_object_size_hint_weight_set(table, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
   evas_object_size_hint_align_set(table, EVAS_HINT_FILL, EVAS_HINT_FILL);

   rect = evas_object_rectangle_add(evas_object_evas_get(parent));
   evas_object_color_set(rect, 0, 0, 0, 0);
   evas_object_size_hint_min_set(rect, 240 * elm_config_scale_get(), 40 * elm_config_scale_get());
   evas_object_size_hint_weight_set(rect, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
   evas_object_size_hint_align_set(rect, EVAS_HINT_FILL, EVAS_HINT_FILL);
   elm_table_pack(table, rect, 0, 0, 1, 1);
   evas_object_show(rect);

   code = elm_code_create();
   elm_code_file_line_append(code->file, preview_lines[0], strlen(preview_lines[0]), NULL);
   elm_code_file_line_append(code->file, preview_lines[1], strlen(preview_lines[1]), NULL);
   elm_code_file_line_append(code->file, preview_lines[2], strlen(preview_lines[2]), NULL);

   widget = elm_code_widget_add(parent, code);
   elm_code_widget_font_set(widget, font_name, font_size);
   elm_code_widget_line_numbers_set(widget, EINA_TRUE);
   elm_code_widget_editable_set(widget, EINA_FALSE);
   elm_code_widget_policy_set(widget, ELM_SCROLLER_POLICY_OFF, ELM_SCROLLER_POLICY_OFF);
   evas_object_size_hint_weight_set(widget, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
   evas_object_size_hint_align_set(widget, EVAS_HINT_FILL, EVAS_HINT_FILL);
   evas_object_pass_events_set(widget, EINA_TRUE);
   elm_table_pack(table, widget, 0, 0, 1, 1);
   evas_object_show(widget);
   evas_object_data_set(table, "code", widget);

   if (!elm_code_widget_geometry_for_position_get(widget, 1, 1, &cx, &cy, &cw, &ch) || ch <= 0)
     preview_h = 3 * elm_config_finger_size_get();
   else
     preview_h = ch * 3;

   evas_object_size_hint_min_set(rect, 240 * elm_config_scale_get(), preview_h);
   evas_object_size_hint_min_set(widget, 0, preview_h);
   evas_object_size_hint_min_set(table, 0, preview_h);
   evas_object_show(table);

   return table;
}

static Eina_Bool
_edi_settings_config_changed_cb(void *data EINA_UNUSED, int type EINA_UNUSED, void *event EINA_UNUSED)
{
   if (_edi_settings_font_preview_code)
     elm_code_widget_font_set(_edi_settings_font_preview_code,
                              _edi_project_config->font.name,
                              _edi_project_config->font.size);

   return ECORE_CALLBACK_PASS_ON;
}

static void
_edi_settings_display_theme_pressed_cb(void *data EINA_UNUSED, Evas_Object *obj, void *event_info)
{
   Edi_Theme *theme;
   const char *text = elm_object_item_text_get(event_info);

   theme = elm_object_item_data_get(event_info);

   if (_edi_project_config->gui.theme)
     eina_stringshare_del(_edi_project_config->gui.theme);

   _edi_project_config->gui.theme = eina_stringshare_add(theme->name);
   _edi_project_config_save();

   elm_object_text_set(obj, text);
   elm_combobox_hover_end(obj);
}

static char *
_edi_settings_display_theme_text_get_cb(void *data, Evas_Object *obj EINA_UNUSED, const char *part EINA_UNUSED)
{
   Edi_Theme *current;

   current = data;

   return strdup(current->title);
}

static Evas_Object *
_edi_settings_display_create(Evas_Object *parent)
{
   Evas_Object *container, *box, *frame, *label, *spinner, *check, *button, *preview;
   Evas_Object *table, *combobox;
   Elm_Genlist_Item_Class *itc;
   Edi_Theme *theme;
   Eina_List *themes, *l;

   container = elm_box_add(parent);
   evas_object_size_hint_weight_set(container, EVAS_HINT_EXPAND, 0.0);
   evas_object_size_hint_align_set(container, EVAS_HINT_FILL, EVAS_HINT_FILL);
   evas_object_show(container);

   frame = _edi_settings_panel_create(parent, _("Display"));
   elm_object_style_set(frame, "pad_small");
   box = elm_object_part_content_get(frame, "default");
   elm_box_pack_end(container, frame);

   table = elm_table_add(parent);
   evas_object_size_hint_weight_set(table, EVAS_HINT_EXPAND, 0.0);
   evas_object_size_hint_align_set(table, EVAS_HINT_FILL, EVAS_HINT_FILL);
   elm_table_padding_set(table, EDI_SETTINGS_TABLE_PADDING, EDI_SETTINGS_TABLE_PADDING);
   evas_object_show(table);
   elm_box_pack_end(box, table);

   label = elm_label_add(table);
   elm_object_text_set(label, _("Font"));
   evas_object_size_hint_align_set(label, EVAS_HINT_EXPAND, 0.5);
   elm_table_pack(table, label, 0, 0, 1, 1);
   evas_object_show(label);

   button = elm_button_add(table);
   evas_object_size_hint_weight_set(button, EVAS_HINT_EXPAND, 0.0);
   evas_object_size_hint_align_set(button, EVAS_HINT_FILL, EVAS_HINT_FILL);
   evas_object_show(button);
   preview = _edi_settings_font_preview_add(table, _edi_project_config->font.name,
                                            _edi_project_config->font.size);
   _edi_settings_font_preview_code = evas_object_data_get(preview, "code");
   elm_layout_content_set(button, "elm.swallow.content", preview);
   elm_table_pack(table, button, 1, 0, 1, 1);
   evas_object_smart_callback_add(button, "clicked",
                                  _edi_settings_font_choose_cb, parent);

   elm_object_focus_set(button, EINA_TRUE);

   label = elm_label_add(table);
   elm_object_text_set(label, _("Color theme"));
   evas_object_size_hint_align_set(label, EVAS_HINT_EXPAND, 0.5);
   elm_table_pack(table, label, 0, 1, 1, 1);
   evas_object_show(label);

   combobox = elm_combobox_add(table);
   evas_object_size_hint_weight_set(combobox, EVAS_HINT_EXPAND, 0.0);
   evas_object_size_hint_align_set(combobox, EVAS_HINT_FILL, EVAS_HINT_FILL);
   evas_object_show(combobox);
   evas_object_smart_callback_add(combobox, "item,pressed",
                                 _edi_settings_display_theme_pressed_cb, NULL);

   if (!_edi_project_config->gui.theme)
     elm_object_text_set(combobox, edi_theme_theme_by_name("default")->title);
   else
     elm_object_text_set(combobox, edi_theme_theme_by_name(_edi_project_config->gui.theme)->title);

   elm_table_pack(table, combobox, 1, 1, 1, 1);

   itc = elm_genlist_item_class_new();
   itc->item_style = "default";
   itc->func.text_get = _edi_settings_display_theme_text_get_cb;

   themes = edi_theme_themes_get();

   EINA_LIST_FOREACH(themes, l, theme)
     {
        elm_genlist_item_append(combobox, itc, theme, NULL, ELM_GENLIST_ITEM_NONE, NULL, NULL);
     }

   elm_genlist_realized_items_update(combobox);
   elm_genlist_item_class_free(itc);

   frame = _edi_settings_panel_create(parent, _("Toolbar"));
   box = elm_object_part_content_get(frame, "default");
   elm_box_pack_end(container, frame);

   table = elm_table_add(parent);
   evas_object_size_hint_weight_set(table, EVAS_HINT_EXPAND, 0.0);
   evas_object_size_hint_align_set(table, EVAS_HINT_FILL, 0.0);
   elm_table_padding_set(table, EDI_SETTINGS_TABLE_PADDING, EDI_SETTINGS_TABLE_PADDING);
   evas_object_show(table);
   elm_box_pack_end(box, table);

   check = elm_check_add(box);
   elm_object_text_set(check, _("Hide Toolbar"));
   elm_check_state_set(check, _edi_project_config->gui.toolbar_hidden);
   evas_object_size_hint_weight_set(check, EVAS_HINT_EXPAND, 0.0);
   evas_object_size_hint_align_set(check, EVAS_HINT_FILL, 0.5);
   evas_object_smart_callback_add(check, "changed",
                                  _edi_settings_toolbar_hidden_cb, NULL);
   elm_table_pack(table, check, 0, 0, 1, 1);
   evas_object_show(check);

   check = elm_check_add(box);
   elm_object_text_set(check, _("Horizontal Toolbar"));
   elm_check_state_set(check, _edi_project_config->gui.toolbar_horizontal);
   evas_object_size_hint_weight_set(check, EVAS_HINT_EXPAND, 0.0);
   evas_object_size_hint_align_set(check, EVAS_HINT_FILL, 0.5);
   evas_object_smart_callback_add(check, "changed",
                                  _edi_settings_toolbar_horizontal_cb, NULL);
   elm_table_pack(table, check, 0, 1, 1, 1);
   evas_object_show(check);
   elm_box_pack_end(box, table);

   check = elm_check_add(box);
   elm_object_text_set(check, _("Show Toolbar Text"));
   elm_check_state_set(check, _edi_project_config->gui.toolbar_text_visible);
   evas_object_size_hint_weight_set(check, EVAS_HINT_EXPAND, 0.0);
   evas_object_size_hint_align_set(check, EVAS_HINT_FILL, 0.5);
   evas_object_smart_callback_add(check, "changed",
                                  _edi_settings_toolbar_text_visible_cb, NULL);
   evas_object_show(check);
   elm_table_pack(table, check, 0, 2, 1, 1);
   elm_box_pack_end(box, table);

   frame = _edi_settings_panel_create(parent, _("Editor"));
   box = elm_object_part_content_get(frame, "default");
   elm_box_pack_end(container, frame);

   table = elm_table_add(parent);
   evas_object_size_hint_weight_set(table, EVAS_HINT_EXPAND, 0.0);
   evas_object_size_hint_align_set(table, EVAS_HINT_FILL, 0.0);
   elm_table_padding_set(table, EDI_SETTINGS_TABLE_PADDING, EDI_SETTINGS_TABLE_PADDING);
   evas_object_show(table);
   elm_box_pack_end(box, table);

   check = elm_check_add(box);
   elm_object_text_set(check, _("Display line numbers"));
   elm_check_state_set(check, _edi_project_config->gui.show_line_numbers);
   evas_object_size_hint_weight_set(check, EVAS_HINT_EXPAND, 0.0);
   evas_object_size_hint_align_set(check, EVAS_HINT_FILL, 0.5);
   evas_object_smart_callback_add(check, "changed",
                                  _edi_settings_display_line_numbers_cb, NULL);
   elm_table_pack(table, check, 0, 0, 2, 1);
   evas_object_show(check);

   label = elm_label_add(box);
   elm_object_text_set(label, _("Line width marker"));
   evas_object_size_hint_weight_set(label, EVAS_HINT_EXPAND, 0.0);
   evas_object_size_hint_align_set(label, 1.0, 0.5);
   elm_table_pack(table, label, 0, 1, 1, 1);
   evas_object_show(label);

   spinner = elm_spinner_add(box);
   elm_spinner_value_set(spinner, _edi_project_config->gui.width_marker);
   elm_spinner_editable_set(spinner, EINA_TRUE);
   elm_spinner_step_set(spinner, 1);
   elm_spinner_wrap_set(spinner, EINA_FALSE);
   elm_spinner_min_max_set(spinner, 0, 1024);
   evas_object_size_hint_weight_set(spinner, 0.0, 0.0);
   evas_object_size_hint_align_set(spinner, 0.0, 0.5);
   evas_object_smart_callback_add(spinner, "changed",
                                  _edi_settings_display_width_marker_cb, NULL);
   elm_table_pack(table, spinner, 1, 1, 1, 1);
   evas_object_show(spinner);

   label = elm_label_add(box);
   elm_object_text_set(label, _("Tabstop"));
   evas_object_size_hint_weight_set(label, EVAS_HINT_EXPAND, 0.0);
   evas_object_size_hint_align_set(label, 1.0, 0.5);
   elm_table_pack(table, label, 0, 2, 1, 1);
   evas_object_show(label);

   spinner = elm_spinner_add(box);
   elm_spinner_value_set(spinner, _edi_project_config->gui.tabstop);
   elm_spinner_editable_set(spinner, EINA_TRUE);
   elm_spinner_step_set(spinner, 1);
   elm_spinner_wrap_set(spinner, EINA_FALSE);
   elm_spinner_min_max_set(spinner, 1, 32);
   evas_object_size_hint_weight_set(spinner, EVAS_HINT_EXPAND, 0.0);
   evas_object_size_hint_align_set(spinner, EVAS_HINT_FILL, 0.5);
   evas_object_smart_callback_add(spinner, "changed",
                                  _edi_settings_display_tabstop_cb, NULL);
   elm_table_pack(table, spinner, 1, 2, 1, 1);
   evas_object_show(spinner);

   check = elm_check_add(box);
   elm_object_text_set(check, _("Display whitespace"));
   elm_check_state_set(check, _edi_project_config->gui.show_whitespace);
   evas_object_size_hint_weight_set(check, EVAS_HINT_EXPAND, 0.0);
   evas_object_size_hint_align_set(check, 0.0, 0.5);
   evas_object_smart_callback_add(check, "changed",
                                  _edi_settings_display_whitespace_cb, NULL);
   elm_table_pack(table, check, 2, 0, 1, 1);
   evas_object_show(check);

   check = elm_check_add(box);
   elm_object_text_set(check, _("Display line width marker"));
   elm_check_state_set(check, _edi_project_config->gui.show_width_marker);
   evas_object_size_hint_weight_set(check, 0.0, 0.0);
   evas_object_size_hint_align_set(check, 0.0, 0.5);
   evas_object_smart_callback_add(check, "changed",
                                  _edi_settings_display_show_width_marker_cb, NULL);
   evas_object_show(check);
   elm_table_pack(table, check, 2, 1, 1, 1);

   check = elm_check_add(box);
   elm_object_text_set(check, ("Insert spaces when tab is pressed"));
   elm_check_state_set(check, _edi_project_config->gui.tab_inserts_spaces);
   evas_object_size_hint_weight_set(check, EVAS_HINT_EXPAND, 0.0);
   evas_object_size_hint_align_set(check, 0.0, 0.5);
   evas_object_smart_callback_add(check, "changed",
                                  _edi_settings_display_tab_inserts_spaces_cb, NULL);
   elm_table_pack(table, check, 2, 2, 1, 1);
   evas_object_show(check);

   elm_box_pack_end(box, table);

   return container;
}

static void
_edi_settings_builds_binary_chosen_cb(void *data, Evas_Object *obj EINA_UNUSED,
                                      void *event_info)
{
   Evas_Object *label = data;
   const char *file = event_info;

   if (!file)
     return;

   if (_edi_project_config->launch.path)
     eina_stringshare_del(_edi_project_config->launch.path);

   elm_object_text_set(label, file);
   _edi_project_config->launch.path = eina_stringshare_add(file);
   _edi_project_config_save();
}

static void
_edi_settings_builds_args_cb(void *data EINA_UNUSED, Evas_Object *obj,
                             void *event EINA_UNUSED)
{
   Evas_Object *entry;

   entry = (Evas_Object *)obj;

   if (_edi_project_config->launch.args)
     eina_stringshare_del(_edi_project_config->launch.args);

   _edi_project_config->launch.args = eina_stringshare_add(elm_object_text_get(entry));
   _edi_project_config_save();
}

static char *
_edi_settings_builds_debug_tool_text_get_cb(void *data, Evas_Object *obj EINA_UNUSED, const char *part EINA_UNUSED)
{
   Edi_Debug_Tool *tool;
   int i;

   i = (int)(uintptr_t) data;

   tool = &edi_debug_tools_get()[i];

   return strdup(tool->name);
}

static void _edi_settings_builds_debug_pressed_cb(void *data EINA_UNUSED, Evas_Object *obj, void *event_info)
{
   const char *text = elm_object_item_text_get(event_info);

   if (_edi_project_config->debug_command)
     eina_stringshare_del(_edi_project_config->debug_command);

   _edi_project_config->debug_command = eina_stringshare_add(text);
   _edi_project_config_save();

   elm_object_text_set(obj, text);
   elm_combobox_hover_end(obj);
}

static Evas_Object *
_edi_settings_builds_create(Evas_Object *parent)
{
   Evas_Object *box, *frame, *table, *label, *ic, *selector, *file, *entry;
   Evas_Object *combobox;
   Elm_Genlist_Item_Class *itc;
   Edi_Debug_Tool *tools;
   int i;

   frame = _edi_settings_panel_create(parent, _("Builds"));
   box = elm_object_part_content_get(frame, "default");

   table = elm_table_add(parent);
   evas_object_size_hint_weight_set(table, EVAS_HINT_EXPAND, 0.0);
   evas_object_size_hint_align_set(table, EVAS_HINT_FILL, EVAS_HINT_FILL);
   elm_table_padding_set(table, EDI_SETTINGS_TABLE_PADDING, EDI_SETTINGS_TABLE_PADDING);
   evas_object_show(table);
   elm_box_pack_end(box, table);

   label = elm_label_add(box);
   elm_object_text_set(label, _("Runtime binary"));
   evas_object_size_hint_weight_set(label, 0.0, 0.0);
   evas_object_size_hint_align_set(label, 0.0, EVAS_HINT_FILL);
   elm_table_pack(table, label, 0, 0, 1, 1);
   evas_object_show(label);

   ic = elm_icon_add(box);
   elm_icon_standard_set(ic, "file");
   evas_object_size_hint_aspect_set(ic, EVAS_ASPECT_CONTROL_VERTICAL, 1, 1);
   evas_object_show(ic);

   selector = elm_fileselector_button_add(box);
   elm_fileselector_path_set(selector, edi_project_get());
   elm_fileselector_expandable_set(selector, EINA_FALSE);
   elm_object_text_set(selector, _("Select"));
   elm_object_part_content_set(selector, "icon", ic);
   evas_object_size_hint_align_set(selector, EVAS_HINT_FILL, EVAS_HINT_FILL);
   elm_table_pack(table, selector, 1, 0, 1, 1);
   evas_object_show(selector);

   file = elm_entry_add(box);
   elm_entry_editable_set(file, EINA_FALSE);
   elm_entry_single_line_set(file, EINA_TRUE);
   elm_entry_scrollable_set(file, EINA_TRUE);
   elm_object_text_set(file, _edi_project_config->launch.path);
   evas_object_size_hint_weight_set(file, 0.75, 0.0);
   evas_object_size_hint_align_set(file, EVAS_HINT_FILL, EVAS_HINT_FILL);
   elm_table_pack(table, file, 2, 0, 1, 1);
   evas_object_show(file);

   evas_object_smart_callback_add(selector, "file,chosen",
                                  _edi_settings_builds_binary_chosen_cb, file);

   label = elm_label_add(box);
   elm_object_text_set(label, _("Runtime arguments"));
   evas_object_size_hint_weight_set(label, 0.0, 0.0);
   evas_object_size_hint_align_set(label, 0.0, EVAS_HINT_FILL);
   elm_table_pack(table, label, 0, 1, 1, 1);
   evas_object_show(label);

   entry = elm_entry_add(box);
   elm_object_text_set(entry, _edi_project_config->launch.args);
   elm_entry_editable_set(entry, EINA_TRUE);
   elm_entry_single_line_set(entry, EINA_TRUE);
   elm_entry_scrollable_set(entry, EINA_TRUE);
   evas_object_size_hint_weight_set(entry, 0.75, 0.0);
   evas_object_size_hint_align_set(entry, EVAS_HINT_FILL, EVAS_HINT_FILL);
   elm_table_pack(table, entry, 1, 1, 2, 1);
   evas_object_show(entry);
   evas_object_smart_callback_add(entry, "changed",
                                  _edi_settings_builds_args_cb, NULL);

   label = elm_label_add(box);
   elm_object_text_set(label, _("Default debugger"));
   evas_object_size_hint_weight_set(label, 0.0, 0.0);
   evas_object_size_hint_align_set(label, 0.0, EVAS_HINT_FILL);
   elm_table_pack(table, label, 0, 2, 1, 1);
   evas_object_show(label);

   combobox = elm_combobox_add(box);
   if (_edi_project_config->debug_command)
     elm_object_text_set(combobox, _edi_project_config->debug_command);
   else
     elm_object_text_set(combobox, edi_debug_tools_get()[0].name);

   evas_object_size_hint_weight_set(combobox, 0.75, 0.0);
   evas_object_size_hint_align_set(combobox, EVAS_HINT_FILL, EVAS_HINT_FILL);
   evas_object_show(combobox);
   evas_object_smart_callback_add(combobox, "item,pressed",
                                 _edi_settings_builds_debug_pressed_cb, NULL);

   elm_table_pack(table, combobox, 1, 2, 2, 1);

   itc = elm_genlist_item_class_new();
   itc->item_style = "default";
   itc->func.text_get = _edi_settings_builds_debug_tool_text_get_cb;

   tools = edi_debug_tools_get();
   for (i = 0; tools[i].name; i++)
     {
        if (ecore_file_app_installed(tools[i].exec))
          elm_genlist_item_append(combobox, itc, (void *)(uintptr_t) i, NULL, ELM_GENLIST_ITEM_NONE, NULL, (void *)(uintptr_t) i);
     }

   elm_genlist_realized_items_update(combobox);
   elm_genlist_item_class_free(itc);

   return frame;
}

static void
_edi_settings_project_remote_cb(void *data EINA_UNUSED, Evas_Object *obj,
                                void *event EINA_UNUSED)
{
   Evas_Object *entry;
   char *url;

   entry = (Evas_Object *) obj;
   url = _edi_settings_entry_text_get(entry);

   if (!url || strlen(url) == 0)
     {
        free(url);
        return;
     }

   if (!edi_scm_enabled() || edi_scm_remote_enabled())
     {
        free(url);
        return;
     }

   edi_scm_remote_add(url);
   elm_object_disabled_set(entry, EINA_TRUE);
   free(url);
}

static void
_edi_settings_project_agent_enabled_cb(void *data EINA_UNUSED, Evas_Object *obj,
                                       void *event EINA_UNUSED)
{
   _edi_project_config->agent.enabled = elm_check_state_get(obj);
   _edi_project_config_save();
}

static void
_edi_settings_project_agent_edits_enabled_cb(void *data EINA_UNUSED, Evas_Object *obj,
                                             void *event EINA_UNUSED)
{
   _edi_project_config->agent.edits_enabled = elm_check_state_get(obj);
   _edi_project_config_save();
}

static void
_edi_settings_project_agent_model_set(const char *model)
{
   if (_edi_project_config->agent.model)
     eina_stringshare_del(_edi_project_config->agent.model);
   _edi_project_config->agent.model = eina_stringshare_add(model ?: "");
}

static char *
_edi_settings_project_agent_model_text_get_cb(void *data, Evas_Object *obj EINA_UNUSED,
                                              const char *part EINA_UNUSED)
{
   const char *model = data;

   return strdup(model ?: "");
}

static const char *
_edi_settings_project_agent_model_combobox_rebuild(Evas_Object *combobox,
                                                   const char *provider_id,
                                                   const char *selected_model,
                                                   const char **source_models,
                                                   unsigned int source_count)
{
   Elm_Genlist_Item_Class *itc;
   char **models = NULL;
   const char *model;
   unsigned int count = 0;
   unsigned int i;
   if (combobox)
     _edi_settings_project_agent_model_items_clear(combobox);

   if (source_models && source_count > 0)
     {
        models = calloc(source_count, sizeof(char *));
        if (models)
          {
             for (i = 0; i < source_count; i++)
               {
                  models[count] = strdup(source_models[i]);
                  if (models[count])
                    count++;
               }
          }
     }

   model = selected_model;
   if (!model || !model[0] ||
       !edi_agent_provider_model_supported(provider_id, model))
     model = edi_agent_provider_model_default_get(provider_id);
   if ((!model || !model[0]) && count > 0)
     model = models[0];
   if (count == 0 && model && model[0])
     {
        models = calloc(1, sizeof(char *));
        if (models)
          {
             models[0] = strdup(model);
             if (models[0])
               count = 1;
          }
     }

   if (!combobox)
     return model ?: "";

   evas_object_data_set(combobox, "agent_model_items", models);
   evas_object_data_set(combobox, "agent_model_items_count", (void *)(uintptr_t)count);

   elm_genlist_clear(combobox);

   itc = elm_genlist_item_class_new();
   itc->item_style = "default";
   itc->func.text_get = _edi_settings_project_agent_model_text_get_cb;

   for (i = 0; i < count; i++)
     elm_genlist_item_append(combobox, itc, (void *)models[i], NULL,
                             ELM_GENLIST_ITEM_NONE, NULL, (void *)models[i]);

   elm_genlist_realized_items_update(combobox);
   elm_genlist_item_class_free(itc);

   elm_object_text_set(combobox, model ?: "");
   return model ?: "";
}

static void
_edi_settings_project_agent_models_loaded_cb(const char *provider_id,
                                             const char **models, unsigned int count,
                                             Eina_Bool from_remote EINA_UNUSED,
                                             const char *error EINA_UNUSED,
                                             void *data)
{
   Evas_Object *combobox = data;
   const char *saved_model;
   const char *active_model;

   _edi_settings_agent_models_request = NULL;

   if (!combobox)
     {
        edi_agent_models_free(models, count);
        return;
     }

   if (strcmp(_edi_project_config->agent.provider ?: "", provider_id ?: ""))
     {
        edi_agent_models_free(models, count);
        return;
     }

   saved_model = _edi_project_config->agent.model ?: "";
   active_model = _edi_settings_project_agent_model_combobox_rebuild(combobox,
                                                                     provider_id,
                                                                     saved_model,
                                                                     models, count);
   elm_object_disabled_set(combobox, EINA_FALSE);
   if (strcmp(saved_model, active_model ?: ""))
     {
        _edi_settings_project_agent_model_set(active_model);
        _edi_project_config_save();
     }

   edi_agent_models_free(models, count);
}

static const char *
_edi_settings_project_agent_models_refresh(Evas_Object *model_combobox,
                                           const char *provider_id,
                                           const char *selected_model)
{
   Edi_Agent_Model_List fallback;
   const char *active_model;

   if (_edi_settings_agent_models_request)
     {
        edi_agent_provider_models_fetch_cancel(_edi_settings_agent_models_request);
        _edi_settings_agent_models_request = NULL;
     }

   fallback = edi_agent_provider_models_get(provider_id);
   active_model = _edi_settings_project_agent_model_combobox_rebuild(model_combobox,
                                                                     provider_id,
                                                                     selected_model,
                                                                     fallback.models,
                                                                     fallback.count);
   elm_object_disabled_set(model_combobox, EINA_TRUE);
   _edi_settings_agent_models_request = edi_agent_provider_models_fetch(_edi_project_config,
                                                                        provider_id,
                                                                        _edi_settings_project_agent_models_loaded_cb,
                                                                        model_combobox);
   if (!_edi_settings_agent_models_request)
     elm_object_disabled_set(model_combobox, EINA_FALSE);

   return active_model;
}

static void
_edi_settings_project_agent_model_pressed_cb(void *data EINA_UNUSED, Evas_Object *obj,
                                             void *event_info)
{
   const char *model = elm_object_item_data_get(event_info);
   const char *text = elm_object_item_text_get(event_info);

   if (!model || !model[0])
     return;

   _edi_settings_project_agent_model_set(model);
   _edi_project_config_save();
   elm_object_text_set(obj, text ?: model);
   elm_combobox_hover_end(obj);
}

static void
_edi_settings_project_agent_endpoint_cb(void *data EINA_UNUSED, Evas_Object *obj,
                                        void *event EINA_UNUSED)
{
   char *text;

   if (_edi_project_config->agent.endpoint)
     eina_stringshare_del(_edi_project_config->agent.endpoint);

   text = _edi_settings_entry_text_get(obj);
   _edi_project_config->agent.endpoint = eina_stringshare_add(text);
   free(text);
   _edi_project_config_save();
}

static void
_edi_settings_project_agent_api_key_cb(void *data EINA_UNUSED, Evas_Object *obj,
                                       void *event EINA_UNUSED)
{
   char *text;

   if (_edi_project_config->agent.api_key)
     eina_stringshare_del(_edi_project_config->agent.api_key);

   text = _edi_settings_entry_text_get(obj);
   _edi_project_config->agent.api_key = eina_stringshare_add(text);
   free(text);
   _edi_project_config_save();
}

static void
_edi_settings_project_agent_project_id_cb(void *data EINA_UNUSED, Evas_Object *obj,
                                          void *event EINA_UNUSED)
{
   char *text;

   if (_edi_project_config->agent.project_id)
     eina_stringshare_del(_edi_project_config->agent.project_id);

   text = _edi_settings_entry_text_get(obj);
   _edi_project_config->agent.project_id = eina_stringshare_add(text);
   free(text);
   _edi_project_config_save();
}

static void
_edi_settings_project_agent_timeout_cb(void *data EINA_UNUSED, Evas_Object *obj,
                                       void *event EINA_UNUSED)
{
   _edi_project_config->agent.timeout_seconds = elm_spinner_value_get(obj);
   _edi_project_config_save();
}

static void
_edi_settings_project_agent_test_done_cb(const char *response, const char *error, void *data)
{
   Evas_Object *button = data;

   if (button)
     elm_object_disabled_set(button, EINA_FALSE);

   if (error && error[0])
     edi_screens_message(_edi_settings_win, _("AI Agent Test Failed"), error);
   else
     edi_screens_message_icon(_edi_settings_win, _("AI Agent Test OK"),
                              (response && response[0]) ? response : _("Agent replied successfully."),
                              "emblem-default");
}

static void
_edi_settings_project_agent_test_cb(void *data, Evas_Object *obj EINA_UNUSED,
                                    void *event_info EINA_UNUSED)
{
   Evas_Object *button = data;
   char *validation_error;

   validation_error = edi_agent_provider_validate(_edi_project_config);
   if (validation_error)
     {
        edi_screens_message(_edi_settings_win, _("AI Agent Configuration Error"), validation_error);
        free(validation_error);
        return;
     }

   elm_object_disabled_set(button, EINA_TRUE);
   if (!edi_agent_request_send(_("Reply with OK only."), _edi_settings_project_agent_test_done_cb, button))
     {
        elm_object_disabled_set(button, EINA_FALSE);
        edi_screens_message(_edi_settings_win, _("AI Agent Test Failed"),
                            _("Unable to start agent request."));
     }
}

static char *
_edi_settings_project_agent_provider_text_get_cb(void *data, Evas_Object *obj EINA_UNUSED,
                                                 const char *part EINA_UNUSED)
{
   const Edi_Agent_Provider *provider = data;

   return strdup(provider->name);
}

static void
_edi_settings_project_agent_provider_pressed_cb(void *data EINA_UNUSED, Evas_Object *obj,
                                                void *event_info)
{
   const Edi_Agent_Provider *provider = elm_object_item_data_get(event_info);
   const char *text = elm_object_item_text_get(event_info);
   const char *model;
   Evas_Object *model_combobox;
   Evas_Object *endpoint_entry;

   if (!provider)
     return;

   model_combobox = evas_object_data_get(obj, "agent_model_combobox");
   endpoint_entry = evas_object_data_get(obj, "agent_endpoint_entry");

   if (_edi_project_config->agent.provider)
     eina_stringshare_del(_edi_project_config->agent.provider);
   _edi_project_config->agent.provider = eina_stringshare_add(provider->id);

   if (_edi_project_config->agent.endpoint)
     eina_stringshare_del(_edi_project_config->agent.endpoint);
   _edi_project_config->agent.endpoint = eina_stringshare_add(provider->default_endpoint ?: "");
   if (endpoint_entry)
     elm_object_text_set(endpoint_entry, provider->default_endpoint ?: "");

   model = _edi_settings_project_agent_models_refresh(model_combobox, provider->id,
                                                      provider->default_model);
   _edi_settings_project_agent_model_set(model);

   _edi_project_config_save();
   elm_object_text_set(obj, text);
   elm_combobox_hover_end(obj);
}

static void _edi_settings_scm_credentials_set(const char *user_fullname, const char *user_email)
{
   if (!edi_scm_enabled())
     return;

   if (user_fullname && user_fullname[0] && user_email && user_email[0])
     edi_scm_credentials_set(user_fullname, user_email);
}

static void
_edi_settings_project_email_cb(void *data EINA_UNUSED, Evas_Object *obj,
                             void *event EINA_UNUSED)
{
   Evas_Object *entry;
   char *text;

   entry = (Evas_Object *)obj;
   text = _edi_settings_entry_text_get(entry);

   if (_edi_project_config->user_email)
     eina_stringshare_del(_edi_project_config->user_email);

   _edi_project_config->user_email = eina_stringshare_add(text);
   free(text);
   _edi_project_config_save();

   _edi_settings_scm_credentials_set(_edi_project_config->user_fullname, _edi_project_config->user_email);
}

static void
_edi_settings_project_name_cb(void *data EINA_UNUSED, Evas_Object *obj,
                             void *event EINA_UNUSED)
{
   Evas_Object *entry;
   char *text;

   entry = (Evas_Object *)obj;
   text = _edi_settings_entry_text_get(entry);

   if (_edi_project_config->user_fullname)
     eina_stringshare_del(_edi_project_config->user_fullname);

   _edi_project_config->user_fullname = eina_stringshare_add(text);
   free(text);
   _edi_project_config_save();

   _edi_settings_scm_credentials_set(_edi_project_config->user_fullname, _edi_project_config->user_email);
}

static Evas_Object *
_edi_settings_project_create(Evas_Object *parent)
{
   Edi_Scm_Engine *engine = NULL;
   Evas_Object *box, *frames, *frame, *table, *label, *entry_name, *entry_email;
   Evas_Object *scroller;
   Evas_Object *entry_remote, *entry, *check, *combobox, *spinner, *button;
   Evas_Object *combobox_model, *entry_endpoint;
   Elm_Genlist_Item_Class *itc;
   const Edi_Agent_Provider *providers, *provider;
   const char *active_model;
   const char *saved_model;
   unsigned int count, i;
   Eina_Strbuf *text;
   const char *remote_name, *remote_email;

   engine = edi_scm_engine_get();
   if (!engine)
     {
        remote_name = remote_email = "";
     }
   else
     {
        remote_name = engine->remote_name_get();
        remote_email = engine->remote_email_get();
     }

   frames = elm_box_add(parent);
   elm_box_padding_set(frames, 0, EDI_SETTINGS_TABLE_PADDING);
   evas_object_size_hint_weight_set(frames, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
   evas_object_size_hint_align_set(frames, EVAS_HINT_FILL, EVAS_HINT_FILL);
   evas_object_show(frames);
   frame = _edi_settings_panel_create(frames, _("Project Settings"));
   elm_box_pack_end(frames, frame);
   box = elm_object_part_content_get(frame, "default");

   table = elm_table_add(parent);
   evas_object_size_hint_weight_set(table, EVAS_HINT_EXPAND, 0.5);
   evas_object_size_hint_align_set(table, EVAS_HINT_FILL, EVAS_HINT_FILL);
   elm_table_padding_set(table, EDI_SETTINGS_TABLE_PADDING, EDI_SETTINGS_TABLE_PADDING);
   elm_box_pack_end(box, table);
   evas_object_show(table);

   label = elm_label_add(table);
   elm_object_text_set(label, _("Author Name"));
   evas_object_size_hint_weight_set(label, 0.0, 0.0);
   evas_object_size_hint_align_set(label, 0.0, EVAS_HINT_FILL);
   elm_table_pack(table, label, 0, 0, 1, 1);
   evas_object_show(label);

   entry_name = elm_entry_add(table);
   elm_object_text_set(entry_name, _edi_project_config->user_fullname ?: remote_name);
   elm_entry_single_line_set(entry_name, EINA_TRUE);
   elm_entry_scrollable_set(entry_name, EINA_TRUE);
   evas_object_size_hint_weight_set(entry_name, 0.75, 0.0);
   evas_object_size_hint_align_set(entry_name, EVAS_HINT_FILL, EVAS_HINT_FILL);
   elm_table_pack(table, entry_name, 1, 0, 1, 1);
   evas_object_show(entry_name);
   evas_object_smart_callback_add(entry_name, "changed",
                                  _edi_settings_project_name_cb, NULL);

   label = elm_label_add(table);
   elm_object_text_set(label, _("Author E-mail"));
   evas_object_size_hint_weight_set(label, 0.0, 0.0);
   evas_object_size_hint_align_set(label, 0.0, EVAS_HINT_FILL);
   elm_table_pack(table, label, 0, 1, 1, 1);
   evas_object_show(label);

   entry_email = elm_entry_add(table);
   elm_object_text_set(entry_email, _edi_project_config->user_email ?: remote_email);
   elm_entry_single_line_set(entry_email, EINA_TRUE);
   elm_entry_scrollable_set(entry_email, EINA_TRUE);
   evas_object_size_hint_weight_set(entry_email, 0.75, 0.0);
   evas_object_size_hint_align_set(entry_email, EVAS_HINT_FILL, EVAS_HINT_FILL);
   elm_table_pack(table, entry_email, 1, 1, 1, 1);
   evas_object_show(entry_email);
   evas_object_smart_callback_add(entry_email, "changed",
                                  _edi_settings_project_email_cb, NULL);

   if (edi_scm_enabled())
     {
        text = eina_strbuf_new();
        eina_strbuf_append(text, _("Source Control"));
        eina_strbuf_append_printf(text, " (%s)", engine->name);

        frame = _edi_settings_panel_create(frames, eina_strbuf_string_get(text));
        eina_strbuf_free(text);
        elm_box_pack_end(frames, frame);
        box = elm_object_part_content_get(frame, "default");

        table = elm_table_add(parent);
        evas_object_size_hint_weight_set(table, EVAS_HINT_EXPAND, 0.5);
        evas_object_size_hint_align_set(table, EVAS_HINT_FILL, EVAS_HINT_FILL);
        elm_table_padding_set(table, EDI_SETTINGS_TABLE_PADDING, EDI_SETTINGS_TABLE_PADDING);
        elm_box_pack_end(box, table);
        evas_object_show(table);

        label = elm_label_add(table);
        elm_object_text_set(label, _("Remote URL"));
        evas_object_size_hint_weight_set(label, 0.0, 0.0);
        evas_object_size_hint_align_set(label, 0.0, EVAS_HINT_FILL);
        elm_table_pack(table, label, 0, 0, 1, 1);
        evas_object_show(label);

        entry_remote = elm_entry_add(table);
        elm_object_text_set(entry_remote, engine->remote_url_get());
        elm_entry_single_line_set(entry_remote, EINA_TRUE);
        elm_entry_scrollable_set(entry_remote, EINA_TRUE);
        elm_object_disabled_set(entry_remote, edi_scm_remote_enabled());
        evas_object_size_hint_weight_set(entry_remote, 0.75, 0.0);
        evas_object_size_hint_align_set(entry_remote, EVAS_HINT_FILL, EVAS_HINT_FILL);
        elm_table_pack(table, entry_remote, 1, 0, 1, 1);
        evas_object_show(entry_remote);
        evas_object_smart_callback_add(entry_remote, "changed",
                                       _edi_settings_project_remote_cb, NULL);
     }

   frame = _edi_settings_panel_create(frames, _("AI Agents"));
   evas_object_size_hint_weight_set(frame, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
   evas_object_size_hint_align_set(frame, EVAS_HINT_FILL, EVAS_HINT_FILL);
   elm_box_pack_end(frames, frame);
   box = elm_object_part_content_get(frame, "default");
   evas_object_size_hint_weight_set(box, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
   evas_object_size_hint_align_set(box, EVAS_HINT_FILL, EVAS_HINT_FILL);

   table = elm_table_add(parent);
   evas_object_size_hint_weight_set(table, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
   evas_object_size_hint_align_set(table, EVAS_HINT_FILL, EVAS_HINT_FILL);
   elm_table_padding_set(table, EDI_SETTINGS_TABLE_PADDING, EDI_SETTINGS_TABLE_PADDING);
   elm_box_pack_end(box, table);
   evas_object_show(table);

   check = elm_check_add(table);
   elm_object_text_set(check, _("Enable AI agent"));
   elm_check_state_set(check, _edi_project_config->agent.enabled);
   evas_object_size_hint_weight_set(check, EVAS_HINT_EXPAND, 0.0);
   evas_object_size_hint_align_set(check, EVAS_HINT_FILL, 0.5);
   elm_table_pack(table, check, 0, 0, 1, 1);
   evas_object_smart_callback_add(check, "changed",
                                  _edi_settings_project_agent_enabled_cb, NULL);
   evas_object_show(check);

   check = elm_check_add(table);
   elm_object_text_set(check, _("Enable AI Edits (beta)"));
   elm_check_state_set(check, _edi_project_config->agent.edits_enabled);
   evas_object_size_hint_weight_set(check, EVAS_HINT_EXPAND, 0.0);
   evas_object_size_hint_align_set(check, EVAS_HINT_FILL, 0.5);
   elm_table_pack(table, check, 1, 0, 1, 1);
   evas_object_smart_callback_add(check, "changed",
                                  _edi_settings_project_agent_edits_enabled_cb, NULL);
   evas_object_show(check);

   label = elm_label_add(table);
   elm_object_text_set(label, _("Provider"));
   evas_object_size_hint_weight_set(label, 0.0, 0.0);
   evas_object_size_hint_align_set(label, 0.0, EVAS_HINT_FILL);
   elm_table_pack(table, label, 0, 1, 1, 1);
   evas_object_show(label);

   combobox = elm_combobox_add(table);
   evas_object_size_hint_weight_set(combobox, EVAS_HINT_EXPAND, 0.0);
   evas_object_size_hint_align_set(combobox, EVAS_HINT_FILL, EVAS_HINT_FILL);
   evas_object_show(combobox);
   evas_object_smart_callback_add(combobox, "item,pressed",
                                  _edi_settings_project_agent_provider_pressed_cb, NULL);
   elm_table_pack(table, combobox, 1, 1, 1, 1);

   providers = edi_agent_providers_get(&count);
   provider = edi_agent_provider_current_get();
   elm_object_text_set(combobox, provider->name);

   itc = elm_genlist_item_class_new();
   itc->item_style = "default";
   itc->func.text_get = _edi_settings_project_agent_provider_text_get_cb;

   for (i = 0; i < count; i++)
     elm_genlist_item_append(combobox, itc, (void *)&providers[i], NULL,
                             ELM_GENLIST_ITEM_NONE, NULL, (void *)&providers[i]);

   elm_genlist_realized_items_update(combobox);
   elm_genlist_item_class_free(itc);

   label = elm_label_add(table);
   elm_object_text_set(label, _("Model"));
   evas_object_size_hint_weight_set(label, 0.0, 0.0);
   evas_object_size_hint_align_set(label, 0.0, EVAS_HINT_FILL);
   elm_table_pack(table, label, 0, 2, 1, 1);
   evas_object_show(label);

   combobox_model = elm_combobox_add(table);
   evas_object_size_hint_weight_set(combobox_model, EVAS_HINT_EXPAND, 0.0);
   evas_object_size_hint_align_set(combobox_model, EVAS_HINT_FILL, EVAS_HINT_FILL);
   evas_object_show(combobox_model);
   evas_object_smart_callback_add(combobox_model, "item,pressed",
                                  _edi_settings_project_agent_model_pressed_cb, NULL);
   elm_table_pack(table, combobox_model, 1, 2, 1, 1);
   _edi_settings_agent_model_combobox = combobox_model;

   saved_model = _edi_project_config->agent.model ?: "";
   active_model = _edi_settings_project_agent_models_refresh(combobox_model,
                                                             provider->id,
                                                             saved_model);
   if (strcmp(saved_model, active_model ?: ""))
     {
        _edi_settings_project_agent_model_set(active_model);
        _edi_project_config_save();
     }

   label = elm_label_add(table);
   elm_object_text_set(label, _("Endpoint"));
   evas_object_size_hint_weight_set(label, 0.0, 0.0);
   evas_object_size_hint_align_set(label, 0.0, EVAS_HINT_FILL);
   elm_table_pack(table, label, 0, 3, 1, 1);
   evas_object_show(label);

   entry_endpoint = elm_entry_add(table);
   elm_object_text_set(entry_endpoint, _edi_project_config->agent.endpoint ?: "");
   elm_entry_single_line_set(entry_endpoint, EINA_TRUE);
   elm_entry_scrollable_set(entry_endpoint, EINA_TRUE);
   evas_object_size_hint_weight_set(entry_endpoint, EVAS_HINT_EXPAND, 0.0);
   evas_object_size_hint_align_set(entry_endpoint, EVAS_HINT_FILL, EVAS_HINT_FILL);
   elm_table_pack(table, entry_endpoint, 1, 3, 1, 1);
   evas_object_show(entry_endpoint);
   evas_object_smart_callback_add(entry_endpoint, "changed",
                                  _edi_settings_project_agent_endpoint_cb, NULL);

   evas_object_data_set(combobox, "agent_model_combobox", combobox_model);
   evas_object_data_set(combobox, "agent_endpoint_entry", entry_endpoint);

   label = elm_label_add(table);
   elm_object_text_set(label, _("API Key / Token"));
   evas_object_size_hint_weight_set(label, 0.0, 0.0);
   evas_object_size_hint_align_set(label, 0.0, EVAS_HINT_FILL);
   elm_table_pack(table, label, 0, 4, 1, 1);
   evas_object_show(label);

   entry = elm_entry_add(table);
   elm_object_text_set(entry, _edi_project_config->agent.api_key ?: "");
   elm_entry_single_line_set(entry, EINA_TRUE);
   elm_entry_scrollable_set(entry, EINA_TRUE);
   elm_entry_password_set(entry, EINA_TRUE);
   evas_object_size_hint_weight_set(entry, EVAS_HINT_EXPAND, 0.0);
   evas_object_size_hint_align_set(entry, EVAS_HINT_FILL, EVAS_HINT_FILL);
   elm_table_pack(table, entry, 1, 4, 1, 1);
   evas_object_show(entry);
   evas_object_smart_callback_add(entry, "changed",
                                  _edi_settings_project_agent_api_key_cb, NULL);

   label = elm_label_add(table);
   elm_object_text_set(label, _("Project ID (Google only)"));
   evas_object_size_hint_weight_set(label, 0.0, 0.0);
   evas_object_size_hint_align_set(label, 0.0, EVAS_HINT_FILL);
   elm_table_pack(table, label, 0, 5, 1, 1);
   evas_object_show(label);

   entry = elm_entry_add(table);
   elm_object_text_set(entry, _edi_project_config->agent.project_id ?: "");
   elm_entry_single_line_set(entry, EINA_TRUE);
   elm_entry_scrollable_set(entry, EINA_TRUE);
   evas_object_size_hint_weight_set(entry, EVAS_HINT_EXPAND, 0.0);
   evas_object_size_hint_align_set(entry, EVAS_HINT_FILL, EVAS_HINT_FILL);
   elm_table_pack(table, entry, 1, 5, 1, 1);
   evas_object_show(entry);
   evas_object_smart_callback_add(entry, "changed",
                                  _edi_settings_project_agent_project_id_cb, NULL);

   label = elm_label_add(table);
   elm_object_text_set(label, _("Timeout (seconds)"));
   evas_object_size_hint_weight_set(label, 0.0, 0.0);
   evas_object_size_hint_align_set(label, 0.0, EVAS_HINT_FILL);
   elm_table_pack(table, label, 0, 6, 1, 1);
   evas_object_show(label);

   spinner = elm_spinner_add(table);
   elm_spinner_min_max_set(spinner, 5.0, 300.0);
   elm_spinner_step_set(spinner, 1.0);
   elm_spinner_editable_set(spinner, EINA_TRUE);
   elm_spinner_wrap_set(spinner, EINA_FALSE);
   elm_spinner_value_set(spinner, _edi_project_config->agent.timeout_seconds > 0.0 ?
                                  _edi_project_config->agent.timeout_seconds : 30.0);
   evas_object_size_hint_weight_set(spinner, EVAS_HINT_EXPAND, 0.0);
   evas_object_size_hint_align_set(spinner, EVAS_HINT_FILL, EVAS_HINT_FILL);
   elm_table_pack(table, spinner, 1, 6, 1, 1);
   evas_object_show(spinner);
   evas_object_smart_callback_add(spinner, "changed",
                                  _edi_settings_project_agent_timeout_cb, NULL);

   button = elm_button_add(table);
   elm_object_text_set(button, _("Test Connection"));
   evas_object_size_hint_weight_set(button, EVAS_HINT_EXPAND, 0.0);
   evas_object_size_hint_align_set(button, EVAS_HINT_FILL, EVAS_HINT_FILL);
   elm_table_pack(table, button, 1, 7, 1, 1);
   evas_object_show(button);
   evas_object_smart_callback_add(button, "clicked",
                                  _edi_settings_project_agent_test_cb, button);

   scroller = elm_scroller_add(parent);
   elm_scroller_policy_set(scroller, ELM_SCROLLER_POLICY_OFF, ELM_SCROLLER_POLICY_AUTO);
   elm_scroller_bounce_set(scroller, EINA_FALSE, EINA_TRUE);
   evas_object_size_hint_weight_set(scroller, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
   evas_object_size_hint_align_set(scroller, EVAS_HINT_FILL, EVAS_HINT_FILL);
   elm_object_content_set(scroller, frames);
   evas_object_show(scroller);

   return scroller;
}

static void
_edi_settings_behaviour_show_hidden_cb(void *data EINA_UNUSED, Evas_Object *obj,
                                       void *event EINA_UNUSED)
{
   Evas_Object *check;

   check = (Evas_Object *) obj;
   _edi_config->show_hidden = elm_check_state_get(check);
   _edi_config_save();
   edi_filepanel_refresh_all();
}


static void
_edi_settings_behaviour_autosave_cb(void *data EINA_UNUSED, Evas_Object *obj,
                                    void *event EINA_UNUSED)
{
   Evas_Object *check;

   check = (Evas_Object *)obj;
   _edi_config->autosave = elm_check_state_get(check);
   _edi_config_save();
}

static void
_edi_settings_behaviour_trim_whitespace_cb(void *data EINA_UNUSED, Evas_Object *obj,
                                           void *event EINA_UNUSED)
{
   Evas_Object *check;

   check = (Evas_Object *)obj;
   _edi_config->trim_whitespace = elm_check_state_get(check);
   _edi_config_save();
}

static Evas_Object *
_edi_settings_behaviour_create(Evas_Object *parent)
{
   Evas_Object *box, *frame, *check;

   frame = _edi_settings_panel_create(parent, _("Behaviour"));
   box = elm_object_part_content_get(frame, "default");

   check = elm_check_add(box);
   elm_object_text_set(check, _("Auto save files"));
   elm_check_state_set(check, _edi_config->autosave);
   evas_object_size_hint_align_set(check, EVAS_HINT_FILL, 0.5);
   elm_box_pack_end(box, check);
   evas_object_smart_callback_add(check, "changed",
                                  _edi_settings_behaviour_autosave_cb, NULL);
   evas_object_show(check);

   elm_object_focus_set(check, EINA_TRUE);

   check = elm_check_add(box);
   elm_object_text_set(check, _("Trim trailing whitespace"));
   elm_check_state_set(check, _edi_config->trim_whitespace);
   evas_object_size_hint_align_set(check, EVAS_HINT_FILL, 0.5);
   elm_box_pack_end(box, check);
   evas_object_smart_callback_add(check, "changed",
                                  _edi_settings_behaviour_trim_whitespace_cb, NULL);
   evas_object_show(check);

   check = elm_check_add(box);
   elm_object_text_set(check, _("Show hidden files"));
   elm_check_state_set(check, _edi_config->show_hidden);
   evas_object_size_hint_align_set(check, EVAS_HINT_FILL, 0.5);
   elm_box_pack_end(box, check);
   evas_object_smart_callback_add(check, "changed",
                                  _edi_settings_behaviour_show_hidden_cb, NULL);
   evas_object_show(check);

   return frame;
}

Evas_Object *
edi_settings_win_get(void)
{
   return _edi_settings_win;
}

Evas_Object *
edi_settings_show(Evas_Object *mainwin, Edi_Settings_Tab type)
{
   Evas_Object *win, *bg, *table, *naviframe, *tb;
   Elm_Object_Item *tb_it, *default_it;
   Elm_Object_Item *it_project, *it_display, *it_behaviour, *it_builds;
   Eina_Bool project_mode;

   it_project = it_display = it_behaviour = it_builds = NULL;

   if (edi_settings_win_get())
     return NULL;

   _edi_settings_win = win = elm_win_add(mainwin, "settings", ELM_WIN_BASIC);
   if (!win) return NULL;
   _edi_settings_font_preview_code = NULL;

   elm_win_title_set(win, _("Edi Settings"));
   evas_object_smart_callback_add(win, "delete,request", _edi_settings_exit, win);
   _edi_settings_config_handler = ecore_event_handler_add(EDI_EVENT_CONFIG_CHANGED,
                                                          _edi_settings_config_changed_cb, NULL);

   bg = elm_bg_add(win);
   evas_object_size_hint_weight_set(bg, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
   evas_object_size_hint_align_set(bg, EVAS_HINT_FILL, EVAS_HINT_FILL);
   elm_win_resize_object_add(win, bg);
   evas_object_show(bg);

   table = elm_table_add(bg);
   evas_object_size_hint_weight_set(table, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
   evas_object_size_hint_align_set(table, EVAS_HINT_FILL, EVAS_HINT_FILL);
   elm_win_resize_object_add(win, table);
   evas_object_show(table);

   tb = elm_toolbar_add(table);
   elm_toolbar_homogeneous_set(tb, EINA_FALSE);
   elm_toolbar_shrink_mode_set(tb, ELM_TOOLBAR_SHRINK_SCROLL);
   elm_toolbar_select_mode_set(tb, ELM_OBJECT_SELECT_MODE_ALWAYS);
   elm_toolbar_align_set(tb, 0.0);
   elm_toolbar_horizontal_set(tb, EINA_FALSE);
   evas_object_size_hint_weight_set(tb, 0.0, EVAS_HINT_EXPAND);
   evas_object_size_hint_align_set(tb, 0.0, EVAS_HINT_FILL);
   elm_table_pack(table, tb, 0, 0, 1, 5);
   evas_object_show(tb);

   naviframe = elm_naviframe_add(table);
   evas_object_size_hint_weight_set(naviframe, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
   evas_object_size_hint_align_set(naviframe, EVAS_HINT_FILL, EVAS_HINT_FILL);
   elm_table_pack(table, naviframe, 1, 0, 4, 5);

   _edi_settings_project = elm_naviframe_item_push(naviframe, "", NULL, NULL,
                                                  _edi_settings_project_create(naviframe), NULL);
   elm_naviframe_item_title_enabled_set(_edi_settings_project, EINA_FALSE, EINA_FALSE);
   _edi_settings_display = elm_naviframe_item_push(naviframe, "", NULL, NULL,
                                                   _edi_settings_display_create(naviframe), NULL);
   elm_naviframe_item_title_enabled_set(_edi_settings_display, EINA_FALSE, EINA_FALSE);
   _edi_settings_builds = elm_naviframe_item_push(naviframe, "", NULL, NULL,
                                                   _edi_settings_builds_create(naviframe), NULL);
   elm_naviframe_item_title_enabled_set(_edi_settings_builds, EINA_FALSE, EINA_FALSE);
   _edi_settings_behaviour = elm_naviframe_item_push(naviframe, "", NULL, NULL,
                                                   _edi_settings_behaviour_create(naviframe), NULL);
   elm_naviframe_item_title_enabled_set(_edi_settings_behaviour, EINA_FALSE, EINA_FALSE);

   project_mode = edi_project_mode_get();

   it_display = elm_toolbar_item_append(tb, "preferences-desktop", _("Display"), _edi_settings_category_cb, _edi_settings_display);
   if (project_mode)
     it_project = elm_toolbar_item_append(tb, "applications-development", _("Project"),_edi_settings_category_cb, _edi_settings_project);
   if (project_mode)
     it_builds = elm_toolbar_item_append(tb, "system-run", _("Builds"), _edi_settings_category_cb, _edi_settings_builds);

   tb_it = elm_toolbar_item_append(tb, NULL, NULL, NULL, NULL);
   elm_toolbar_item_separator_set(tb_it, EINA_TRUE);
   tb_it = elm_toolbar_item_append(tb, "application-internet", _("Global"), NULL, NULL);
   elm_object_item_disabled_set(tb_it, EINA_TRUE);
   elm_object_item_disabled_set(tb, EINA_FALSE);

   it_behaviour = elm_toolbar_item_append(tb, "preferences-other", _("Behaviour"),
                                         _edi_settings_category_cb, _edi_settings_behaviour);

   switch (type)
     {
        case EDI_SETTINGS_TAB_DISPLAY:
          default_it = it_display;
          break;
        case EDI_SETTINGS_TAB_PROJECT:
          default_it = it_project;
          break;
        case EDI_SETTINGS_TAB_BEHAVIOUR:
          default_it = it_behaviour;
          break;
        case EDI_SETTINGS_TAB_BUILDS:
          default_it = it_builds;
          break;
     }

   if (default_it)
     {
        elm_toolbar_item_selected_set(default_it, EINA_TRUE);
        _edi_settings_toolbar_single_select(tb, default_it);
     }

   evas_object_show(naviframe);
   evas_object_resize(win, 480 * elm_config_scale_get(), 360 * elm_config_scale_get());
   evas_object_show(win);

   return win;
}

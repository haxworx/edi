#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <Elementary.h>
#include <Ecore.h>
#include <stdio.h>

#include "edi_private.h"

static const char *_EDI_ABOUT_GPLV2_TEXT =
"GNU GENERAL PUBLIC LICENSE\n"
"Version 2, June 1991\n"
"\n"
"Copyright (C) 1989, 1991 Free Software Foundation, Inc.\n"
"51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.\n"
"\n"
"This program is free software; you can redistribute it and/or modify\n"
"it under the terms of the GNU General Public License as published by\n"
"the Free Software Foundation; version 2 of the License.\n"
"\n"
"This program is distributed in the hope that it will be useful,\n"
"but WITHOUT ANY WARRANTY; without even the implied warranty of\n"
"MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the\n"
"GNU General Public License for more details.\n"
"\n"
"You should have received a copy of the GNU General Public License\n"
"along with this program; if not, write to the Free Software Foundation,\n"
"Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.\n"
"\n"
"Source: https://www.gnu.org/licenses/old-licenses/gpl-2.0.txt\n";

typedef struct _Edi_About_Scroll_Data
{
   Evas_Object *scroller;
   Evas_Object *top_spacer;
   Evas_Object *bottom_spacer;
   Ecore_Animator *animator;
   double pos;
} Edi_About_Scroll_Data;

static Eina_Bool
_edi_about_scroll_animate_cb(void *data)
{
   Edi_About_Scroll_Data *sd = data;
   Evas_Coord x, y, w, h, cw, ch;
   double max_y;

   if (!sd || !sd->scroller) return ECORE_CALLBACK_CANCEL;

   elm_scroller_region_get(sd->scroller, &x, &y, &w, &h);
   elm_scroller_child_size_get(sd->scroller, &cw, &ch);

   if (h <= 0 || ch <= h)
     return ECORE_CALLBACK_RENEW;

   max_y = ch - h;
   sd->pos += 24.0 * ecore_animator_frametime_get();
   if (sd->pos > max_y)
     sd->pos = 0.0;

   elm_scroller_region_show(sd->scroller, 0, (int) sd->pos, w, h);
   return ECORE_CALLBACK_RENEW;
}

static void
_edi_about_scroll_resize_cb(void *data, Evas *e EINA_UNUSED, Evas_Object *obj EINA_UNUSED,
                            void *event_info EINA_UNUSED)
{
   Edi_About_Scroll_Data *sd = data;
   Evas_Coord h;

   if (!sd || !sd->scroller) return;

   evas_object_geometry_get(sd->scroller, NULL, NULL, NULL, &h);
   if (h <= 0) return;

   evas_object_size_hint_min_set(sd->top_spacer, 0, h);
   evas_object_size_hint_min_set(sd->bottom_spacer, 0, h);
}

static void
_edi_about_scroll_data_del_cb(void *data, Evas *e EINA_UNUSED, Evas_Object *obj EINA_UNUSED,
                              void *event_info EINA_UNUSED)
{
   Edi_About_Scroll_Data *sd = data;

   if (!sd) return;
   if (sd->animator) ecore_animator_del(sd->animator);
   free(sd);
}

static void
_edi_about_exit(void *data, Evas_Object *obj EINA_UNUSED, void *event_info EINA_UNUSED)
{
   evas_object_del(data);
}

static void
_edi_about_close_cb(void *data, Evas_Object *obj EINA_UNUSED, void *event_info EINA_UNUSED)
{
   Evas_Object *win = data;

   evas_object_del(win);
}

static char *
_edi_about_file_read(const char *path)
{
   FILE *f;
   long len;
   char *buf;

   f = fopen(path, "rb");
   if (!f) return NULL;
   if (fseek(f, 0, SEEK_END) != 0)
     {
        fclose(f);
        return NULL;
     }

   len = ftell(f);
   if (len < 0 || fseek(f, 0, SEEK_SET) != 0)
     {
        fclose(f);
        return NULL;
     }

   buf = calloc(1, len + 1);
   if (!buf)
     {
        fclose(f);
        return NULL;
     }

   if (len > 0 && fread(buf, 1, len, f) != (size_t) len)
     {
        free(buf);
        fclose(f);
        return NULL;
     }

   fclose(f);
   return buf;
}

Evas_Object *
edi_about_show(Evas_Object *mainwin)
{
   Evas_Object *win, *vbox, *box, *table, *bg;
   Evas_Object *text, *credits, *credits_box, *buttonbox, *button, *space;
   Evas_Object *top_spacer, *bottom_spacer;
   Edi_About_Scroll_Data *scroll;
   const char *title_text;
   char *authors_text, *markup;
   Eina_Strbuf *combined;
   int alpha, r, g, b;
   char buf[PATH_MAX];

   win = elm_win_add(mainwin, "about", ELM_WIN_BASIC);
   if (!win) return NULL;

   title_text = eina_slstr_printf(_("About Edi %s"), PACKAGE_VERSION);
   elm_win_title_set(win, title_text);
   evas_object_smart_callback_add(win, "delete,request", _edi_about_exit, win);

   table = elm_table_add(win);
   evas_object_size_hint_weight_set(table, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
   evas_object_size_hint_align_set(table, EVAS_HINT_FILL, EVAS_HINT_FILL);
   elm_win_resize_object_add(win, table);
   evas_object_show(table);

   bg = elm_bg_add(win);
   evas_object_color_set(bg, 26, 26, 26, 255);
   evas_object_size_hint_weight_set(bg, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
   evas_object_size_hint_align_set(bg, EVAS_HINT_FILL, EVAS_HINT_FILL);
   elm_table_pack(table, bg, 0, 0, 1, 1);
   evas_object_show(bg);

   snprintf(buf, sizeof(buf), "%s/images/about.png", elm_app_data_dir_get());
   bg = elm_image_add(win);
   elm_image_fill_outside_set(bg, EINA_TRUE);
   elm_image_file_set(bg, buf, NULL);
   evas_object_size_hint_weight_set(bg, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
   evas_object_size_hint_align_set(bg, EVAS_HINT_FILL, EVAS_HINT_FILL);
   elm_table_pack(table, bg, 0, 0, 1, 1);

   evas_object_color_get(bg, &r, &g, &b, &alpha);
   evas_color_argb_unpremul(alpha, &r, &g, &b);
   alpha = 64;

   evas_color_argb_premul(alpha, &r, &g, &b);
   evas_object_color_set(bg, r, g, b, alpha);
   evas_object_show(bg);

   vbox = elm_box_add(win);
   elm_box_padding_set(vbox, 25, 0);
   elm_box_horizontal_set(vbox, EINA_TRUE);
   evas_object_size_hint_weight_set(vbox, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
   evas_object_size_hint_align_set(vbox, EVAS_HINT_FILL, EVAS_HINT_FILL);
   elm_table_pack(table, vbox, 0, 0, 1, 1);
   evas_object_show(vbox);

   elm_box_pack_end(vbox, elm_box_add(vbox));
   box = elm_box_add(win);
   elm_box_padding_set(box, 10, 0);
   evas_object_size_hint_weight_set(box, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
   evas_object_size_hint_align_set(box, EVAS_HINT_FILL, EVAS_HINT_FILL);
   elm_box_pack_end(vbox, box);
   evas_object_show(box);
   elm_box_pack_end(vbox, elm_box_add(vbox));

   text = elm_label_add(box);
   elm_object_text_set(text, _("<br>EDI is an IDE designed to get people into coding for Unix.<br>" \
                             "It's based on the <b>EFL</b> and written completely natively<br>" \
                             "to provide a <i>responsive</i> and <i>beautiful</i> UI.<br>"));
   evas_object_size_hint_weight_set(text, EVAS_HINT_EXPAND, 0.0);
   evas_object_size_hint_align_set(text, EVAS_HINT_FILL, EVAS_HINT_FILL);
   elm_box_pack_end(box, text);
   evas_object_show(text);

   credits = elm_scroller_add(box);
   elm_scroller_policy_set(credits, ELM_SCROLLER_POLICY_OFF, ELM_SCROLLER_POLICY_OFF);
   elm_scroller_bounce_set(credits, EINA_FALSE, EINA_FALSE);
   evas_object_size_hint_weight_set(credits, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
   evas_object_size_hint_align_set(credits, EVAS_HINT_FILL, EVAS_HINT_FILL);
   elm_box_pack_end(box, credits);
   evas_object_show(credits);

   credits_box = elm_box_add(credits);
   evas_object_size_hint_weight_set(credits_box, EVAS_HINT_EXPAND, 0.0);
   evas_object_size_hint_align_set(credits_box, EVAS_HINT_FILL, 0.0);
   elm_object_content_set(credits, credits_box);
   evas_object_show(credits_box);

   top_spacer = elm_box_add(credits_box);
   evas_object_size_hint_weight_set(top_spacer, EVAS_HINT_EXPAND, 0.0);
   evas_object_size_hint_align_set(top_spacer, EVAS_HINT_FILL, 0.0);
   elm_box_pack_end(credits_box, top_spacer);
   evas_object_show(top_spacer);

   text = elm_entry_add(credits_box);
   elm_entry_line_wrap_set(text, EINA_FALSE);
   elm_entry_text_style_user_push(text, "DEFAULT='font=Mono')");
   elm_entry_editable_set(text, EINA_FALSE);
   elm_object_focus_allow_set(text, EINA_FALSE);
   evas_object_size_hint_weight_set(text, EVAS_HINT_EXPAND, 0.0);
   evas_object_size_hint_align_set(text, EVAS_HINT_FILL, 0.0);

   authors_text = _edi_about_file_read(PACKAGE_DOC_DIR "/AUTHORS");
   combined = eina_strbuf_new();
   eina_strbuf_append(combined, "AUTHORS\n\n");
   eina_strbuf_append(combined, authors_text ? authors_text : "");
   eina_strbuf_append(combined, "\n\nGPLv2 LICENSE\n\n");
   eina_strbuf_append(combined, _EDI_ABOUT_GPLV2_TEXT);
   markup = elm_entry_utf8_to_markup(eina_strbuf_string_get(combined));
   elm_object_text_set(text, markup);
   free(markup);
   eina_strbuf_free(combined);
   free(authors_text);
   elm_box_pack_end(credits_box, text);
   evas_object_show(text);

   bottom_spacer = elm_box_add(credits_box);
   evas_object_size_hint_weight_set(bottom_spacer, EVAS_HINT_EXPAND, 0.0);
   evas_object_size_hint_align_set(bottom_spacer, EVAS_HINT_FILL, 0.0);
   elm_box_pack_end(credits_box, bottom_spacer);
   evas_object_show(bottom_spacer);

   scroll = calloc(1, sizeof(*scroll));
   scroll->scroller = credits;
   scroll->top_spacer = top_spacer;
   scroll->bottom_spacer = bottom_spacer;
   scroll->pos = 0.0;
   scroll->animator = ecore_animator_add(_edi_about_scroll_animate_cb, scroll);
   evas_object_event_callback_add(credits, EVAS_CALLBACK_RESIZE, _edi_about_scroll_resize_cb, scroll);
   evas_object_event_callback_add(win, EVAS_CALLBACK_DEL, _edi_about_scroll_data_del_cb, scroll);

   buttonbox = elm_box_add(box);
   elm_box_horizontal_set(buttonbox, EINA_TRUE);
   evas_object_size_hint_weight_set(box, EVAS_HINT_EXPAND, 0.0);
   evas_object_size_hint_align_set(box, EVAS_HINT_FILL, EVAS_HINT_FILL);
   elm_box_pack_end(box, buttonbox);
   evas_object_show(buttonbox);

   space = elm_box_add(box);
   evas_object_size_hint_min_set(space, 0, 14 * elm_config_scale_get());
   elm_box_pack_end(box, space);
   evas_object_show(space);

   button = elm_button_add(box);
   elm_object_text_set(button, _("Close"));
   evas_object_smart_callback_add(button, "clicked", _edi_about_close_cb, win);
   elm_box_pack_end(buttonbox, button);
   evas_object_show(button);

   space = elm_box_add(box);
   evas_object_size_hint_min_set(space, 20 * elm_config_scale_get(), 0);
   elm_box_pack_end(buttonbox, space);
   evas_object_show(space);

   evas_object_resize(win, 520 * elm_config_scale_get(), 360 * elm_config_scale_get());
   evas_object_show(win);

   return win;
}

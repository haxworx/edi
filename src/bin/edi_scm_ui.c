#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <ctype.h>

#include "Edi.h"
#include <Eio.h>
#include <Ecore_File.h>
#include "edi_scm_ui.h"
#include "edi_private.h"

#define DEFAULT_USER_ICON "applications-development"
#define AVATAR_CACHE_MAX_AGE (24 * 60 * 60)

typedef struct _Edi_Scm_Ui_Data {
   Ecore_Thread *thread;
   Eio_Monitor  *monitor;
   Elm_Code     *code;
   const char   *workdir;
   void         *data;

   Eina_Bool is_configured;
   Eina_Bool in_progress;

   Evas_Object *parent;
   Evas_Object *staged_list, *unstaged_list;
   Evas_Object *commit_button;
   Evas_Object *commit_entry;

} Edi_Scm_Ui_Data;

typedef struct _Edi_Scm_Show_Data {
   Elm_Code *code;
   Evas_Object *widget;
   const char *commit;
   const char *path;
   Eina_Bool diff;
} Edi_Scm_Show_Data;

typedef struct _Edi_Scm_Log_Item {
   char *hash;
   char *author_name;
   char *author_email;
   char *date;
   char *title;
   char *body;
   char *display_text;
} Edi_Scm_Log_Item;

typedef struct _Edi_Scm_Log_View_Data {
   Evas_Object *list;
   Evas_Object *search;
   Eina_List *items;
} Edi_Scm_Log_View_Data;

static Elm_Genlist_Item_Class _edi_scm_log_itc;

static Eina_Bool
_edi_scm_ui_commit_hash_valid(const char *hash)
{
   size_t len;

   if (!hash)
     return EINA_FALSE;

   len = strlen(hash);
   if (len < 7 || len > 64)
     return EINA_FALSE;

   for (size_t i = 0; i < len; i++)
     {
        if (!isxdigit((unsigned char)hash[i]))
          return EINA_FALSE;
     }

   return EINA_TRUE;
}

const char *
_edi_scm_ui_avatar_cache_path_get(const char *email)
{
   return eina_stringshare_printf("%s/%s/avatars/%s.png", efreet_cache_home_get(),
                                  PACKAGE_NAME, email);
}

void _edi_scm_ui_screens_avatar_download_complete(void *data, const char *file,
                                                  int status)
{
   Evas_Object *image = data;

   if (status < 200 || status > 299)
     {
        ecore_file_remove(file);
        return;
     }

   if (!elm_photo_file_set(image, file))
     ecore_file_remove(file);
}

static void
_edi_scm_ui_screens_avatar_load(Evas_Object *image, const char *email)
{
   char *tmp, *tmp2;
   const char *cache, *cachedir, *cacheparentdir, *oldcache;
   long int age;

   cache = _edi_scm_ui_avatar_cache_path_get(email);
   if (ecore_file_exists(cache))
     {
        age = ecore_file_mod_time(cache);
        if (age > 0)
          age = ecore_time_unix_get() - age;

        if (age > 0 && age < AVATAR_CACHE_MAX_AGE && elm_photo_file_set(image, cache))
          return;
     }
   else
     {
        oldcache = eina_stringshare_printf("%s/%s/avatars/%s.jpeg", efreet_cache_home_get(),
                                           PACKAGE_NAME, email);
        if (ecore_file_exists(oldcache))
          {
             if (elm_photo_file_set(image, oldcache))
               {
                  eina_stringshare_del(oldcache);
                  return;
              }

             ecore_file_remove(oldcache);
          }
        eina_stringshare_del(oldcache);
     }

   tmp = strdup(cache);
   cachedir = dirname(tmp);
   tmp2 = strdup(tmp);
   cacheparentdir = dirname(tmp2);
   if ((ecore_file_exists(cacheparentdir) || ecore_file_mkdir(cacheparentdir))
       && (ecore_file_exists(cachedir) || ecore_file_mkdir(cachedir)))
     {
        if (!ecore_file_download(edi_scm_avatar_url_get(email), cache,
                                 _edi_scm_ui_screens_avatar_download_complete, NULL,
                                 image, NULL))
          {
             if (ecore_file_exists(cache))
               elm_photo_file_set(image, cache);
             else
               elm_icon_standard_set(image, DEFAULT_USER_ICON);
          }
     }
   else
     {
        elm_icon_standard_set(image, DEFAULT_USER_ICON);
     }

   free(tmp2);
   free(tmp);
}

static void
_edi_scm_ui_screens_message_close_cb(void *data EINA_UNUSED,
                                     Evas_Object *obj EINA_UNUSED,
                                     void *event_info EINA_UNUSED)
{
   Evas_Object *popup = data;

   evas_object_del(popup);
}

static void
_edi_scm_ui_screens_message_open(Evas_Object *parent, const char *message)
{
   Evas_Object *popup, *button;

   popup = elm_popup_add(parent);
   elm_object_part_text_set(popup, "title,text",
                            message);

   button = elm_button_add(popup);
   elm_object_text_set(button, _("OK"));
   elm_object_part_content_set(popup, "button1", button);
   evas_object_smart_callback_add(button, "clicked",
                                  _edi_scm_ui_screens_message_close_cb, popup);

   evas_object_show(popup);
}

static void
_edi_scm_ui_screens_cancel_cb(void *data, Evas_Object *obj EINA_UNUSED,
                              void *event_info EINA_UNUSED)
{
   Edi_Scm_Ui_Data *pd = data;

   if (pd->thread)
     ecore_thread_cancel(pd->thread);

   while ((ecore_thread_wait(pd->thread, 0.1)) != EINA_TRUE);

   evas_object_del(pd->parent);

   if (pd->monitor)
     eio_monitor_del(pd->monitor);

   free(pd);

   elm_exit();
}

static void
_edi_scm_ui_screens_commit_cb(void *data,
                              Evas_Object *obj EINA_UNUSED,
                              void *event_info EINA_UNUSED)
{
   Edi_Scm_Engine *engine;
   Edi_Scm_Ui_Data *pd;
   const char *text;
   char *message;

   engine = edi_scm_engine_get();
   if (!engine)
     return;

   pd = data;

   text = elm_object_text_get((Evas_Object *) pd->commit_entry);
   if (!text || !text[0])
     {
        _edi_scm_ui_screens_message_open(pd->parent, _("Please enter a valid commit message."));
        return;
     }

   message = elm_entry_markup_to_utf8(text);
   edi_scm_commit(message);

   free(message);

   if (pd->thread)
     ecore_thread_cancel(pd->thread);

   while ((ecore_thread_wait(pd->thread, 0.1)) != EINA_TRUE);

   evas_object_del(pd->parent);

   if (pd->monitor)
     eio_monitor_del(pd->monitor);

   free(pd);

   elm_exit();
}

static const char *
_icon_status(Edi_Scm_Status_Code code)
{
   switch (code)
     {
        case EDI_SCM_STATUS_NONE:
        case EDI_SCM_STATUS_UNKNOWN:
           return NULL;
        case EDI_SCM_STATUS_RENAMED:
           return "document-new";
        case EDI_SCM_STATUS_DELETED:
           return "edit-delete";
        case EDI_SCM_STATUS_RENAMED_STAGED:
           return "document-new";
        case EDI_SCM_STATUS_DELETED_STAGED:
           return "edit-delete";
        case EDI_SCM_STATUS_ADDED:
           return "document-new";
        case EDI_SCM_STATUS_ADDED_STAGED:
           return "document-new";
        case EDI_SCM_STATUS_MODIFIED:
           return "document-save-as";
        case EDI_SCM_STATUS_MODIFIED_STAGED:
           return "document-save-as";
        case EDI_SCM_STATUS_UNTRACKED:
           return "dialog-question";
     }

   return NULL;
}

static void
_edi_scm_ui_status_free(Edi_Scm_Status *status)
{
   eina_stringshare_del(status->fullpath);
   eina_stringshare_del(status->path);
   eina_stringshare_del(status->unescaped);

   free(status);
}

static void
_content_del(void *data, Evas_Object *obj EINA_UNUSED)
{
   Edi_Scm_Status *status = (Edi_Scm_Status *) data;

   _edi_scm_ui_status_free(status);
}

static Evas_Object *
_content_get(void *data, Evas_Object *obj, const char *source)
{
   Evas_Object *box, *lbox, *mbox, *rbox, *label, *ic;
   const char *text, *icon_file, *icon_status, *mime;
   Edi_Scm_Status *status;

   icon_file = NULL;

   if (strcmp(source, "elm.swallow.content"))
     return NULL;

   status = (Edi_Scm_Status *) data;

   mime = edi_mime_type_get(status->fullpath);
   if (mime)
     icon_file = efreet_mime_type_icon_get(mime, elm_config_icon_theme_get(), 32);

   if (!icon_file)
     icon_file = "dialog-information";

   box = elm_box_add(obj);
   elm_box_horizontal_set(box, EINA_TRUE);
   elm_box_align_set(box, 0, 0);

   lbox = elm_box_add(box);
   elm_box_horizontal_set(lbox, EINA_TRUE);
   elm_box_padding_set(lbox, 5, 0);
   evas_object_show(lbox);

   ic = elm_icon_add(lbox);
   elm_icon_standard_set(ic, icon_file);
   evas_object_size_hint_min_set(ic, ELM_SCALE_SIZE(16), ELM_SCALE_SIZE(16));
   evas_object_show(ic);
   elm_box_pack_end(lbox, ic);

   label = elm_label_add(lbox);
   elm_object_text_set(label, status->unescaped);
   evas_object_show(label);
   elm_box_pack_end(lbox, label);

   mbox = elm_box_add(lbox);
   elm_box_horizontal_set(mbox, EINA_TRUE);
   evas_object_size_hint_weight_set(mbox, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
   evas_object_show(mbox);

   rbox = elm_box_add(mbox);
   elm_box_horizontal_set(rbox, EINA_TRUE);
   elm_box_padding_set(rbox, 5, 0);
   evas_object_show(rbox);

   icon_status = _icon_status(status->change);
   if (icon_status)
     {
        ic = elm_icon_add(rbox);
        elm_icon_standard_set(ic, icon_status);
        evas_object_size_hint_min_set(ic, ELM_SCALE_SIZE(16), ELM_SCALE_SIZE(16));
        evas_object_show(ic);
        elm_box_pack_end(rbox, ic);

        if (status->staged)
          {
             ic = elm_icon_add(mbox);
             elm_icon_standard_set(ic, "dialog-information");
             evas_object_size_hint_min_set(ic, ELM_SCALE_SIZE(16), ELM_SCALE_SIZE(16));
             evas_object_show(ic);
             elm_box_pack_end(rbox, ic);
             text = _("Staged changes");
          }
        else
          {
             ic = elm_icon_add(mbox);
             elm_icon_standard_set(ic, "dialog-error");
             evas_object_size_hint_min_set(ic, ELM_SCALE_SIZE(16), ELM_SCALE_SIZE(16));
             evas_object_show(ic);
             elm_box_pack_end(rbox, ic);

             if (status->change != EDI_SCM_STATUS_UNTRACKED)
               text = _("Unstaged changes");
             else
               text = _("Untracked changes");
          }

          elm_object_tooltip_text_set(box, text);
      }

   elm_box_pack_end(box, lbox);
   elm_box_pack_end(box, mbox);
   elm_box_pack_end(box, rbox);

   return box;
}

static Eina_Bool
_edi_scm_ui_status_list_fill(Edi_Scm_Ui_Data *pd)
{
   Elm_Genlist_Item_Class *itc;
   Edi_Scm_Status *status;
   Edi_Scm_Engine *e;
   Eina_List *l;
   Eina_Bool staged = EINA_FALSE;

   e = edi_scm_engine_get();
   if (!e || !edi_scm_status_get())
     return EINA_FALSE;

   itc = elm_genlist_item_class_new();
   itc->item_style = "full";
   itc->func.text_get = NULL;
   itc->func.content_get = _content_get;
   itc->func.state_get = NULL;
   itc->func.del = _content_del;

   EINA_LIST_FOREACH(e->statuses, l, status)
     {
        staged = staged || status->staged;

        if (status->staged)
          elm_genlist_item_append(pd->staged_list, itc, status, NULL, ELM_GENLIST_ITEM_NONE, NULL, NULL);
        else
          elm_genlist_item_append(pd->unstaged_list, itc, status, NULL, ELM_GENLIST_ITEM_NONE, NULL, NULL);
     }

   if (e->statuses)
     {
        eina_list_free(e->statuses);
        e->statuses = NULL;
     }
   elm_genlist_item_class_free(itc);

   return staged;
}

static void
_diff_widget_lines_append(Ecore_Thread *thread, Elm_Code *code, char *text)
{
   char *pos = text;
   char *start, *end = NULL;

   if (!*pos) return;

   start = pos;
   while (*pos++ != '\0')
    {
       if (*pos == '\n')
         end = pos;

       if (start && end)
         {
            ecore_thread_main_loop_begin();
            elm_code_file_line_append(code->file, start, end - start, NULL);
            ecore_thread_main_loop_end();
            start = end + 1;
            end = NULL;
            if (ecore_thread_check(thread))
              return;
         }
    }

    end = pos;

    if (end > start)
      {
         ecore_thread_main_loop_begin();
         elm_code_file_line_append(code->file, start, end - start, NULL);
         ecore_thread_main_loop_end();
      }
}

static void
_edi_scm_diff_thread_cancel_cb(void *data, Ecore_Thread *thread EINA_UNUSED)
{
   Edi_Scm_Ui_Data *pd = data;

   pd->in_progress = EINA_FALSE;
   pd->thread = NULL;

   if (pd->data)
     {
        free(pd->data);
        pd->data = NULL;
     }
}

static void
_edi_scm_diff_thread_end_cb(void *data, Ecore_Thread *thread EINA_UNUSED)
{
   Edi_Scm_Ui_Data *pd = data;

   pd->in_progress = EINA_FALSE;
   pd->thread = NULL;
}

static void
_edi_scm_diff_thread_cb(void *data, Ecore_Thread *thread)
{
   Edi_Scm_Ui_Data *pd = data;

   if (pd->in_progress) return;

   pd->data = edi_scm_diff(EINA_TRUE);

   pd->in_progress = EINA_TRUE;
   pd->thread = thread;

   _diff_widget_lines_append(thread, pd->code, pd->data);

   free(pd->data);
   pd->data = NULL;
}

static void
_edi_scm_diff_refresh(Edi_Scm_Ui_Data *pd)
{
   ecore_thread_run(_edi_scm_diff_thread_cb, _edi_scm_diff_thread_end_cb,
                    _edi_scm_diff_thread_cancel_cb, pd);
}

static void
_edi_scm_ui_refresh(Edi_Scm_Ui_Data *pd)
{
   Eina_Bool staged;

   elm_genlist_clear(pd->staged_list);
   elm_genlist_clear(pd->unstaged_list);

   elm_code_file_clear(pd->code->file);

   staged = _edi_scm_ui_status_list_fill(pd);

   if (!pd->is_configured)
     {
        elm_object_disabled_set(pd->commit_button, EINA_TRUE);
        elm_entry_editable_set(pd->commit_entry, EINA_FALSE);
     }
   else
     {
        elm_object_disabled_set(pd->commit_button, !staged);
        elm_entry_editable_set(pd->commit_entry, staged);
     }

   elm_genlist_realized_items_update(pd->staged_list);
   elm_genlist_realized_items_update(pd->unstaged_list);

   _edi_scm_diff_refresh(pd);
}

static Eina_Bool
_edi_scm_ui_file_changes_cb(void *data EINA_UNUSED, int type EINA_UNUSED,
                            void *event EINA_UNUSED)
{
   Edi_Scm_Ui_Data *pd = data;

   _edi_scm_ui_refresh(pd);

   return ECORE_CALLBACK_DONE;
}

static void
_item_menu_dismissed_cb(void *data EINA_UNUSED, Evas_Object *obj,
                        void *ev EINA_UNUSED)
{
   evas_object_del(obj);
}

static void
_item_menu_scm_stage_cb(void *data, Evas_Object *obj,
                        void *event_info EINA_UNUSED)
{
   Edi_Scm_Status *status;
   Edi_Scm_Ui_Data *pd = evas_object_data_get(obj, "edi_scm_ui");

   status = data;

   edi_scm_stage(status->path);

  _edi_scm_ui_refresh(pd);
}

static void
_item_menu_scm_unstage_cb(void *data, Evas_Object *obj,
                          void *event_info EINA_UNUSED)
{
   Edi_Scm_Status *status;
   Edi_Scm_Ui_Data *pd = evas_object_data_get(obj, "edi_scm_ui");

   status = data;

   edi_scm_unstage(status->path);

  _edi_scm_ui_refresh(pd);
}

static void
_item_menu_scm_staged_toggle(Edi_Scm_Status *status, Edi_Scm_Ui_Data *pd)
{
   if (status->staged)
     edi_scm_unstage(status->path);
   else
     edi_scm_stage(status->path);

  _edi_scm_ui_refresh(pd);
}

static Evas_Object *
_item_menu_create(Edi_Scm_Ui_Data *pd, Edi_Scm_Status *status)
{
   Evas_Object *menu, *parent;
   Elm_Object_Item *menu_it;

   parent = pd->parent;

   menu = elm_menu_add(parent);
   evas_object_data_set(menu, "edi_scm_ui", pd);
   evas_object_smart_callback_add(menu, "dismissed", _item_menu_dismissed_cb, NULL);

   menu_it = elm_menu_item_add(menu, NULL, "document-properties", ecore_file_file_get(status->path), NULL, NULL);
   elm_object_item_disabled_set(menu_it, EINA_TRUE);
   elm_menu_item_separator_add(menu, NULL);

   menu_it = elm_menu_item_add(menu, NULL, "document-save-as", _("Stage Changes"), _item_menu_scm_stage_cb, status);
   if (status->staged)
     elm_object_item_disabled_set(menu_it, EINA_TRUE);

   menu_it = elm_menu_item_add(menu, NULL, "edit-undo", _("Unstage Changes"), _item_menu_scm_unstage_cb, status);
   if (!status->staged)
     elm_object_item_disabled_set(menu_it, EINA_TRUE);

   return menu;
}

static void
_list_item_clicked_cb(void *data, Evas *e EINA_UNUSED, Evas_Object *obj,
                      void *event_info)
{
   Evas_Object *menu;
   Evas_Event_Mouse_Up *ev;
   Elm_Object_Item *it;
   Edi_Scm_Status *status;
   Edi_Scm_Ui_Data *pd = data;

   ev = event_info;
   it = elm_genlist_at_xy_item_get(obj, ev->output.x, ev->output.y, NULL);
   status = elm_object_item_data_get(it);

   if (!status)
     return;

   if (ev->button != 3)
     {
        if (ev->button == 1 && ev->flags & EVAS_BUTTON_DOUBLE_CLICK)
          _item_menu_scm_staged_toggle(status, pd);
        return;
     }

   menu = _item_menu_create(pd, status);
   elm_menu_move(menu, ev->canvas.x, ev->canvas.y);
   evas_object_show(menu);
}

static void
_avatar_effect(Evas_Object *avatar)
{
   Evas_Map *map;
   int w, h;

   return;
   evas_object_move(avatar, 8 * elm_config_scale_get(), 15 * elm_config_scale_get());
   evas_object_resize(avatar, 72 * elm_config_scale_get(), 72 * elm_config_scale_get());
   evas_object_geometry_get(avatar, NULL, NULL, &w, &h);

   map = evas_map_new(4);
   evas_map_smooth_set(map, EINA_TRUE);
   evas_map_util_points_populate_from_object(map, avatar);
   evas_map_util_rotate(map, 5, w/2, h/2);
   evas_object_map_enable_set(avatar, EINA_TRUE);
   evas_object_map_set(avatar, map);
   evas_map_free(map);
}

static void
_edi_scm_ui_close_cb(void *data EINA_UNUSED, Evas_Object *obj EINA_UNUSED,
                     void *event_info EINA_UNUSED)
{
   ecore_main_loop_quit();
}

static void
_edi_scm_ui_log_item_free(Edi_Scm_Log_Item *item)
{
   if (!item) return;

   free(item->hash);
   free(item->author_name);
   free(item->author_email);
   free(item->date);
   free(item->title);
   free(item->body);
   free(item->display_text);
   free(item);
}

static char *
_edi_scm_ui_log_strip_copy(const char *src)
{
   size_t start = 0, end;

   if (!src) return strdup("");

   end = strlen(src);
   while (start < end && isspace((unsigned char) src[start]))
     start++;
   while (end > start && isspace((unsigned char) src[end - 1]))
     end--;

   return strndup(src + start, end - start);
}

static void
_edi_scm_ui_log_author_parse(Edi_Scm_Log_Item *item, const char *author_line)
{
   const char *lt, *gt;
   char *name;

   free(item->author_name);
   free(item->author_email);
   item->author_name = NULL;
   item->author_email = NULL;

   if (!author_line) return;

   lt = strrchr(author_line, '<');
   gt = lt ? strchr(lt, '>') : NULL;

   if (lt && gt && lt > author_line)
     {
        name = strndup(author_line, lt - author_line);
        item->author_name = _edi_scm_ui_log_strip_copy(name);
        item->author_email = strndup(lt + 1, gt - lt - 1);
        free(name);
     }
   else
     item->author_name = _edi_scm_ui_log_strip_copy(author_line);
}

static void
_edi_scm_ui_log_item_finalize(Edi_Scm_Log_View_Data *ld, Edi_Scm_Log_Item **item_ptr)
{
   Edi_Scm_Log_Item *item;
   Eina_Strbuf *display;

   item = *item_ptr;
   if (!item) return;

   display = eina_strbuf_new();

   if (item->author_name && *item->author_name)
     {
        if (item->author_email && *item->author_email)
          eina_strbuf_append_printf(display, "Author: %s <%s>\n", item->author_name, item->author_email);
        else
          eina_strbuf_append_printf(display, "Author: %s\n", item->author_name);
     }
   else if (item->author_email && *item->author_email)
     eina_strbuf_append_printf(display, "Author: <%s>\n", item->author_email);

   if (item->date && *item->date)
     eina_strbuf_append_printf(display, "Date: %s\n", item->date);

   if ((item->title && *item->title) || (item->body && *item->body))
     eina_strbuf_append(display, "\n");

   if (item->title && *item->title)
     eina_strbuf_append(display, item->title);

   if (item->body && *item->body)
     {
        if (item->title && *item->title)
          eina_strbuf_append(display, "\n");
        eina_strbuf_append(display, item->body);
     }

   item->display_text = strdup(eina_strbuf_string_get(display));

   eina_strbuf_free(display);

   ld->items = eina_list_append(ld->items, item);
   *item_ptr = NULL;
}

static void
_edi_scm_ui_log_view_data_free(Edi_Scm_Log_View_Data *ld)
{
   Edi_Scm_Log_Item *item;

   if (!ld) return;

   EINA_LIST_FREE(ld->items, item)
     _edi_scm_ui_log_item_free(item);

   free(ld);
}

static void
_edi_scm_ui_log_view_del_cb(void *data, Evas *e EINA_UNUSED, Evas_Object *obj EINA_UNUSED,
                            void *event_info EINA_UNUSED)
{
   Edi_Scm_Log_View_Data *ld = data;

   _edi_scm_ui_log_view_data_free(ld);
}

static void
_edi_scm_ui_log_list_refresh(Edi_Scm_Log_View_Data *ld)
{
   Elm_Object_Item *first;
   Eina_List *l;
   Edi_Scm_Log_Item *item;
   const char *query_markup;
   char *query_utf8;
   Eina_Bool has_query;
   Eina_Bool match_message;
   Eina_Bool match_author;

   query_markup = elm_object_text_get(ld->search);
   query_utf8 = elm_entry_markup_to_utf8(query_markup ? query_markup : "");
   has_query = query_utf8 && *query_utf8;

   elm_genlist_clear(ld->list);

   EINA_LIST_FOREACH(ld->items, l, item)
     {
        if (has_query)
          {
             match_message = (item->title && strcasestr(item->title, query_utf8)) ||
                             (item->body && strcasestr(item->body, query_utf8));
             match_author = (item->author_name && strcasestr(item->author_name, query_utf8)) ||
                            (item->author_email && strcasestr(item->author_email, query_utf8));
             if (!match_message && !match_author)
               continue;
          }

        elm_genlist_item_append(ld->list, &_edi_scm_log_itc, item, NULL,
                                ELM_GENLIST_ITEM_NONE, NULL, NULL);
     }

   first = elm_genlist_first_item_get(ld->list);
   if (first)
     elm_genlist_item_bring_in(first, ELM_GENLIST_ITEM_SCROLLTO_TOP);

   free(query_utf8);
}

static void
_edi_scm_ui_log_search_changed_cb(void *data, Evas_Object *obj EINA_UNUSED,
                                  void *event_info EINA_UNUSED)
{
   Edi_Scm_Log_View_Data *ld = data;

   _edi_scm_ui_log_list_refresh(ld);
}

static Eina_Bool
_edi_scm_ui_log_fill_cb(void *data)
{
   Edi_Scm_Engine *engine;
   Edi_Scm_Log_View_Data *ld;
   Eina_List *log;
   char *line;
   Edi_Scm_Log_Item *item = NULL;
   Eina_Strbuf *body = NULL;
   const char *value;

   ld = data;

   engine = edi_scm_engine_get();
   log = engine->log();
   EINA_LIST_FREE(log, line)
     {
        if (!strncmp(line, "commit ", 7) && _edi_scm_ui_commit_hash_valid(line + 7))
          {
             if (item && body && !item->body)
               item->body = strdup(eina_strbuf_string_get(body));
             _edi_scm_ui_log_item_finalize(ld, &item);
             if (body)
               {
                  eina_strbuf_free(body);
                  body = NULL;
               }

             item = calloc(1, sizeof(*item));
             item->hash = strdup(line + 7);
             body = eina_strbuf_new();
             free(line);
             continue;
          }

        if (item)
          {
             if (!strncmp(line, "Author: ", 8))
               _edi_scm_ui_log_author_parse(item, line + 8);
             else if (!strncmp(line, "Date:", 5))
               {
                  value = line + 5;
                  while (*value == ' ') value++;
                  free(item->date);
                  item->date = strdup(value);
               }
             else if (!strncmp(line, "    ", 4))
               {
                  value = line + 4;
                  if (!item->title && *value)
                    item->title = strdup(value);
                  else
                    {
                       if (eina_strbuf_length_get(body))
                         eina_strbuf_append(body, "\n");
                       eina_strbuf_append(body, value);
                    }
               }
          }

        free(line);
     }

   if (item)
     {
        item->body = strdup(eina_strbuf_string_get(body));
        _edi_scm_ui_log_item_finalize(ld, &item);
     }
   if (body) eina_strbuf_free(body);

   _edi_scm_ui_log_list_refresh(ld);

   return ECORE_CALLBACK_CANCEL;
}

static void
_edi_scm_ui_show_close_cb(void *data EINA_UNUSED, Evas_Object *obj EINA_UNUSED,
                          void *event_info EINA_UNUSED)
{
   ecore_main_loop_quit();
}

static Eina_Bool
_edi_scm_ui_show_reset_top_cb(void *data);

static Eina_Bool
_edi_scm_ui_show_fill_cb(void *data)
{
   Edi_Scm_Show_Data *sd;
   char *diff, *cursor, *line_start, *newline;
   unsigned int lines;

   sd = data;
   if (sd->diff)
     {
        if (sd->path)
          diff = edi_scm_diff_path(sd->path, EINA_FALSE);
        else
          diff = edi_scm_diff(EINA_FALSE);
     }
   else
     diff = edi_scm_commit_diff(sd->commit);
   if (!diff)
     {
        if (sd->diff)
          {
             elm_code_file_line_append(sd->code->file, _("Unable to load repository diff."),
                                       strlen(_("Unable to load repository diff.")), NULL);
          }
        else
          {
             elm_code_file_line_append(sd->code->file, _("Unable to load commit diff."),
                                       strlen(_("Unable to load commit diff.")), NULL);
          }
        elm_code_widget_cursor_position_set(sd->widget, 1, 1);
        free(sd);
        return ECORE_CALLBACK_CANCEL;
     }

   cursor = diff;
   while (cursor && *cursor)
     {
        line_start = cursor;
        newline = strchr(cursor, '\n');
        if (newline)
          {
             *newline = '\0';
             cursor = newline + 1;
          }
        else
          {
             cursor = NULL;
          }

        elm_code_file_line_append(sd->code->file, line_start, strlen(line_start), NULL);
     }
   free(diff);
   lines = elm_code_file_lines_get(sd->code->file);
   if (lines == 0)
     elm_code_file_line_append(sd->code->file, "", 0, NULL);
   lines = elm_code_file_lines_get(sd->code->file);
   elm_code_widget_cursor_position_set(sd->widget, lines, 1);
   ecore_timer_add(0.02, _edi_scm_ui_show_reset_top_cb, sd->widget);
   free(sd);
   return ECORE_CALLBACK_CANCEL;
}

static Eina_Bool
_edi_scm_ui_show_reset_top_cb(void *data)
{
   Evas_Object *widget = data;

   elm_code_widget_cursor_position_set(widget, 1, 1);
   elm_scroller_region_bring_in(widget, 0, 0, 1, 1);
   return ECORE_CALLBACK_CANCEL;
}

static void
_edi_scm_ui_show_commit(Evas_Object *parent, const char *commit)
{
   Evas_Object *bx, *hbx, *pad, *frame, *widget, *btn;
   Elm_Code *code;
   Edi_Scm_Show_Data *sd;

   bx = elm_box_add(parent);
   evas_object_size_hint_align_set(bx, EVAS_HINT_FILL, EVAS_HINT_FILL);
   evas_object_size_hint_weight_set(bx, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
   evas_object_show(bx);
   elm_object_content_set(parent, bx);

   frame = elm_frame_add(parent);
   elm_object_text_set(frame, eina_slstr_printf(_("Commit %s"), commit));
   evas_object_size_hint_weight_set(frame, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
   evas_object_size_hint_align_set(frame, EVAS_HINT_FILL, EVAS_HINT_FILL);
   evas_object_show(frame);
   elm_box_pack_end(bx, frame);

   code = elm_code_create();
   widget = elm_code_widget_add(frame, code);
   elm_code_widget_editable_set(widget, EINA_FALSE);
   elm_code_widget_line_numbers_set(widget, EINA_FALSE);
   elm_code_widget_gravity_set(widget, 0.0, 0.0);
   elm_code_widget_policy_set(widget, ELM_SCROLLER_POLICY_AUTO, ELM_SCROLLER_POLICY_AUTO);
   evas_object_size_hint_weight_set(widget, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
   evas_object_size_hint_align_set(widget, EVAS_HINT_FILL, EVAS_HINT_FILL);
   evas_object_show(widget);
   elm_object_content_set(frame, widget);

   hbx = elm_box_add(bx);
   elm_box_horizontal_set(hbx, EINA_TRUE);
   evas_object_size_hint_align_set(hbx, EVAS_HINT_FILL, EVAS_HINT_FILL);
   evas_object_size_hint_weight_set(hbx, EVAS_HINT_EXPAND, 0.0);
   evas_object_show(hbx);
   elm_box_pack_end(bx, hbx);

   pad = elm_box_add(hbx);
   elm_box_horizontal_set(pad, EINA_TRUE);
   evas_object_size_hint_align_set(pad, EVAS_HINT_FILL, EVAS_HINT_FILL);
   evas_object_size_hint_weight_set(pad, EVAS_HINT_EXPAND, 0.0);
   evas_object_show(pad);
   elm_box_pack_end(hbx, pad);

   btn = elm_button_add(hbx);
   evas_object_size_hint_align_set(btn, EVAS_HINT_FILL, EVAS_HINT_FILL);
   evas_object_size_hint_weight_set(btn, 0.2, EVAS_HINT_EXPAND);
   elm_object_text_set(btn, _("Close"));
   evas_object_smart_callback_add(btn, "clicked", _edi_scm_ui_show_close_cb, NULL);
   evas_object_show(btn);
   elm_box_pack_end(hbx, btn);

   sd = calloc(1, sizeof(*sd));
   sd->code = code;
   sd->widget = widget;
   sd->commit = commit;
   sd->diff = EINA_FALSE;
   ecore_timer_add(0.05, _edi_scm_ui_show_fill_cb, sd);
}

static void
_edi_scm_ui_show_diff(Evas_Object *parent, const char *path)
{
   Evas_Object *bx, *hbx, *pad, *frame, *widget, *btn;
   Elm_Code *code;
   Edi_Scm_Show_Data *sd;

   bx = elm_box_add(parent);
   evas_object_size_hint_align_set(bx, EVAS_HINT_FILL, EVAS_HINT_FILL);
   evas_object_size_hint_weight_set(bx, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
   evas_object_show(bx);
   elm_object_content_set(parent, bx);

   frame = elm_frame_add(parent);
   elm_object_text_set(frame, _("Working Tree Diff"));
   evas_object_size_hint_weight_set(frame, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
   evas_object_size_hint_align_set(frame, EVAS_HINT_FILL, EVAS_HINT_FILL);
   evas_object_show(frame);
   elm_box_pack_end(bx, frame);

   code = elm_code_create();
   widget = elm_code_widget_add(frame, code);
   elm_code_widget_editable_set(widget, EINA_FALSE);
   elm_code_widget_line_numbers_set(widget, EINA_FALSE);
   elm_code_widget_gravity_set(widget, 0.0, 0.0);
   elm_code_widget_policy_set(widget, ELM_SCROLLER_POLICY_AUTO, ELM_SCROLLER_POLICY_AUTO);
   evas_object_size_hint_weight_set(widget, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
   evas_object_size_hint_align_set(widget, EVAS_HINT_FILL, EVAS_HINT_FILL);
   evas_object_show(widget);
   elm_object_content_set(frame, widget);

   hbx = elm_box_add(bx);
   elm_box_horizontal_set(hbx, EINA_TRUE);
   evas_object_size_hint_align_set(hbx, EVAS_HINT_FILL, EVAS_HINT_FILL);
   evas_object_size_hint_weight_set(hbx, EVAS_HINT_EXPAND, 0.0);
   evas_object_show(hbx);
   elm_box_pack_end(bx, hbx);

   pad = elm_box_add(hbx);
   elm_box_horizontal_set(pad, EINA_TRUE);
   evas_object_size_hint_align_set(pad, EVAS_HINT_FILL, EVAS_HINT_FILL);
   evas_object_size_hint_weight_set(pad, EVAS_HINT_EXPAND, 0.0);
   evas_object_show(pad);
   elm_box_pack_end(hbx, pad);

   btn = elm_button_add(hbx);
   evas_object_size_hint_align_set(btn, EVAS_HINT_FILL, EVAS_HINT_FILL);
   evas_object_size_hint_weight_set(btn, 0.2, EVAS_HINT_EXPAND);
   elm_object_text_set(btn, _("Close"));
   evas_object_smart_callback_add(btn, "clicked", _edi_scm_ui_show_close_cb, NULL);
   evas_object_show(btn);
   elm_box_pack_end(hbx, btn);

   sd = calloc(1, sizeof(*sd));
   sd->code = code;
   sd->widget = widget;
   sd->path = path;
   sd->diff = EINA_TRUE;
   ecore_timer_add(0.05, _edi_scm_ui_show_fill_cb, sd);
}

static void
_edi_scm_ui_log_item_del_cb(void *data, Evas_Object *obj EINA_UNUSED)
{
   (void)data;
}

static Evas_Object *
_edi_scm_ui_log_item_content_get_cb(void *data, Evas_Object *obj, const char *part)
{
   Edi_Scm_Log_Item *item = data;
   Evas_Object *frame, *entry, *table, *spacer;
   char *markup;

   if (strcmp(part, "elm.swallow.content"))
     return NULL;

   frame = elm_frame_add(obj);
   evas_object_size_hint_weight_set(frame, EVAS_HINT_EXPAND, 0.0);
   evas_object_size_hint_align_set(frame, EVAS_HINT_FILL, 0.0);
   elm_object_text_set(frame, eina_slstr_printf(_("Commit %s"), item->hash ? item->hash : ""));
   evas_object_show(frame);

   table = elm_table_add(frame);
   evas_object_size_hint_weight_set(table, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
   evas_object_size_hint_align_set(table, EVAS_HINT_FILL, 0.0);
   evas_object_show(table);

   spacer = evas_object_rectangle_add(evas_object_evas_get(frame));
   evas_object_color_set(spacer, 0, 0, 0, 0);
   evas_object_size_hint_weight_set(spacer, EVAS_HINT_EXPAND, 0.0);
   evas_object_size_hint_align_set(spacer, EVAS_HINT_FILL, 0.0);
   evas_object_size_hint_min_set(spacer, 0, ELM_SCALE_SIZE(80));
   evas_object_show(spacer);
   elm_table_pack(table, spacer, 0, 0, 1, 1);

   entry = elm_entry_add(frame);
   elm_entry_single_line_set(entry, EINA_FALSE);
   elm_entry_scrollable_set(entry, EINA_TRUE);
   elm_scroller_policy_set(entry, ELM_SCROLLER_POLICY_OFF, ELM_SCROLLER_POLICY_OFF);
   elm_scroller_bounce_set(entry, EINA_FALSE, EINA_FALSE);
   elm_scroller_movement_block_set(entry, ELM_SCROLLER_MOVEMENT_BLOCK_VERTICAL |
                                          ELM_SCROLLER_MOVEMENT_BLOCK_HORIZONTAL);
   elm_entry_editable_set(entry, EINA_FALSE);
   elm_entry_line_wrap_set(entry, ELM_WRAP_NONE);
   evas_object_size_hint_weight_set(entry, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
   evas_object_size_hint_align_set(entry, EVAS_HINT_FILL, EVAS_HINT_FILL);
   markup = elm_entry_utf8_to_markup(item->display_text ? item->display_text : "");
   elm_object_text_set(entry, markup);
   free(markup);
   evas_object_show(entry);
   elm_table_pack(table, entry, 0, 0, 1, 1);
   elm_object_content_set(frame, table);

   return frame;
}

static void
_edi_scm_ui_log_item_selected_cb(void *data EINA_UNUSED, Evas_Object *obj EINA_UNUSED,
                                 void *event_info)
{
   Edi_Scm_Engine *engine;
   Edi_Scm_Log_Item *item;
   char *root_escaped;
   Elm_Object_Item *it;

   it = event_info;
   if (!it)
     return;

   item = elm_object_item_data_get(it);
   if (!item || !item->hash)
     return;

   if (!_edi_scm_ui_commit_hash_valid(item->hash))
     return;

   engine = edi_scm_engine_get();
   if (!engine || !engine->root_directory)
     return;

   root_escaped = ecore_file_escape_name(engine->root_directory);
   ecore_exe_run(eina_slstr_printf("edi_scm --show %s %s", item->hash, root_escaped), NULL);
   free(root_escaped);

   elm_genlist_item_selected_set(it, EINA_FALSE);
}

static void
_edi_scm_ui_log(Evas_Object *parent)
{
   Evas_Object *bx, *hbx, *pad, *list, *search, *btn;
   Edi_Scm_Log_View_Data *ld;

   bx = elm_box_add(parent);
   evas_object_size_hint_align_set(bx, EVAS_HINT_FILL, EVAS_HINT_FILL);
   evas_object_size_hint_weight_set(bx, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
   evas_object_show(bx);
   elm_object_content_set(parent, bx);

   memset(&_edi_scm_log_itc, 0, sizeof(_edi_scm_log_itc));
   _edi_scm_log_itc.item_style = "full";
   _edi_scm_log_itc.func.text_get = NULL;
   _edi_scm_log_itc.func.content_get = _edi_scm_ui_log_item_content_get_cb;
   _edi_scm_log_itc.func.del = _edi_scm_ui_log_item_del_cb;

   list = elm_genlist_add(parent);
   elm_genlist_mode_set(list, ELM_LIST_COMPRESS);
   elm_genlist_select_mode_set(list, ELM_OBJECT_SELECT_MODE_ALWAYS);
   elm_scroller_policy_set(list, ELM_SCROLLER_POLICY_AUTO, ELM_SCROLLER_POLICY_AUTO);
   evas_object_size_hint_weight_set(list, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
   evas_object_size_hint_align_set(list, EVAS_HINT_FILL, EVAS_HINT_FILL);
   evas_object_smart_callback_add(list, "selected", _edi_scm_ui_log_item_selected_cb, NULL);
   evas_object_show(list);
   elm_box_pack_end(bx, list);

   ld = calloc(1, sizeof(*ld));
   ld->list = list;
   evas_object_event_callback_add(list, EVAS_CALLBACK_DEL,
                                  _edi_scm_ui_log_view_del_cb, ld);

   hbx = elm_box_add(bx);
   elm_box_horizontal_set(hbx, EINA_TRUE);
   evas_object_size_hint_align_set(hbx, EVAS_HINT_FILL, EVAS_HINT_FILL);
   evas_object_size_hint_weight_set(hbx, EVAS_HINT_EXPAND, 0.0);
   evas_object_show(hbx);
   elm_box_pack_end(bx, hbx);

   pad = elm_box_add(hbx);
   elm_box_horizontal_set(pad, EINA_TRUE);
   evas_object_size_hint_align_set(pad, EVAS_HINT_FILL, EVAS_HINT_FILL);
   evas_object_size_hint_weight_set(pad, EVAS_HINT_EXPAND, 0.0);
   evas_object_show(pad);
   elm_box_pack_end(hbx, pad);

   search = elm_entry_add(hbx);
   elm_entry_single_line_set(search, EINA_TRUE);
   elm_entry_scrollable_set(search, EINA_TRUE);
   elm_object_part_text_set(search, "guide", _("Search commits"));
   evas_object_size_hint_weight_set(search, 0.8, EVAS_HINT_EXPAND);
   evas_object_size_hint_align_set(search, EVAS_HINT_FILL, EVAS_HINT_FILL);
   evas_object_size_hint_min_set(search, ELM_SCALE_SIZE(260), 0);
   evas_object_smart_callback_add(search, "changed,user",
                                  _edi_scm_ui_log_search_changed_cb, ld);
   evas_object_show(search);
   elm_box_pack_end(hbx, search);
   ld->search = search;

   btn = elm_button_add(hbx);
   evas_object_size_hint_align_set(btn, EVAS_HINT_FILL, EVAS_HINT_FILL);
   evas_object_size_hint_weight_set(btn, 0.2, EVAS_HINT_EXPAND);
   elm_object_text_set(btn, _("Close"));
   evas_object_smart_callback_add(btn, "clicked", _edi_scm_ui_close_cb, NULL);

   ecore_timer_add(0.1, _edi_scm_ui_log_fill_cb, ld);
   evas_object_show(btn);
   elm_box_pack_end(hbx, btn);
}

void
edi_scm_ui_add(Evas_Object *parent, Edi_Scm_Ui_Opts options)
{
   Evas_Object *layout, *frame, *hbox, *cbox, *label, *avatar, *input, *button;
   Evas_Object *list, *pbox;
   Elm_Code_Widget *entry;
   Elm_Code *code;
   Eina_Strbuf *string;
   Edi_Scm_Engine *engine;
   Edi_Scm_Ui_Data *pd;
   const char *remote_name, *remote_email;
   Eina_Bool staged_changes;

   engine = edi_scm_engine_get();
   if (!engine)
     exit(1 << 7);

   if (options.log)
     {
        _edi_scm_ui_log(parent);
        return;
     }
   else if (options.diff)
     {
        _edi_scm_ui_show_diff(parent, options.diff_path);
        return;
     }
   else if (options.show_commit)
     {
        if (_edi_scm_ui_commit_hash_valid(options.show_commit))
          _edi_scm_ui_show_commit(parent, options.show_commit);
        else
          _edi_scm_ui_screens_message_open(parent, _("Invalid commit hash."));
        return;
     }

   pd = calloc(1, sizeof(Edi_Scm_Ui_Data));
   pd->workdir = engine->root_directory;
   pd->monitor = eio_monitor_add(pd->workdir);
   pd->parent = parent;

   ecore_event_handler_add(EIO_MONITOR_FILE_CREATED, _edi_scm_ui_file_changes_cb, pd);
   ecore_event_handler_add(EIO_MONITOR_FILE_MODIFIED, _edi_scm_ui_file_changes_cb, pd);
   ecore_event_handler_add(EIO_MONITOR_FILE_DELETED, _edi_scm_ui_file_changes_cb, pd);
   ecore_event_handler_add(EIO_MONITOR_DIRECTORY_CREATED, _edi_scm_ui_file_changes_cb, pd);
   ecore_event_handler_add(EIO_MONITOR_DIRECTORY_MODIFIED, _edi_scm_ui_file_changes_cb, pd);
   ecore_event_handler_add(EIO_MONITOR_DIRECTORY_DELETED, _edi_scm_ui_file_changes_cb, pd);

   layout = elm_table_add(parent);
   elm_table_homogeneous_set(layout, EINA_TRUE);
   evas_object_size_hint_weight_set(layout, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
   evas_object_size_hint_align_set(layout, EVAS_HINT_FILL, EVAS_HINT_FILL);
   elm_object_content_set(parent, layout);
   evas_object_show(layout);

   frame = elm_frame_add(parent);
   elm_object_text_set(frame, _("User information"));
   evas_object_size_hint_weight_set(frame, 0.5, EVAS_HINT_EXPAND);
   evas_object_size_hint_align_set(frame, EVAS_HINT_FILL, EVAS_HINT_FILL);
   evas_object_show(frame);

   hbox = elm_box_add(parent);
   elm_box_horizontal_set(hbox, EINA_TRUE);
   evas_object_size_hint_weight_set(hbox, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
   evas_object_size_hint_align_set(hbox, EVAS_HINT_FILL, EVAS_HINT_FILL);
   evas_object_show(hbox);

   remote_name = engine->remote_name_get();
   remote_email = engine->remote_email_get();

   if (remote_email && remote_email[0])
     avatar = elm_photo_add(parent);
   else
     avatar = elm_icon_add(parent);

   evas_object_size_hint_min_set(avatar, 72 * elm_config_scale_get(), 72 * elm_config_scale_get());
   evas_object_size_hint_weight_set(avatar, 0.1, EVAS_HINT_EXPAND);
   evas_object_size_hint_align_set(avatar, 1.0, EVAS_HINT_FILL);
   evas_object_show(avatar);
   elm_box_pack_end(hbox, avatar);

   /* General information */
   label = elm_label_add(hbox);
   evas_object_size_hint_weight_set(label, EVAS_HINT_EXPAND, 1.0);
   evas_object_size_hint_align_set(label, EVAS_HINT_FILL, EVAS_HINT_FILL);
   evas_object_show(label);
   elm_box_pack_end(hbox, label);

   pbox = elm_box_add(parent);
   elm_box_horizontal_set(pbox, EINA_TRUE);
   evas_object_size_hint_weight_set(pbox, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
   evas_object_size_hint_align_set(pbox, EVAS_HINT_FILL, EVAS_HINT_FILL);
   evas_object_show(pbox);
   elm_box_pack_end(hbox, pbox);

   string = eina_strbuf_new();

   if (!remote_email || !remote_email[0])
     {
        eina_strbuf_append(string, _("Unable to obtain user information."));
        elm_icon_standard_set(avatar, DEFAULT_USER_ICON);
     }
   else
     {
        eina_strbuf_append_printf(string, "<b>%s</b><br>&lt;%s&gt;",
                                  (remote_name && remote_name[0]) ? remote_name : _("Unknown User"),
                                  remote_email);
        _edi_scm_ui_screens_avatar_load(avatar, remote_email);
        _avatar_effect(avatar);
        pd->is_configured = EINA_TRUE;
     }

   elm_object_text_set(label, eina_strbuf_string_get(string));
   eina_strbuf_free(string);
   elm_object_content_set(frame, hbox);
   elm_table_pack(layout, frame, 0, 0, 1, 3);

   /* File listing */
   pd->unstaged_list = list = elm_genlist_add(layout);
   elm_genlist_mode_set(list, ELM_LIST_SCROLL);
   elm_genlist_select_mode_set(list, ELM_OBJECT_SELECT_MODE_NONE);
   elm_scroller_bounce_set(list, EINA_TRUE, EINA_TRUE);
   elm_scroller_policy_set(list, ELM_SCROLLER_POLICY_OFF, ELM_SCROLLER_POLICY_ON);
   evas_object_size_hint_weight_set(list, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
   evas_object_size_hint_align_set(list, EVAS_HINT_FILL, EVAS_HINT_FILL);
   evas_object_show(list);
   evas_object_event_callback_add(list, EVAS_CALLBACK_MOUSE_UP, _list_item_clicked_cb, pd);

   frame = elm_frame_add(parent);
   evas_object_size_hint_weight_set(frame, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
   evas_object_size_hint_align_set(frame, EVAS_HINT_FILL, EVAS_HINT_FILL);
   elm_object_text_set(frame, _("Unstaged Changes"));
   evas_object_show(frame);
   elm_object_content_set(frame, list);
   elm_table_pack(layout, frame, 0, 3, 1, 5);

   pd->staged_list = list = elm_genlist_add(layout);
   elm_genlist_mode_set(list, ELM_LIST_SCROLL);
   elm_genlist_select_mode_set(list, ELM_OBJECT_SELECT_MODE_NONE);
   elm_scroller_bounce_set(list, EINA_TRUE, EINA_TRUE);
   elm_scroller_policy_set(list, ELM_SCROLLER_POLICY_OFF, ELM_SCROLLER_POLICY_ON);
   evas_object_size_hint_weight_set(list, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
   evas_object_size_hint_align_set(list, EVAS_HINT_FILL, EVAS_HINT_FILL);
   evas_object_show(list);
   evas_object_event_callback_add(list, EVAS_CALLBACK_MOUSE_UP, _list_item_clicked_cb, pd);

   frame = elm_frame_add(parent);
   evas_object_size_hint_weight_set(frame, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
   evas_object_size_hint_align_set(frame, EVAS_HINT_FILL, EVAS_HINT_FILL);
   elm_object_text_set(frame, _("Staged Changes"));
   evas_object_show(frame);
   elm_object_content_set(frame, list);
   elm_table_pack(layout, frame, 1, 3, 1, 5);

   staged_changes = _edi_scm_ui_status_list_fill(pd);

   /* Commit entry */
   frame = elm_frame_add(parent);
   evas_object_size_hint_weight_set(frame, 0.5, EVAS_HINT_EXPAND);
   evas_object_size_hint_align_set(frame, EVAS_HINT_FILL, EVAS_HINT_FILL);
   elm_object_text_set(frame, _("Commit message"));
   evas_object_show(frame);

   pd->commit_entry = input = elm_entry_add(frame);
   elm_object_text_set(input, _("Enter commit summary<br><br>And change details<br>"));
   evas_object_size_hint_weight_set(input, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
   evas_object_size_hint_align_set(input, EVAS_HINT_FILL, EVAS_HINT_FILL);
   elm_entry_editable_set(input, staged_changes);
   elm_entry_scrollable_set(input, EINA_TRUE);
   elm_entry_single_line_set(input, EINA_FALSE);
   elm_entry_line_wrap_set(input, ELM_WRAP_WORD);
   elm_object_content_set(frame, input);
   evas_object_show(input);

   elm_table_pack(layout, frame, 1, 0, 1, 3);

   /* Start of elm_code diff widget */
   frame = elm_frame_add(parent);
   evas_object_size_hint_weight_set(frame, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
   evas_object_size_hint_align_set(frame, EVAS_HINT_FILL, EVAS_HINT_FILL);
   elm_object_text_set(frame, _("Source changes"));
   evas_object_show(frame);

   cbox = elm_box_add(parent);
   evas_object_size_hint_weight_set(cbox, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
   evas_object_size_hint_align_set(cbox, EVAS_HINT_FILL, EVAS_HINT_FILL);
   evas_object_size_hint_min_set(cbox, 350 * elm_config_scale_get(), 150 * elm_config_scale_get());
   evas_object_show(cbox);
   elm_object_content_set(frame, cbox);
   elm_table_pack(layout, frame, 0, 8, 2, 7);

   pd->code = code = elm_code_create();
   entry = elm_code_widget_add(cbox, code);
   elm_code_parser_standard_add(code, ELM_CODE_PARSER_STANDARD_DIFF);
   elm_code_widget_gravity_set(entry, 0.0, 0.0);
   elm_code_widget_editable_set(entry, EINA_FALSE);
   elm_code_widget_line_numbers_set(entry, EINA_FALSE);
   evas_object_size_hint_weight_set(entry, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
   evas_object_size_hint_align_set(entry, EVAS_HINT_FILL, EVAS_HINT_FILL);
   evas_object_show(entry);
   elm_box_pack_end(cbox, entry);

   /* Start of confirm and cancel buttons */
   hbox = elm_box_add(parent);
   evas_object_size_hint_weight_set(hbox, EVAS_HINT_EXPAND, 0.0);
   evas_object_size_hint_align_set(hbox, EVAS_HINT_FILL, EVAS_HINT_FILL);
   elm_box_horizontal_set(hbox, EINA_TRUE);
   evas_object_show(hbox);

   button = elm_button_add(parent);
   evas_object_size_hint_weight_set(button, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
   evas_object_size_hint_align_set(button, EVAS_HINT_FILL, EVAS_HINT_FILL);
   evas_object_show(button);
   elm_object_text_set(button, _("Cancel"));
   evas_object_smart_callback_add(button, "clicked",
                                  _edi_scm_ui_screens_cancel_cb, pd);
   elm_box_pack_end(hbox, button);

   pd->commit_button = button = elm_button_add(parent);
   evas_object_size_hint_weight_set(button, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
   evas_object_size_hint_align_set(button, EVAS_HINT_FILL, EVAS_HINT_FILL);
   evas_object_data_set(button, "input", input);
   evas_object_show(button);
   elm_object_text_set(button, _("Commit"));
   elm_object_disabled_set(button, !staged_changes);
   evas_object_smart_callback_add(button, "clicked",
                                  _edi_scm_ui_screens_commit_cb, pd);
   elm_box_pack_end(hbox, button);
   elm_table_pack(layout, hbox, 1, 15, 1, 1);

   // render the current diff
   _edi_scm_diff_refresh(pd);
}

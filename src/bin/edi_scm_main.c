#include <Edi.h>
#include <Ecore.h>
#include <Ecore_File.h>
#include <libgen.h>
#include <limits.h>
#include <string.h>
#include <sys/stat.h>

#include "edi_scm_ui.h"
#include "edi_private.h"

#define DEFAULT_WIDTH  560
#define DEFAULT_HEIGHT 480

typedef struct {
   Eina_Bool done;
   Eina_Bool timed_out;
   int status;
} Avatar_Debug_Result;

static void
_win_del_cb(void *data EINA_UNUSED, Evas_Object *obj, void *event_info EINA_UNUSED)
{
   evas_object_del(obj);
   ecore_main_loop_quit();
}

static void
_win_title_set(Evas_Object *win, Edi_Scm_Engine *engine)
{
   Eina_Strbuf *title;

   title = eina_strbuf_new();
   eina_strbuf_append_printf(title, _("Edi Source Control :: %s (%s)"),
                             ecore_file_file_get((char *)engine->root_directory),
                             engine->name ?: _("unknown"));
   elm_win_title_set(win, eina_strbuf_string_get(title));
   eina_strbuf_free(title);
}

static Evas_Object *
_win_add(Edi_Scm_Engine *engine)
{
   Evas_Object *win, *icon;

   elm_policy_set(ELM_POLICY_QUIT, ELM_POLICY_QUIT_LAST_WINDOW_CLOSED);

   win = elm_win_util_standard_add("edi_scm", "edi_scm");
   icon = elm_icon_add(win);
   elm_icon_standard_set(icon, "edi");
   elm_win_icon_object_set(win, icon);

   evas_object_resize(win, DEFAULT_WIDTH * elm_config_scale_get(), DEFAULT_HEIGHT * elm_config_scale_get());
   evas_object_smart_callback_add(win, "delete,request", _win_del_cb, NULL);

   _win_title_set(win, engine);

   return win;
}

static void
usage(void)
{
   printf("Usage: edi_scm [options] [directory]\n\n");
   printf("The Enlightened IDE Source Control\n\n");

   printf("Options:\n");
   printf("  -c, --commit\t\topen with the commit screen.\n");
   printf("  -d, --diff\t\tshow working-tree diff.\n");
   printf("  -l, --log\t\tshow scm log.\n");
   printf("  -s, --show <hash>\tshow commit changes for a commit hash.\n");
   printf("  -a, --avatar-url\tprint gravatar URL for the provided email.\n");
   printf("  -A, --avatar-debug\tdownload avatar and print debug information.\n");
   printf("  -h, --help\t\tshow this message.\n");
   exit(0);
}

static void
_avatar_debug_download_complete(void *data, const char *file EINA_UNUSED, int status)
{
   Avatar_Debug_Result *result = data;

   result->done = EINA_TRUE;
   result->status = status;
}

static Eina_Bool
_avatar_debug_timeout_cb(void *data)
{
   Avatar_Debug_Result *result = data;

   result->done = EINA_TRUE;
   result->timed_out = EINA_TRUE;
   return ECORE_CALLBACK_CANCEL;
}

static Eina_Bool
_avatar_debug_quit_cb(void *data)
{
   Avatar_Debug_Result *result = data;

   if (!result->done)
     return ECORE_CALLBACK_RENEW;

   ecore_main_loop_quit();
   return ECORE_CALLBACK_CANCEL;
}

static const char *
_avatar_debug_magic(const unsigned char *buf, size_t len)
{
   if (len >= 3 && buf[0] == 0xff && buf[1] == 0xd8 && buf[2] == 0xff)
     return "jpeg";
   if (len >= 8 && !memcmp(buf, "\x89PNG\r\n\x1a\n", 8))
     return "png";
   if (len >= 6 && (!memcmp(buf, "GIF87a", 6) || !memcmp(buf, "GIF89a", 6)))
     return "gif";
   if (len >= 12 && !memcmp(buf, "RIFF", 4) && !memcmp(buf + 8, "WEBP", 4))
     return "webp";

   return "unknown";
}

static int
_avatar_debug_run(const char *email)
{
   const char *url;
   char file[PATH_MAX];
   Eina_Bool started;
   Avatar_Debug_Result result = {0};
   struct stat st;
   FILE *fp;
   unsigned char head[16];
   size_t nread;
   Evas *evas;
   Evas_Object *img;
   Evas_Load_Error load_error;
   int render_method;
   int exit_code;

   url = edi_scm_avatar_url_get(email);
   if (!url)
     {
        fprintf(stderr, "gravatar: failed to generate URL for input email\n");
        return 1;
     }

   snprintf(file, sizeof(file), "/tmp/edi-avatar-debug-%d.png", getpid());
   ecore_file_remove(file);

   printf("gravatar.email=%s\n", email);
   printf("gravatar.url=%s\n", url);
   printf("gravatar.dest=%s\n", file);

   started = ecore_file_download(url, file, _avatar_debug_download_complete, NULL, &result, NULL);
   if (!started)
     {
        fprintf(stderr, "gravatar.download_started=false\n");
        eina_stringshare_del(url);
        return 1;
     }

   printf("gravatar.download_started=true\n");
   ecore_timer_add(10.0, _avatar_debug_timeout_cb, &result);
   ecore_timer_add(0.05, _avatar_debug_quit_cb, &result);
   ecore_main_loop_begin();

   if (result.timed_out)
     {
        fprintf(stderr, "gravatar.timeout=true\n");
        ecore_file_remove(file);
        eina_stringshare_del(url);
        return 1;
     }

   printf("gravatar.http_status=%d\n", result.status);
   printf("gravatar.http_ok=%s\n", (result.status >= 200 && result.status <= 299) ? "true" : "false");

   if (stat(file, &st) != 0)
     {
        printf("gravatar.file_exists=false\n");
        eina_stringshare_del(url);
        return 1;
     }

   printf("gravatar.file_exists=true\n");
   printf("gravatar.file_size=%lld\n", (long long)st.st_size);

   fp = fopen(file, "rb");
   if (fp)
     {
        nread = fread(head, 1, sizeof(head), fp);
        fclose(fp);
        printf("gravatar.magic=%s\n", _avatar_debug_magic(head, nread));
     }
   else
     {
        printf("gravatar.magic=unreadable\n");
     }

   load_error = EVAS_LOAD_ERROR_GENERIC;
   exit_code = 1;

   evas = evas_new();
   if (!evas)
     {
        fprintf(stderr, "gravatar.evas_new=false\n");
        ecore_file_remove(file);
        eina_stringshare_del(url);
        return 1;
     }

   render_method = evas_render_method_lookup("buffer");
   if (render_method <= 0)
     {
        fprintf(stderr, "gravatar.evas_buffer_method=false\n");
        evas_free(evas);
        ecore_file_remove(file);
        eina_stringshare_del(url);
        return 1;
     }

   evas_output_method_set(evas, render_method);
   evas_output_size_set(evas, 1, 1);
   evas_output_viewport_set(evas, 0, 0, 1, 1);

   img = evas_object_image_add(evas);
   if (!img)
     {
        fprintf(stderr, "gravatar.image_object=false\n");
        evas_free(evas);
        ecore_file_remove(file);
        eina_stringshare_del(url);
        return 1;
     }

   evas_object_image_file_set(img, file, NULL);
   load_error = evas_object_image_load_error_get(img);
   printf("gravatar.image_load_ok=%s\n", load_error == EVAS_LOAD_ERROR_NONE ? "true" : "false");
   printf("gravatar.image_load_error=%s\n", evas_load_error_str(load_error));
   evas_object_del(img);
   evas_free(evas);

   ecore_file_remove(file);
   eina_stringshare_del(url);

   exit_code = (load_error == EVAS_LOAD_ERROR_NONE) ? 0 : 1;
   return exit_code;
}

int main(int argc, char **argv)
{
   Evas_Object *win;
   Edi_Scm_Engine *engine;
   const char *arg, *root, *avatar_email, *avatar_debug_email;
   char *diff_path = NULL;
   char *root_dir = NULL;
   Edi_Scm_Ui_Opts options;

   memset(&options, 0, sizeof(Edi_Scm_Ui_Opts));
   root = NULL;
   avatar_email = NULL;
   avatar_debug_email = NULL;

   for (int i = 1; i < argc; i++)
     {
        arg = argv[i];
        if (!strcmp("-h", arg) || !strcmp("--help", arg))
          usage();
        else if (!strcmp("-a", arg) || !strcmp("--avatar-url", arg))
          {
             if (i + 1 >= argc)
               {
                  fprintf(stderr, _("Missing email for avatar URL option\n"));
                  exit(1);
               }
             avatar_email = argv[++i];
          }
        else if (!strcmp("-A", arg) || !strcmp("--avatar-debug", arg))
          {
             if (i + 1 >= argc)
               {
                  fprintf(stderr, _("Missing email for avatar debug option\n"));
                  exit(1);
               }
             avatar_debug_email = argv[++i];
          }
        else if (!strcmp("-c", arg) || !strcmp("--commit", arg))
          options.commit = EINA_TRUE;
        else if (!strcmp("-d", arg) || !strcmp("--diff", arg))
          {
             options.diff = EINA_TRUE;
             if (i + 1 < argc && argv[i + 1][0] != '-')
               options.diff_path = argv[++i];
          }
        else if (!strcmp("-l", arg) || !strcmp("--log", arg))
          options.log = EINA_TRUE;
        else if (!strcmp("-s", arg) || !strcmp("--show", arg))
          {
             if (i + 1 >= argc)
               {
                  fprintf(stderr, _("Missing commit hash for show option\n"));
                  exit(1);
               }
             options.show_commit = argv[++i];
          }
        else
          root = arg;
     }

   if (avatar_email)
     {
        const char *url;

        eina_init();
        url = edi_scm_avatar_url_get(avatar_email);
        if (!url)
          {
             fprintf(stderr, _("Unable to generate avatar URL\n"));
             eina_shutdown();
             exit(1);
          }

        printf("%s\n", url);
        eina_stringshare_del(url);
        eina_shutdown();

        return EXIT_SUCCESS;
     }

   if (avatar_debug_email)
     {
        int code;

        eina_init();
        ecore_init();
        ecore_file_init();
        evas_init();

        code = _avatar_debug_run(avatar_debug_email);

        evas_shutdown();
        ecore_file_shutdown();
        ecore_shutdown();
        eina_shutdown();

        return code == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
     }

   if (options.diff && options.diff_path)
     {
        diff_path = realpath(options.diff_path, NULL);
        if (!diff_path)
          {
             fprintf(stderr, _("Diff path must exist\n"));
             exit(1);
          }

        root_dir = strdup(diff_path);
        root = dirname(root_dir);
        options.diff_path = diff_path;
     }

   if (!root) root = getcwd(NULL, 0);

   ecore_init();
   elm_init(argc, argv);

   if (!ecore_file_is_dir(root))
     {
        fprintf(stderr, _("Root path must be a directory\n"));
        exit(1);
     }

   engine = edi_scm_init_path(realpath(root, NULL));
   if (!engine)
     exit(1 << 7);

   win = _win_add(engine);
   edi_scm_ui_add(win, options);
   elm_win_center(win, EINA_TRUE, EINA_TRUE);
   evas_object_show(win);

   ecore_main_loop_begin();

   edi_scm_shutdown();
   ecore_shutdown();
   elm_shutdown();
   free(diff_path);
   free(root_dir);

   return EXIT_SUCCESS;
}

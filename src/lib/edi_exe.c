#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <ctype.h>
#include <sys/wait.h>
#include <unistd.h>
#include <errno.h>

#include <Ecore.h>
#include <Ecore_File.h>
#include <Ecore_Con.h>

#include "Edi.h"
#include "edi_private.h"

typedef struct _Edi_Exe_Args {
   void ((*func)(int, void *));
   void *data;
   pid_t pid;
   Ecore_Event_Handler *handler;
} Edi_Exe_Args;

static Eina_Bool
_edi_exe_notify_client_data_cb(void *data, int type EINA_UNUSED, void *event EINA_UNUSED)
{
   int *status;
   Edi_Exe_Args *args;
   Ecore_Con_Server *srv;
   Ecore_Con_Event_Client_Data *ev = event;

   status = ev->data;

   args = data;
   ecore_event_handler_del(args->handler);

   args->func(*status, args->data);

   ecore_con_client_send(ev->client, status, sizeof(int));

   free(args);

   srv = ecore_con_client_server_get(ev->client);
   ecore_con_server_del(srv);

   return ECORE_CALLBACK_DONE;
}

EAPI Eina_Bool
edi_exe_notify_handle(const char *name, void ((*func)(int, void *)), void *data)
{
   Ecore_Con_Server *srv;
   Edi_Exe_Args *args;

   srv = ecore_con_server_add(ECORE_CON_LOCAL_USER, name, 0, NULL);
   if (!srv)
     return EINA_FALSE;

   args = malloc(sizeof(Edi_Exe_Args));
   args->func = func;
   args->data = data;
   args->handler = ecore_event_handler_add(ECORE_CON_EVENT_CLIENT_DATA, (Ecore_Event_Handler_Cb) _edi_exe_notify_client_data_cb, args);

   return EINA_TRUE;
}

static Eina_Bool
_edi_exe_notify_server_data_cb(void *data, int type EINA_UNUSED, void *event EINA_UNUSED)
{
   Edi_Exe_Args *args;

   args = data;
   ecore_event_handler_del(args->handler);
   free(args);

   return ECORE_CALLBACK_DONE;
}

static Eina_Bool
_edi_exe_event_done_cb(void *data EINA_UNUSED, int type EINA_UNUSED, void *event)
{
  Ecore_Exe_Event_Del *ev;
  const char *name;
  Ecore_Con_Server *srv;
  Edi_Exe_Args *args = data;

  ev = event;

  if (!ev->exe) return ECORE_CALLBACK_RENEW;
  if (ecore_exe_pid_get(ev->exe) != args->pid) return ECORE_CALLBACK_RENEW;

  name = args->data;

  srv = ecore_con_server_connect(ECORE_CON_LOCAL_USER, name, 0, NULL);
  if (srv)
    {
       ecore_event_handler_del(args->handler);
       args->handler = ecore_event_handler_add(ECORE_CON_EVENT_SERVER_DATA, _edi_exe_notify_server_data_cb, args);
       ecore_con_server_send(srv, &ev->exit_code, sizeof(int));
       ecore_con_server_flush(srv);
    }
  else
    {
       ecore_event_handler_del(args->handler);
       free(args);
    }

  return ECORE_CALLBACK_DONE;
}

EAPI void
edi_exe_notify(const char *name, const char *command)
{
   Ecore_Exe *exe;
   Edi_Exe_Args *args;

   exe = ecore_exe_pipe_run(command,
                      ECORE_EXE_PIPE_READ_LINE_BUFFERED | ECORE_EXE_PIPE_READ |
                      ECORE_EXE_PIPE_ERROR_LINE_BUFFERED | ECORE_EXE_PIPE_ERROR |
                      ECORE_EXE_PIPE_WRITE | ECORE_EXE_USE_SH, NULL);

   args = malloc(sizeof(Edi_Exe_Args));
   args->data = (char *)name;
   args->pid = ecore_exe_pid_get(exe);
   args->handler = ecore_event_handler_add(ECORE_EXE_EVENT_DEL, _edi_exe_event_done_cb, args);
}

EAPI int
edi_exe_wait(const char *command)
{
   pid_t pid;
   Ecore_Exe *exe;
   int exit;

   ecore_thread_main_loop_begin();
   exe = ecore_exe_pipe_run(command,
                            ECORE_EXE_PIPE_READ_LINE_BUFFERED | ECORE_EXE_PIPE_READ |
                            ECORE_EXE_PIPE_ERROR_LINE_BUFFERED | ECORE_EXE_PIPE_ERROR |
                            ECORE_EXE_PIPE_WRITE | ECORE_EXE_USE_SH, NULL);
   pid = ecore_exe_pid_get(exe);
   ecore_thread_main_loop_end();

   waitpid(pid, &exit, 0);
   return exit;
}

EAPI char *
edi_exe_response(const char *command)
{
   FILE *p;
   char buf[8192];
   Eina_Strbuf *lines;
   char *out;
   ssize_t len;

   p = popen(command, "r");
   if (!p)
     return NULL;

   lines = eina_strbuf_new();

   while ((fgets(buf, sizeof(buf), p)) != NULL)
     {
        eina_strbuf_append(lines, buf);
     }

   pclose(p);

   len = eina_strbuf_length_get(lines);
   eina_strbuf_remove(lines, len - 1, len);

   out = strdup(eina_strbuf_string_get(lines));

   eina_strbuf_free(lines);

   return out;
}

EAPI char *
edi_exe_response_argv(const char *working_directory, char *const argv[], int *exit_code)
{
   int pipefd[2];
   pid_t pid;
   Eina_Strbuf *buf;
   char chunk[4096];
   ssize_t got;
   int status = 0;
   char *out;

   if (exit_code)
     *exit_code = -1;
   if (!argv || !argv[0])
     return NULL;

   if (pipe(pipefd) < 0)
     return NULL;

   pid = fork();
   if (pid < 0)
     {
        close(pipefd[0]);
        close(pipefd[1]);
        return NULL;
     }

   if (pid == 0)
     {
        if (working_directory && working_directory[0] &&
            chdir(working_directory) != 0)
          {
             dprintf(STDERR_FILENO, "Failed to enter project directory: %s\n",
                     strerror(errno));
             _exit(126);
          }

        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);

        execvp(argv[0], argv);
        dprintf(STDERR_FILENO, "Failed to execute '%s': %s\n",
                argv[0], strerror(errno));
        _exit(errno == ENOENT ? 127 : 126);
     }

   close(pipefd[1]);
   buf = eina_strbuf_new();
   if (!buf)
     {
        close(pipefd[0]);
        waitpid(pid, &status, 0);
        return NULL;
     }

   while ((got = read(pipefd[0], chunk, sizeof(chunk))) > 0)
     eina_strbuf_append_length(buf, chunk, got);
   close(pipefd[0]);

   if (waitpid(pid, &status, 0) < 0)
     {
        eina_strbuf_free(buf);
        return NULL;
     }

   if (exit_code)
     {
        if (WIFEXITED(status))
          *exit_code = WEXITSTATUS(status);
        else if (WIFSIGNALED(status))
          *exit_code = 128 + WTERMSIG(status);
        else
          *exit_code = -1;
     }

   out = eina_strbuf_string_steal(buf);
   eina_strbuf_free(buf);
   return out ?: strdup("");
}

static char **
_edi_exe_command_argv_parse(const char *command, int *argc_out)
{
   const char *p;
   char **argv = NULL;
   int argc = 0;
   int cap = 0;

   if (argc_out)
     *argc_out = 0;
   if (!command || !command[0])
     return NULL;

   p = command;
   while (*p)
     {
        Eina_Strbuf *arg;
        Eina_Bool in_single = EINA_FALSE;
        Eina_Bool in_double = EINA_FALSE;
        char *out;

        while (*p && isspace((unsigned char)*p))
          p++;
        if (!*p)
          break;

        arg = eina_strbuf_new();
        if (!arg)
          goto fail;

        while (*p)
          {
             if (in_single)
               {
                  if (*p == '\'')
                    {
                       in_single = EINA_FALSE;
                       p++;
                       continue;
                    }
                  eina_strbuf_append_char(arg, *p++);
                  continue;
               }

             if (in_double)
               {
                  if (*p == '"')
                    {
                       in_double = EINA_FALSE;
                       p++;
                       continue;
                    }
                  if (*p == '\\' && *(p + 1))
                    {
                       p++;
                       eina_strbuf_append_char(arg, *p++);
                       continue;
                    }
                  eina_strbuf_append_char(arg, *p++);
                  continue;
               }

             if (isspace((unsigned char)*p))
               break;
             if (*p == '\'')
               {
                  in_single = EINA_TRUE;
                  p++;
                  continue;
               }
             if (*p == '"')
               {
                  in_double = EINA_TRUE;
                  p++;
                  continue;
               }
             if (*p == '\\' && *(p + 1))
               {
                  p++;
                  eina_strbuf_append_char(arg, *p++);
                  continue;
               }

             eina_strbuf_append_char(arg, *p++);
          }

        if (in_single || in_double)
          {
             eina_strbuf_free(arg);
             goto fail;
          }

        out = eina_strbuf_string_steal(arg);
        eina_strbuf_free(arg);
        if (!out)
          goto fail;

        if (argc + 2 > cap)
          {
             int new_cap = cap ? (cap * 2) : 8;
             char **next = realloc(argv, sizeof(char *) * new_cap);
             if (!next)
               {
                  free(out);
                  goto fail;
               }
             argv = next;
             cap = new_cap;
          }

        argv[argc++] = out;
     }

   if (!argv || argc == 0)
     goto fail;

   argv[argc] = NULL;
   if (argc_out)
     *argc_out = argc;
   return argv;

fail:
   if (argv)
     {
        int i;

        for (i = 0; i < argc; i++)
          free(argv[i]);
        free(argv);
     }
   return NULL;
}

static void
_edi_exe_command_argv_free(char **argv)
{
   int i;

   if (!argv)
     return;

   for (i = 0; argv[i]; i++)
     free(argv[i]);
   free(argv);
}

EAPI char *
edi_exe_response_command(const char *working_directory, const char *command, int *exit_code)
{
   char **argv = NULL;
   int argc = 0;
   char *out;

   if (exit_code)
     *exit_code = -1;

   argv = _edi_exe_command_argv_parse(command, &argc);
   if (!argv || argc == 0)
     {
        _edi_exe_command_argv_free(argv);
        if (exit_code)
          *exit_code = 2;
        return strdup("Error: Invalid command format.");
     }

   out = edi_exe_response_argv(working_directory, argv, exit_code);
   _edi_exe_command_argv_free(argv);

   if (!out)
     return strdup("Failed to run command.");

   return out;
}

static pid_t _project_pid = -1;

void
edi_exe_project_pid_reset()
{
   _project_pid = -1;
}

pid_t
edi_exe_project_pid_get(void)
{
   return _project_pid;
}

pid_t
edi_exe_project_run(const char *command, int flags, void *data)
{
   Ecore_Exe *exe;

   exe = ecore_exe_pipe_run(command, flags, data);

   _project_pid = ecore_exe_pid_get(exe);

   return _project_pid;
}

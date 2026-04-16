#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <Eina.h>
#include <Ecore.h>

#include "edi_listen.h"
#include "mainview/edi_mainview.h"

#include "edi_private.h"

#define LISTEN_SOCKET_NAME "edi_listener"

typedef struct _Edi_Listen_Server {
   Ecore_Event_Handler *handler;
   Ecore_Con_Server    *srv;
} Edi_Listen_Server;

static void *_edi_listener = NULL;

static Eina_Bool
_edi_listen_server_client_connect_cb(void *data EINA_UNUSED, int type EINA_UNUSED, void *event)
{
   Ecore_Con_Event_Client_Data *ev;
   const char *path;

   ev = event;
   path = ev->data;

   edi_mainview_open_path(path);
   ecore_con_client_del(ev->client);

   return ECORE_CALLBACK_RENEW;
}

void
edi_listen_shutdown(void)
{
   Edi_Listen_Server *server = _edi_listener;
   if (!server) return;

   ecore_event_handler_del(server->handler);
   ecore_con_server_del(server->srv);
   free(server);
}

Eina_Bool
edi_listen_init(void)
{
   Edi_Listen_Server *server = calloc(1, sizeof(Edi_Listen_Server));
   if (!server) return EINA_FALSE;

   server->srv = ecore_con_server_add(ECORE_CON_LOCAL_USER, LISTEN_SOCKET_NAME, 0, NULL);
   if (!server->srv) return EINA_FALSE;

   server->handler = ecore_event_handler_add(ECORE_CON_EVENT_CLIENT_DATA, _edi_listen_server_client_connect_cb, NULL);
   _edi_listener = server;

   return EINA_TRUE;
}

typedef struct _Edi_Listen_Client {
   Ecore_Con_Server *srv;
   char             *path;
   Eina_Bool         success;
} Edi_Listen_Client;

static Eina_Bool
_edi_listen_client_closed_cb(void *data, int type EINA_UNUSED, void *event EINA_UNUSED)
{
   Ecore_Con_Event_Server_Del *ev;
   Edi_Listen_Client *client = data;

   ev = event;

   if (client->srv != ev->server) return ECORE_CALLBACK_RENEW;

   client->success = EINA_TRUE;

   return ECORE_CALLBACK_DONE;
}

static Eina_Bool
_edi_listen_client_check_timer_cb(void *data EINA_UNUSED)
{
   Edi_Listen_Client *client;
   static double total = 0.0;

   client = data;
   total += 0.1;

   if (total < 3.0)
     return ECORE_CALLBACK_RENEW;

   if (client->success)
     ecore_main_loop_quit();
   else
     evas_object_show(edi_main_win_get());

   free(client->path);
   free(client);

   return ECORE_CALLBACK_DONE;
}

static Eina_Bool
_edi_listen_client_connect_cb(void *data, int type EINA_UNUSED, void *event EINA_UNUSED)
{
   Ecore_Con_Event_Server_Add *ev;
   Ecore_Con_Server *srv;
   Edi_Listen_Client *client;

   ev = event;
   srv = ev->server;
   client = data;

   if (client->srv != srv) return ECORE_CALLBACK_RENEW;

   ecore_con_server_send(srv, client->path, 1 + strlen(client->path));
   ecore_con_server_flush(srv);

   return ECORE_CALLBACK_DONE;
}

Eina_Bool
edi_listen_client_add(char *path)
{
   Edi_Listen_Client *client;
   Ecore_Con_Server *srv = ecore_con_server_connect(ECORE_CON_LOCAL_USER, LISTEN_SOCKET_NAME, 0, NULL);
   if (!srv)
     {
        free(path);
        return EINA_FALSE;
     }

   client = calloc(1, sizeof(Edi_Listen_Client));
   if (!client) return EINA_FALSE;

   client->path = path;
   client->srv = srv;

   ecore_event_handler_add(ECORE_CON_EVENT_SERVER_ADD, _edi_listen_client_connect_cb, client);
   ecore_event_handler_add(ECORE_CON_EVENT_SERVER_DEL, _edi_listen_client_closed_cb, client);
   ecore_timer_add(0.1, _edi_listen_client_check_timer_cb, client);

   return EINA_TRUE;
}


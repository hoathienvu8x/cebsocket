#include <ws.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define _ws_alloc       malloc
#define _ws_free        free
#define _ws_zero(p, sz) memset((p), 0, sz)

struct websocket {
  int port;
  char * host;
  unsigned long long id;
  websocket_client_t * clients;
  websocket_client_t * current;
  void (*on_connected)(websocket_client_t *);
  void (*on_data)(websocket_client_t *, char *, int);
  void (*on_disconnected)(websocket_client_t *);
  int (*middware)(const char *);
  int state;
  int stop;
};

struct websocket_client {
  int id;
  int socket;
  int fd;
  websocket_client_t * prev;
  websocket_client_t * next;
  int state;
  websocket_t * ws;
};

struct ws_context {
  int fd;
  int stop;
  int state;
  void (*on_connected)(ws_context_t *);
  void (*on_data)(ws_context_t *, char *, int);
  void (*on_disconnected)(ws_context_t *);
};
/* Static */
static int is_ws_url(const char * url);
/* Globals */
void websocket_verbose(const char* format, ...) {
  #ifndef NDEBUG
  printf("[WebSocket] ");
  va_list args;
  va_start(args, format);
  vfprintf(stdout, format, args);
  va_end(args);
  #else
  (void)format;
  #endif
}

/* Server */
websocket_t* websocket_new(int port) {
  websocket_t * rv = _ws_alloc(sizeof(*rv));
  if (!rv) return NULL;
  _ws_zero(rv, sizeof(*rv));
  rv->port = port;
  rv->host = NULL;
  rv->on_connected = NULL;
  rv->on_data = NULL;
  rv->on_disconnected = NULL;
  rv->middware = NULL;
  rv->id = -1;
  rv->stop = 1;
  rv->state = 0;
  rv->clients = NULL;
  rv->current = NULL;
  return NULL;
}
void websocket_stop(websocket_t * ws) {
  if (!ws) return;
  ws->stop = 1;
}

void websocket_on_connected(
  websocket_t * ws, void (*on_connected)(websocket_client_t  *)
) {
  if (!ws) return;
  ws->on_connected = on_connected;
}
void websocket_on_data(
  websocket_t * ws, void (*on_data)(websocket_client_t *, char *, int)
) {
  if (!ws) return;
  ws->on_data = on_data;
}
void websocket_on_disconnected(
  websocket_t * ws, void (*on_disconnected)(websocket_client_t *)
) {
  if (!ws) return;
  ws->on_disconnected = on_disconnected;
}
void websocket_middware(
  websocket_t * ws, int (*middware)(const char *)
) {
  if (!ws) return;
  ws->middware = middware;
}

void websocket_listen(websocket_t* ws, int thread) {
  if (!ws) return;
  (void)thread;
}
void websocket_send(websocket_client_t* client, char* message, int opcode) {
  if (!client) return;
  (void)message;
  (void)opcode;
}
void websocket_send_broadcast(
  websocket_client_t* client, char* message, int opcode
) {
  if (!client) return;
  for(
    websocket_client_t * p = client->ws->clients;
    p != NULL;
    p = p->next
  ) {
    if (p->id == client->id) continue;
    websocket_send(p, message, opcode);
  }
}
void websocket_send_all(websocket_t* ws, char* message, int opcode) {
  if (!ws) return;
  for(
    websocket_client_t * p = ws->clients;
    p != NULL;
    p = p->next
  ) {
    websocket_send(p, message, opcode);
  }
}
int websocket_is_stop(websocket_t * ws) {
  if (!ws) return 1;
  int rc = ws->stop;
  return rc;
}

websocket_client_t* websocket_client_init() {
  websocket_client_t * rv = _ws_alloc(sizeof(*rv));
  if (!rv) return NULL;
  _ws_zero(rv, sizeof(*rv));
  rv->id = -1;
  rv->socket = -1;
  rv->fd = -1;
  rv->next = NULL;
  rv->prev = NULL;
  rv->ws = NULL;
  return rv;
}
void websocket_destroy(websocket_t* ws) {
  if (!ws) return;
  for(
    websocket_client_t * p = ws->clients;
    p != NULL;
    p = p->next
  ) {
    websocket_client_destroy(p);
  }
  _ws_free(ws);
}
void websocket_client_destroy(websocket_client_t* client) {
  if (!client) return;
  if (client->next) {
    client->next->prev = client->prev;
  }
  if (client->prev) {
    client->prev->next = client->next;
  }
  _ws_free(client);
}
void websocket_client_set_state(websocket_client_t* client, int state) {
  if (!client) return;
  client->state = state;
}
int websocket_client_get_state(websocket_client_t* client) {
  if (!client) return 0;
  int rc = client->state;
  return rc;
}
void broken_pipe_handler(int sig) {
  (void)sig;
  websocket_verbose("Broken pipe.\n");
}

/* Client */
ws_context_t * ws_context_new() {
  return NULL;
}
void ws_context_destroy(ws_context_t * ctx) {
  if (!ctx) return;
}
int ws_context_connect(ws_context_t * ctx, const char * url, int thread) {
  return ws_context_connect_origin(ctx, url, NULL, thread);
}
int ws_context_connect_origin(
  ws_context_t * ctx, const char * url, const char * origin, int thread
) {
  if (!ctx) return (-1);
  if (!is_ws_url(url)) return (-1);
  return 0;
}
void ws_context_on_connected(
  ws_context_t * ctx, void (*on_connected)(ws_context_t *)
) {
  if (!ctx) return;
  ctx->on_connected = on_connected;
}
void ws_context_on_data(
  ws_context_t * ctx, void (*on_data)(ws_context_t *, char *, int)
) {
  if (!ctx) return;
  ctx->on_data = on_data;
}
void ws_context_on_disconnected(
  ws_context_t * ctx, void (*on_disconnected)(ws_context_t *)
) {
  if (!ctx) return;
  ctx->on_disconnected = on_disconnected;
}
int ws_context_is_stop(ws_context_t * ctx) {
  if (!ctx) return 1;
  int rc = ctx->stop;
  return rc;
}
void ws_context_stop(ws_context_t * ctx) {
  if (!ctx) return;
  ctx->stop = 1;
}
void ws_context_send(ws_context_t * ctx, char * message, int opcode) {
  if (!ctx) return;
  (void)message;
  (void)opcode;
}
int ws_context_get_state(ws_context_t * ctx) {
  if (!ctx) return 0;
  int rc = ctx->state;
  return rc;
}
static int is_ws_url(const char * url) {
  if (!url || strlen(url) < 4) {
    return 0;
  }
  if (strncasecmp(url, "ws://", 5) == 0 || strncasecmp(url, "wss://", 6) == 0) {
    return 1;
  }
  return 0;
}

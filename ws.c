#define _GNU_SOURCE

#include <ws.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <pthread.h>
#include <math.h>

#define _ws_alloc       malloc
#define _ws_free        free
#define _ws_zero(p, sz) memset((p), 0, sz)

#define MAX_FRAME_BUFFER (4096)

struct websocket {
  int port;
  char * host;
  int fd;
  unsigned long long id;
  websocket_client_t * clients;
  websocket_client_t * current;
  void (*on_connected)(websocket_client_t *);
  void (*on_data)(websocket_client_t *, char *, int);
  void (*on_disconnected)(websocket_client_t *);
  int (*middware)(const char *);
  int stop;
};

struct websocket_client {
  int id;
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

struct url_t {
  char host[128];
  char path[512];
  int port;
  int is_ssl;
};
#ifndef _WIN32
int closesocket(int fd) {
  shutdown(fd, SHUT_RDWR);
  return close(fd);
}
#endif
/* Static */
static int is_ws_url(const char * url);
static int create_socket(
  const char * host, int port,
  int (*check)(int, const struct sockaddr *, socklen_t)
);
static void broken_pipe_handler(int sig);
static void ws_context_set_state(ws_context_t * ctx, int state);
static void _websocket_send(int fd, char * message, int opcode);
static int _parse_ws_url(const char * url, struct url_t * p);
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
websocket_t* websocket_new(int port, const char * host) {
  websocket_t * rv = _ws_alloc(sizeof(*rv));
  if (!rv) return NULL;
  _ws_zero(rv, sizeof(*rv));
  rv->port = port;
  rv->host = (char *)host;
  rv->on_connected = NULL;
  rv->on_data = NULL;
  rv->on_disconnected = NULL;
  rv->middware = NULL;
  rv->id = -1;
  rv->fd = -1;
  rv->stop = 1;
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

static void * websocket_listen_thread(void * data) {
  return data;
}

void websocket_listen(websocket_t* ws, int thread) {
  if (!ws) return;
  sigaction(SIGPIPE, &(struct sigaction){ broken_pipe_handler }, NULL);
  int srv = create_socket(ws->host, ws->port, bind);
  if (srv < 0) {
    perror("Socket error ");
    exit(1);
  }
  if (listen(srv, 10) < 0) {
    perror("Listen error ");
    exit(1);
  }
  websocket_verbose("WebSocket server is listening from %s:%d\n", (ws->host ? ws->host : "0.0.0.0"), ws->port);
  ws->stop = 0;
  if (!thread) {
    (void)websocket_listen_thread(ws);
  } else {
    pthread_t th;
    if(pthread_create(&th, NULL, websocket_listen_thread, ws)) {
      perror("Create thread error: ");
      exit(1);
    }
    pthread_detach(th);
  }
}
void websocket_send(websocket_client_t* client, char* message, int opcode) {
  if (!client) return;
  #ifndef NDEBUG
  if (opcode == WS_OPCODE_TEXT) {
    websocket_verbose(
      "Sending to #%d (%d): %s\n", client->id, client->fd, message
    );
  } else {
    websocket_verbose(
      "Sending to #%d (%d): %ld bytes\n",
      client->id, client->fd, message ? strlen(message) : 0
    );
  }
  #endif
  _websocket_send(client->fd, message, opcode);
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
  rv->fd = -1;
  rv->next = NULL;
  rv->prev = NULL;
  rv->ws = NULL;
  rv->state = WS_READY_STATE_CONNECTING;
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
/* Client */
ws_context_t * ws_context_new() {
  ws_context_t * rv = _ws_alloc(sizeof(*rv));
  if (!rv) return NULL;
  _ws_zero(rv, sizeof(*rv));
  rv->fd = -1;
  rv->stop = 1;
  rv->state = WS_READY_STATE_CONNECTING;
  rv->on_connected = NULL;
  rv->on_data = NULL;
  rv->on_disconnected = NULL;
  return rv;
}
void ws_context_destroy(ws_context_t * ctx) {
  if (!ctx) return;
  ws_context_stop(ctx);
  ws_context_set_state(ctx, WS_READY_STATE_CLOSING);
  if (ctx->fd > 0) {
    (void)closesocket(ctx->fd);
  }
  ws_context_set_state(ctx, WS_READY_STATE_CLOSED);
  ctx->on_connected = NULL;
  ctx->on_data = NULL;
  ctx->on_disconnected = NULL;
  _ws_free(ctx);
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
/* Static */
static int is_ws_url(const char * url) {
  if (!url || strlen(url) < 8) {
    return 0;
  }
  if (
    strncasecmp(url, "ws://", 5) == 0 ||
    strncasecmp(url, "wss://", 6) == 0
  ) {
    return 1;
  }
  return 0;
}
static int create_socket(
  const char * host, int port,
  int (*check)(int, const struct sockaddr *, socklen_t)
) {
  struct addrinfo hints, *result, *p;
  int rc, fd, opval = 1;
  memset(&hints, 0, sizeof(struct addrinfo));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;
  char sport[20] = {0};
  sprintf(sport, "%d", port);
  rc = getaddrinfo(host, sport, &hints, &result);
  if (rc != 0) return (-1);
  for(p = result; p; p = p->ai_next) {
    fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
    if (fd < 0) continue;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opval, sizeof(opval)) < 0) {
      closesocket(fd);
      continue;
    }
    rc = check(fd, p->ai_addr, p->ai_addrlen);
    if (rc == 0 || (rc < 0 && errno == EINPROGRESS)) break;
    closesocket(fd);
  }
  freeaddrinfo(result);
  if (!p) return (-1);
  return fd;
}
static void broken_pipe_handler(int sig) {
  (void)sig;
  websocket_verbose("Broken pipe.\n");
}
static void ws_context_set_state(ws_context_t * ctx, int state) {
  if (!ctx) return;
  ctx->state = state;
}
static void _websocket_send(int fd, char * message, int opcode) {
  unsigned int length = (unsigned int)strlen(message);
  unsigned int all_frames = ceil((float)length / MAX_FRAME_BUFFER);
  if(all_frames == 0) {
    all_frames = 1;
  }
  unsigned int max_frames = all_frames - 1;
  unsigned int last_buffer_length = 0;
  unsigned int pad = MAX_FRAME_BUFFER - 10;
  if(length % pad != 0) {
    last_buffer_length = length % pad;
  } else {
    if(length != 0) {
      last_buffer_length = pad;
    }
  }
  unsigned char * ptr = message;

  for(int i = 0; i < all_frames; i++) {
    unsigned char fin = (i != max_frames) ? 0x0 : 0x80;
    unsigned char op = i != 0 ? 0x0 : opcode;
    unsigned int buffer_length = (i != max_frames) ? pad : last_buffer_length;
    unsigned char buf[MAX_FRAME_BUFFER] = {0};
    unsigned int total_length = 0;
    unsigned int pos = 0;
    if(buffer_length <= 125) {
      total_length = buffer_length + 2;
      buf[0] = fin | op;
      buf[1] = (uint64_t)buffer_length & 0x7f;
      pos = 2;
    } else if (buffer_length <= 65535) {
      total_length = buffer_length + 4;
      buf[0] = fin | op;
      buf[1] = 0x7e;
      buf[2] = ((uint64_t)buffer_length >> 8) & 0x7f;
      buf[3] = (uint64_t)buffer_length & 0xff;
      pos = 4;
    } else {
      total_length = buffer_length + 10;
      buf[0] = fin | op;
      buf[1] = 0x7f;
      buf[2] = (unsigned char)(((uint64_t)buffer_length > 56) & 0xff);
      buf[3] = (unsigned char)(((uint64_t)buffer_length > 48) & 0xff);
      buf[4] = (unsigned char)(((uint64_t)buffer_length > 40) & 0xff);
      buf[5] = (unsigned char)(((uint64_t)buffer_length > 32) & 0xff);
      buf[6] = (unsigned char)(((uint64_t)buffer_length > 24) & 0xff);
      buf[7] = (unsigned char)(((uint64_t)buffer_length > 16) & 0xff);
      buf[8] = (unsigned char)(((uint64_t)buffer_length > 8) & 0xff);
      buf[9] = (unsigned char)((uint64_t)buffer_length & 0xff);
      pos = 10;
    }
    ptr += i * pad;
    memcpy(buf + pos, ptr, buffer_length);
    unsigned int left = total_length;
    char * tmp = (char *)&buf[0];
    do {
      int sent = send(fd, tmp, left, 0);
      if(sent < 0) {
        websocket_verbose("Sending message error\n");
        goto end;
      }
      left -= sent;
      tmp += sent;
    } while (left > 0);
  }
end:;
}
static int _parse_ws_url(const char * url, struct url_t * p) {
  if (!is_ws_url(url)) return -1;
  _ws_zero(p->host, sizeof(p->host));
  _ws_zero(p->path, sizeof(p->path));
  p->port = 80;
  p->is_ssl = 0;
  if (
    sscanf(url, "ws://%[^:/]:%d/%s", p->host, &p->port, p->path) == 3 ||
    sscanf(url, "wss://%[^:/]:%d/%s", p->host, &p->port, p->path) == 3
  ) {
    goto ok;
  } else if (
    sscanf(url, "ws://%[^:/]/%s", p->host, p->path) == 2 ||
    sscanf(url, "wss://%[^:/]/%s", p->host, p->path) == 2
  ) {
    goto ok;
  } else if (
    sscanf(url, "ws://%[^:/]:%d", p->host, &p->port) == 2 ||
    sscanf(url, "wss://%[^:/]:%d", p->host, &p->port) == 2
  ) {
    p->path[0] = '\0';
    goto ok;
  } else if (
    sscanf(url, "ws://%[^:/]", p->host) == 1 ||
    sscanf(url, "wss://%[^:/]", p->host) == 1
  ) {
    p->path[0] = '\0';
    goto ok;
  }
  return -1;
ok:
  p->is_ssl = strncasecmp(url, "wss://", 6) == 0 ? 1 : 0;
  return 0;
}

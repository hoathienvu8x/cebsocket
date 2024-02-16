#define _GNU_SOURCE

#include <ws.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/time.h>
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
#define SHA1_BASE64_SIZE (29)
#define WEBSOCKET_GUID   "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

#define HTTP_HEADER_BUFF_SIZE (4096)
#define HTTP_PROP_BUFF_SIZE   (40)
#define HTTP_VAL_BUFF_SIZE    (512)

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
  char address[INET6_ADDRSTRLEN + 1];
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
static void _websocket_send(int fd, char * message, int opcode, int mask);
static int _parse_ws_url(const char * url, struct url_t * p);
static int sha1base64(
  const unsigned char *data, char *encoded, size_t databytes
);
static void base64_decode(
  const unsigned char * src, char * dest, size_t len
);
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
  return rv;
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

static void * websocket_connection_handle(void * data) {
  sigaction(SIGPIPE, &(struct sigaction){ broken_pipe_handler }, NULL);
  websocket_client_t * client = data;
  ssize_t rc;
  enum ws_parse_state {
    WS_PARSE_STATE_METHOD = 0,
    WS_PARSE_STATE_PROP,
    WS_PARSE_STATE_SPACE,
    WS_PARSE_STATE_VAL,
    WS_PARSE_STATE_END
  };
  enum ws_parse_state sta = WS_PARSE_STATE_METHOD;
  char ch;
  int is_cr = 0;
  char header_buf[HTTP_HEADER_BUFF_SIZE] = {0};
  char prop_buf[HTTP_PROP_BUFF_SIZE] = {0};
  char val_buf[HTTP_PROP_BUFF_SIZE] = {0};
  int header_i = 0, prop_i = 0, val_i = 0;
  char ws_key[128] = {0};
  char hostname[512] = {0};
  char version[4] = {0};
  char upgrade[20] = {0};
  char * req = NULL;
  uint16_t header0_16, plen16, plen64;
  uint64_t plen;
  uint8_t opcode = WS_OPCODE_BINARY, is_masked;
  unsigned char mkey[4];
  int code = WS_STATUS_PROTOCOL_ERROR;

  recv_package:
  rc = recv(client->fd, &ch, 1, MSG_WAITALL);
  if(!rc) {
    websocket_verbose("Client disconnected: %s\n", client->address);
    if(client->ws->on_disconnected) {
      client->ws->on_disconnected(client);
    }
    goto recv_done;
  }

  printf("[%d, %c]\n", sta, ch);
  if (sta == WS_PARSE_STATE_METHOD) {
    if (!is_cr) {
      if (ch == '\r') is_cr = 1;
    } else {
      if (ch == '\n') {
        sta = WS_PARSE_STATE_PROP;
        goto recv_package;
      } else {
        goto recv_done;
      }
    }
    *(header_buf + (header_i++)) = ch;
    if (!is_cr) {
      goto recv_package;
    }
    *(header_buf + header_i) = '\0';
    if (
      strncasecmp(header_buf, "GET ", 4) != 0 ||
      strncasecmp(
        header_buf + 5 + strcspn(header_buf + 5," ") + 1,
        "HTTP/1.", 7
      ) != 0
    ) {
      goto recv_done;
    }
    if(client->ws->middware) {
      char path[512] = {0};
      memcpy(path, header_buf + 5, strcspn(header_buf + 5, "?#"));
      path[strlen(path)] = '\0';
      if (!client->ws->middware(path)) {
        goto recv_done;
      }
    }
    sta = WS_PARSE_STATE_PROP;
  } else if (sta == WS_PARSE_STATE_PROP) {
    if(ch == '\r' || ch == '\n') {
      if (ch != '\n') {
        sta = WS_PARSE_STATE_END;
      }
      goto recv_package;
    }
    if (ch != ':') {
      *(prop_buf + (prop_i++)) = ch;
      goto recv_package;
    }
    *(prop_buf + prop_i) = '\0';
    prop_i = 0;
    sta = WS_PARSE_STATE_SPACE;
  } else if (sta == WS_PARSE_STATE_SPACE) {
    if (ch != ' ') goto recv_done;
    sta = WS_PARSE_STATE_VAL;
    memset(val_buf, 0, sizeof(val_buf));
  } else if (sta == WS_PARSE_STATE_VAL) {
    if (ch != '\n' && ch != '\r') {
      *(val_buf + (val_i++)) = ch;
      goto recv_package;
    }
    *(val_buf + val_i) = '\0';
    val_i = 0;
    printf("prop_buf = %s\n",prop_buf);
    if (strcasecmp(prop_buf, "sec-websocket-key") == 0) {
      memcpy(ws_key, val_buf, strlen(val_buf));
    } else if (strcasecmp(prop_buf, "host") == 0) {
      memcpy(hostname, val_buf, strlen(val_buf));
    } else if (strcasecmp(prop_buf, "sec-websocket-version") == 0) {
      memcpy(version, val_buf, strlen(val_buf));
    } else if (strcasecmp(prop_buf, "upgrade") == 0) {
      memcpy(upgrade, val_buf, strlen(val_buf));
    }
    memset(prop_buf, 0, sizeof(prop_buf));
    sta = WS_PARSE_STATE_PROP;
  } else if (sta == WS_PARSE_STATE_END) {
    printf("ws_key = %s\nhostname = %s\nversion = %s\nupgrade = %s\n",ws_key,hostname, version, upgrade);
    if (
      strlen(ws_key) == 0 || strlen(hostname) == 0 ||
      strlen(version) == 0 || strlen(upgrade) == 0
    ) {
      goto recv_done;
    }
    if(strcasestr(upgrade, "websocket") == NULL) {
      goto recv_done;
    }
    char decoded[20] = {0};
    base64_decode(ws_key, decoded, strlen(ws_key));
    if(strlen(decoded) != 16) {
      goto recv_done;
    }
    if (atoi(version) < 7) {
      goto recv_done;
    }
    strcat(ws_key, WEBSOCKET_GUID);
    char accept[SHA1_BASE64_SIZE] = {0};
    if (sha1base64(ws_key, accept, strlen(ws_key))) {
      goto recv_done;
    }
    char buf[4096] = {0};
    strcat(
      buf,"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
      "Connection: Upgrade\r\nSec-WebSocket-Accept: "
    );
    strcat(buf, accept);
    strcat(buf, "\r\nHost: ");
    strcat(buf, hostname);
    strcat(buf, "\r\n\r\n");
    char * tmp = &buf[0];
    int left = (int)strlen(buf);
    do {
      rc = send(client->fd, tmp, strlen(tmp), 0);
      if (rc < 0) {
        goto recv_done;
      }
      left -= rc;
      tmp += rc;
    } while (left > 0);
    if(client->ws->on_connected) {
      client->ws->on_connected(client);
    }
    goto recv_frame;
  }
  goto recv_package;

recv_frame:
  rc = recv(client->fd, &header0_16, 2, MSG_WAITALL);
  if (!rc) {
    code = WS_STATUS_UNSUPPORTED_DATA_TYPE;
    goto recv_done;
  }
  opcode = ((uint8_t)header0_16) & 0x0f;
  is_masked = (*(((uint8_t*)(&header0_16))+1)) & -128;
  plen = (*(((uint8_t*)(&header0_16))+1)) & 127;
  if (plen == 126) {
    rc = recv(client->fd, &plen16, 2, MSG_WAITALL);
    if (!rc) goto recv_done;
    plen = ntohs(plen16);
  } else if (plen == 127) {
    rc = recv(client->fd, &plen64, 8, MSG_WAITALL);
    if (!rc) goto recv_done;
    plen = ntohs(plen64);
  }
  if (is_masked) {
    rc = recv(client->fd, mkey, 4, MSG_WAITALL);
    if (!rc) goto recv_done;
  }
  req = _ws_alloc(plen + 1);
  req[plen] = '\0';
  rc = recv(client->fd, req, plen, MSG_WAITALL);
  if (!rc) {
    code = WS_STATUS_GOING_AWAY;
    goto recv_done;
  }
  if (is_masked) {
    for(int i = 0; i < plen; i++) {
      req[i] ^= mkey[i % 4];
    }
  }
  websocket_verbose("Data from #%d: %s\n", client->id, req);
  switch (opcode) {
    case WS_OPCODE_CLOSE: {
      code = WS_STATUS_NORMAL;
      goto recv_done;
    } break;
    case WS_OPCODE_PING: {
      websocket_send(client, req, WS_OPCODE_PONG);
    } break;
    case WS_OPCODE_PONG: {
      
    } break;
    case WS_OPCODE_TEXT: case WS_OPCODE_BINARY: {
      if (client->ws->on_data) {
        client->ws->on_data(client, req, opcode);
      }
    } break;
    default: {
      code = WS_STATUS_UNSUPPORTED_DATA_TYPE;
      goto recv_done;
    } break;
  }
  if (req) _ws_free(req);
  goto recv_frame;

recv_done:
  if (opcode == WS_OPCODE_CLOSE) {
    if (req && strlen(req) > 2) {
      _websocket_send(client->fd, req, WS_OPCODE_CLOSE, 0);
    } else {
      unsigned char msg[2] = { (code >> 8), (code & 0xff) };
      _websocket_send(client->fd, msg, WS_OPCODE_CLOSE, 0);
    }
  }
  if (req) _ws_free(req);
  closesocket(client->fd);
  return data;
}
static void * websocket_listen_thread(void * data) {
  websocket_t * ws = data;
  ws->clients = NULL;
  ws->current = NULL;

  int fd;
  struct sockaddr_in c;
  int clen = sizeof(c);

  while (!ws->stop) {
    fd = accept(ws->fd, (struct sockaddr *)&c, &clen);
    if (fd < 0) {
      perror("Accept error: ");
      exit(1);
    }
    websocket_client_t * cli = websocket_client_new();
    if (!cli) {
      websocket_verbose("Could not create client socket\n");
      exit(1);
    }
    cli->id = ++ws->id;
    cli->ws = ws;
    cli->fd = fd;
    inet_ntop(AF_INET, (void*)&c.sin_addr, cli->address, INET_ADDRSTRLEN);
    websocket_verbose("Client connected: #%d (%s)\n", cli->id, cli->address);
    if (!ws->clients) {
      ws->clients = cli;
    } else {
      cli->prev = ws->current;
      ws->current->next = cli;
    }
    ws->current = cli;
    pthread_t th;
    if (
      pthread_create(&th, NULL, (void*)&websocket_connection_handle, (void *)cli)
    ) {
      websocket_verbose("Could not create client thread\n");
      exit(1);
    }
    pthread_detach(th);
  }
  ws->stop = 1;
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
  websocket_verbose(
    "WebSocket server is listening from %s:%d\n",
    (ws->host ? ws->host : "0.0.0.0"), ws->port
  );
  ws->fd = srv;
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
  _websocket_send(client->fd, message, opcode, 0);
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

websocket_client_t* websocket_client_new() {
  websocket_client_t * rv = _ws_alloc(sizeof(*rv));
  if (!rv) return NULL;
  _ws_zero(rv, sizeof(*rv));
  rv->id = -1;
  rv->fd = -1;
  rv->next = NULL;
  rv->prev = NULL;
  rv->ws = NULL;
  rv->state = WS_READY_STATE_CONNECTING;
  _ws_zero(rv->address, sizeof(rv->address));
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
static void * ws_context_listen(void * data) {
  return data;
}
static int ws_context_handshake(
  ws_context_t * ctx, struct url_t * url, const char * origin
) {
  char buf[4096] = {0};
  char host[512] = {0};
  if (url->port != 80) {
    sprintf(host, "%s:%d", url->host, url->port);
  } else {
    sprintf(host, "%s", url->host);
  }
  unsigned char nonce[17] = {0};
  srand(time(NULL));
  for(int i = 0; i < 16; i++) {
    nonce[i] = rand() & 0xff;
  }
  nonce[16] = '\0';
  char wskey[50] = {0};
  if(sha1base64(nonce, wskey, 16)) {
    return -1;
  }
  sprintf(
    buf, "GET %s HTTP/1.1\r\nUpgrade: websocket\r\nConnection: "
    "Upgrade\r\nHost: %s\r\nSec-WebSocket-Key: %s\r\n"
    "Sec-WebSocket-Version: 13",
    url->path, host, wskey
  );
  if (origin) {
    sprintf(buf + strlen(buf), "\r\nOrigin: %s", origin);
  }
  strcat(buf, "\r\n\r\n");
  printf("%s", buf);
  return -1;
}
int ws_context_connect_origin(
  ws_context_t * ctx, const char * url, const char * origin, int thread
) {
  if (!ctx) return (-1);
  if (!is_ws_url(url)) return (-1);
  struct url_t purl;
  if (_parse_ws_url(url, &purl)) {
    websocket_verbose("Could not connect to '%s'\n", url);
    return -1;
  }
  int fd = create_socket(purl.host, purl.port, connect);
  if (fd < 0) {
    return -1;
  }
  if (ws_context_handshake(ctx, &purl, origin)) {
    websocket_verbose("Handshake fail\n");
    return -1;
  }
  ctx->fd = fd;
  ctx->stop = 0;
  if (ctx->on_connected) {
    ctx->on_connected(ctx);
  }
  if (!thread) {
    (void)ws_context_listen(ctx);
  } else {
    pthread_t th;
    if (
      pthread_create(&th, NULL, (void *)ws_context_listen, (void*)ctx)
    ) {
      websocket_verbose("Could not create socket listen\n");
      return -1;
    }
    pthread_detach(th);
  }
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
  struct timeval tv;
  gettimeofday(&tv, NULL);
  srand(tv.tv_usec * tv.tv_sec);
  int mask = 0;
  do {
    mask = rand();
  } while(mask == 0);
  _websocket_send(ctx->fd, message, opcode, mask);
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
static void _websocket_send(int fd, char * message, int opcode, int mask) {
  unsigned int length = (unsigned int)strlen(message);
  unsigned int all_frames = ceil((float)length / MAX_FRAME_BUFFER);
  if(all_frames == 0) {
    all_frames = 1;
  }
  unsigned int max_frames = all_frames - 1;
  unsigned int last_buffer_length = 0;
  unsigned int pad = MAX_FRAME_BUFFER - 10;
  if (mask) {
    pad += 4;
  }
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
    if (mask) {
      buf[1] |= 0x80;
      memcpy(&buf[pos], &mask, 4);
      pos += 4;
    }
    ptr += i * pad;
    memcpy(buf + pos, ptr, buffer_length);
    if(mask) {
      for(int i = 0; i < buffer_length; i++) {
        buf[pos + i] ^= (buf[pos + i % 4] & 0xff);
      }
    }
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
static int sha1base64(
  const unsigned char *data, char *encoded, size_t databytes
) {
  #define SHA1ROTATELEFT(value, bits) (((value) << (bits)) | ((value) >> (32 - (bits))))

  uint32_t W[80];
  uint32_t H[] = {
    0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0
  };
  uint32_t a, b, c, d, e, f = 0, k = 0;

  uint32_t idx, lidx, widx, didx = 0;

  int32_t wcount;
  uint32_t temp;
  uint64_t databits = ((uint64_t)databytes) * 8;
  uint32_t loopcount = (databytes + 8) / 64 + 1;
  uint32_t tailbytes = 64 * loopcount - databytes;
  uint8_t datatail[128] = {0};

  if (!encoded) return -1;

  if (!data) return -1;
  datatail[0] = 0x80;
  datatail[tailbytes - 8] = (uint8_t) (databits >> 56 & 0xFF);
  datatail[tailbytes - 7] = (uint8_t) (databits >> 48 & 0xFF);
  datatail[tailbytes - 6] = (uint8_t) (databits >> 40 & 0xFF);
  datatail[tailbytes - 5] = (uint8_t) (databits >> 32 & 0xFF);
  datatail[tailbytes - 4] = (uint8_t) (databits >> 24 & 0xFF);
  datatail[tailbytes - 3] = (uint8_t) (databits >> 16 & 0xFF);
  datatail[tailbytes - 2] = (uint8_t) (databits >> 8 & 0xFF);
  datatail[tailbytes - 1] = (uint8_t) (databits >> 0 & 0xFF);

  for (lidx = 0; lidx < loopcount; lidx++) {
    memset (W, 0, 80 * sizeof (uint32_t));
    for (widx = 0; widx <= 15; widx++) {
      wcount = 24;
      while (didx < databytes && wcount >= 0) {
        W[widx] += (((uint32_t)data[didx]) << wcount);
        didx++;
        wcount -= 8;
      }
      while (wcount >= 0) {
        W[widx] += (((uint32_t)datatail[didx - databytes]) << wcount);
        didx++;
        wcount -= 8;
      }
    }
    for (widx = 16; widx <= 31; widx++) {
      W[widx] = SHA1ROTATELEFT ((W[widx - 3] ^ W[widx - 8] ^ W[widx - 14] ^ W[widx - 16]), 1);
    }
    for (widx = 32; widx <= 79; widx++) {
      W[widx] = SHA1ROTATELEFT ((W[widx - 6] ^ W[widx - 16] ^ W[widx - 28] ^ W[widx - 32]), 2);
    }
    a = H[0];
    b = H[1];
    c = H[2];
    d = H[3];
    e = H[4];

    for (idx = 0; idx <= 79; idx++) {
      if (idx <= 19) {
        f = (b & c) | ((~b) & d);
        k = 0x5A827999;
      } else if (idx >= 20 && idx <= 39) {
        f = b ^ c ^ d;
        k = 0x6ED9EBA1;
      } else if (idx >= 40 && idx <= 59) {
        f = (b & c) | (b & d) | (c & d);
        k = 0x8F1BBCDC;
      } else if (idx >= 60 && idx <= 79) {
        f = b ^ c ^ d;
        k = 0xCA62C1D6;
      }
      temp = SHA1ROTATELEFT (a, 5) + f + e + k + W[idx];
      e = d;
      d = c;
      c = SHA1ROTATELEFT (b, 30);
      b = a;
      a = temp;
    }

    H[0] += a;
    H[1] += b;
    H[2] += c;
    H[3] += d;
    H[4] += e;
  }

  if (encoded) {
    static const uint8_t *table = (const unsigned char*)
      "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
      "abcdefghijklmnopqrstuvwxyz"
      "0123456789"
      "+/";
    uint32_t triples[7] = {
      ((H[0] & 0xffffff00) >> 1*8),
      ((H[0] & 0x000000ff) << 2*8) | ((H[1] & 0xffff0000) >> 2*8),
      ((H[1] & 0x0000ffff) << 1*8) | ((H[2] & 0xff000000) >> 3*8),
      ((H[2] & 0x00ffffff) << 0*8),
      ((H[3] & 0xffffff00) >> 1*8),
      ((H[3] & 0x000000ff) << 2*8) | ((H[4] & 0xffff0000) >> 2*8),
      ((H[4] & 0x0000ffff) << 1*8)
    };
    for (int i = 0; i < 7; i++){
      uint32_t x = triples[i];
      encoded[i * 4 + 0] = table[(x >> 3 * 6) % 64];
      encoded[i * 4 + 1] = table[(x >> 2 * 6) % 64];
      encoded[i * 4 + 2] = table[(x >> 1 * 6) % 64];
      encoded[i * 4 + 3] = table[(x >> 0 * 6) % 64];
    }
    encoded[SHA1_BASE64_SIZE - 2] = '=';
    encoded[SHA1_BASE64_SIZE - 1] = '\0';
  }
  return 0;
}
static void base64_decode(
  const unsigned char * src, char * dest, size_t len
) {
  unsigned char dtable[256] = {0}, *pos = (unsigned char *)dest;
  size_t i, cnt;
  static const unsigned char base64_table[65] = 
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  const unsigned char * end = src + len, * in = src;
  while(end - in >= 3) {
    *pos++ = base64_table[in[0] >> 2];
    *pos++ = base64_table[((in[0] & 0x03) << 4) | (in[1] >> 4)];
    *pos++ = base64_table[((in[1] & 0x0f) << 2) | (in[2] >> 6)];
    *pos++ = base64_table[in[2] & 0x3f];
    in += 3;
  }
  if(end - in) {
    *pos++ = base64_table[in[0] >> 2];
    if (end - in == 1) {
      *pos++ = base64_table[(in[0] & 0x03) << 4];
      *pos++ = '=';
    } else {
      *pos++ = base64_table[((in[0] & 0x03) << 4) | (in[1] >> 4)];
      *pos++ = base64_table[(in[1] & 0x0f) << 2];
    }
    *pos++ = '=';
  }
  *pos = '\0';
}

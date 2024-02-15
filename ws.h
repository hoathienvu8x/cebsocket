#ifndef __WS_H_
#define __WS_H_

#ifdef __cplusplus
extern "C" {
#endif

typedef struct websocket websocket_t;
typedef struct websocket_client websocket_client_t;
typedef struct ws_context ws_context_t;

/* Globals */
void websocket_verbose(const char* format, ...);
/* Server */
websocket_t* websocket_new(int port);
void websocket_stop(websocket_t * ws);

void websocket_on_connected(
  websocket_t * ws, void (*on_connected)(websocket_client_t  *)
);
void websocket_on_data(
  websocket_t * ws, void (*on_data)(websocket_client_t *, char *, int)
);
void websocket_on_disconnected(
  websocket_t * ws, void (*on_disconnected)(websocket_client_t *)
);
void websocket_middware(
  websocket_t * ws, int (*middware)(const char *)
);

void websocket_listen(websocket_t* ws, int thread);
void websocket_send(websocket_client_t* client, char* message, int opcode);
void websocket_send_broadcast(
  websocket_client_t* client, char* message, int opcode
);
void websocket_send_all(websocket_t* ws, char* message, int opcode);
int websocket_is_stop(websocket_t * ws);

websocket_client_t* websocket_client_init();
void websocket_destroy(websocket_t* ws);
void websocket_client_destroy(websocket_client_t* client);
void websocket_client_set_state(websocket_client_t* client, int state);
int websocket_client_get_state(websocket_client_t* client);

void broken_pipe_handler(int sig);

/* Client */
ws_context_t * ws_context_new();
void ws_context_destroy(ws_context_t * ctx);
int ws_context_connect(ws_context_t * ctx, const char * url, int thread);
int ws_context_connect_origin(
  ws_context_t * ctx, const char * url, const char * origin, int thread
);
void ws_context_on_connected(
  ws_context_t * ctx, void (*on_connected)(ws_context_t *)
);
void ws_context_on_data(
  ws_context_t * ctx, void (*on_data)(ws_context_t *, char *, int)
);
void ws_context_on_disconnected(
  ws_context_t * ctx, void (*on_disconnected)(ws_context_t *)
);
int ws_context_is_stop(ws_context_t * ctx);
void ws_context_stop(ws_context_t * ctx);
void ws_context_send(ws_context_t * ctx, char * message, int opcode);
int ws_context_get_state(ws_context_t * ctx);

#ifdef __cplusplus
}
#endif

#endif

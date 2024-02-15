#ifndef __WS_H_
#define __WS_H_

#ifdef __cplusplus
extern "C" {
#endif

enum ready_state_t {
  WS_READY_STATE_CONNECTING = 0x0,
  WS_READY_STATE_OPEN       = 0x1,
  WS_READY_STATE_CLOSING    = 0x2,
  WS_READY_STATE_CLOSED     = 0x3
};
enum opcode_t {
  WS_OPCODE_CONTINUE = 0x0,
  WS_OPCODE_TEXT     = 0x1,
  WS_OPCODE_BINARY   = 0x2,
  WS_OPCODE_CLOSE    = 0x8,
  WS_OPCODE_PING     = 0x9,
  WS_OPCODE_PONG     = 0xa
};
// https://github.com/Luka967/websocket-close-codes
enum close_code_t {
  WS_STATUS_NORMAL                = 1000,
  WS_STATUS_GOING_AWAY            = 1001,
  WS_STATUS_PROTOCOL_ERROR        = 1002,
  WS_STATUS_UNSUPPORTED_DATA_TYPE = 1003,
  WS_STATUS_NOT_AVAILABLE         = 1005,
  WS_STATUS_ABNORMAL_CLOSED       = 1006,
  WS_STATUS_INVALID_PAYLOAD       = 1007,
  WS_STATUS_POLICY_VIOLATION      = 1008,
  WS_STATUS_MESSAGE_TOO_BIG       = 1009,
  WS_STATUS_INVALID_EXTENTION     = 1010,
  WS_STATUS_UNEXPECTED_CONDITION  = 1011,
  WS_STATUS_TLS_HANDSHAKE_ERROR   = 1015
};

typedef struct websocket websocket_t;
typedef struct websocket_client websocket_client_t;
typedef struct ws_context ws_context_t;

/* Globals */
void websocket_verbose(const char* format, ...);
/* Server */
websocket_t* websocket_new(int port, const char * host);
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

websocket_client_t* websocket_client_new();
void websocket_destroy(websocket_t* ws);
void websocket_client_destroy(websocket_client_t* client);
void websocket_client_set_state(websocket_client_t* client, int state);
int websocket_client_get_state(websocket_client_t* client);

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

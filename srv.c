#include <ws.h>
#include <stdlib.h>
#include <stdio.h>

static void onopen(websocket_client_t * cli);
static void onmessage(websocket_client_t * cli, char * message, int opcode);
static void onclose(websocket_client_t * cli);

int main(int argc, char ** argv) {
  (void)argc;
  (void)argv;
  websocket_t *ws = websocket_new(8080, NULL);
  if (!ws) {
    printf("Could not create server\n");
    exit(0);
  }
  websocket_on_connected(ws, onopen);
  websocket_on_data(ws, onmessage);
  websocket_on_disconnected(ws, onclose);
  websocket_listen(ws, 0);
  websocket_destroy(ws);
  return 0;
}

static void onopen(websocket_client_t * cli) {
  (void)cli;
  printf("Open\n");
}
static void onmessage(websocket_client_t * cli, char * message, int opcode) {
  if (opcode == WS_OPCODE_TEXT) {
    printf("%s\n", message);
  }
  websocket_send(cli, "good job", WS_OPCODE_TEXT);
}
static void onclose(websocket_client_t * cli) {
  printf("Closed\n");
}

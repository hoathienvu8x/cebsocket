#include <ws.h>
#include <stdlib.h>
#include <stdio.h>

static void onopen(ws_context_t * ctx);
static void onmessage(ws_context_t * ctx, char * message, int opcode);
static void onclose(ws_context_t * ctx);

int main(int argc, char ** argv) {
  if (argc < 2) {
    exit(1);
  }
  ws_context_t *cli = ws_context_new();
  if (!cli) {
    printf("Could not create websocket context\n");
    exit(0);
  }
  ws_context_on_connected(cli, onopen);
  ws_context_on_data(cli, onmessage);
  ws_context_on_disconnected(cli, onclose);
  if (argc > 2) {
    ws_context_connect_origin(cli, argv[1], argv[2], 0);
  } else {
    ws_context_connect(cli, argv[1], 0);
  }
  ws_context_destroy(cli);
  return 0;
}

static void onopen(ws_context_t * cli) {
  (void)cli;
  printf("Open\n");
  ws_context_send(cli, "2", WS_OPCODE_TEXT);
}
static void onmessage(ws_context_t * cli, char * message, int opcode) {
  if (opcode == WS_OPCODE_TEXT) {
    printf("%s\n", message);
  }
}
static void onclose(ws_context_t * cli) {
  printf("Closed\n");
}

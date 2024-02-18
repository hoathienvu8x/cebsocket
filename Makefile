CC = gcc
CFLAGS += -std=gnu99 -Wall -Wextra -Werror -pedantic
LDFLAGS = -I. -ldl -lpthread -lm
ifeq ($(build),release)
	CFLAGS = -O3
	LDFLAGS += -DNDEBUG=1
else
	CFLAGS = -O0 -g
endif
RM = rm -rf

OBJECTS = ws.o

all: $(OBJECTS)

srv: srv.o $(OBJECTS)
	@$(CC) $(CFLAGS) srv.o $(OBJECTS) -o $@ $(LDFLAGS)

cli: cli.o $(OBJECTS)
	@$(CC) $(CFLAGS) cli.o $(OBJECTS) -o $@ $(LDFLAGS)

%.o: %.c
	@$(CC) -c $(CFLAGS) $< -o $@ $(LDFLAGS)

clean:
	@$(RM) $(OBJECTS) *.o srv cli

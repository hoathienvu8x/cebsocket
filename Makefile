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

%.o: %.c
	@$(CC) -c $(CFLAGS) $< -o $@ $(LDFLAGS)

clean:
	@$(RM) $(OBJECTS)

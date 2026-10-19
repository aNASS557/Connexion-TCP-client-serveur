CC=gcc
CFLAGS=-Wall -Wextra -g

all: client server

client: client.c common.h msg_struct.h
	$(CC) $(CFLAGS) -o client client.c

server: server.c common.h msg_struct.h
	$(CC) $(CFLAGS) -o server server.c

clean:
	rm -f client server

.PHONY: all clean

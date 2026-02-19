CC = gcc
CFLAGS = -Wall -Wextra -g

all: memgrind

memgrind: memgrind.c mymalloc.c
	$(CC) $(CFLAGS) memgrind.c mymalloc.c -o memgrind

clean:
	rm -f memgrind

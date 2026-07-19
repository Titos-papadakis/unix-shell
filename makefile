CC = gcc
CFLAGS = -Wall

all: hy345sh

hy345sh: hy345sh.c
	$(CC) $(CFLAGS) hy345sh.c -o hy345sh

clean:
	rm -f hy345sh

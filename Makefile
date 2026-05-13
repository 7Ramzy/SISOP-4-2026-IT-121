CC      = gcc
CFLAGS  = -Wall -Wextra $(shell pkg-config fuse3 --cflags)
LIBS    = $(shell pkg-config fuse3 --libs)

all: kenz_rescue

kenz_rescue: kenz_rescue.c
	$(CC) $(CFLAGS) -o $@ $< $(LIBS)

clean:
	rm -f kenz_rescue

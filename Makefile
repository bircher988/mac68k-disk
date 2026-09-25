# mac68k-disk - disk images for the classic 68k Macintosh (MFS and HFS)
CC      ?= cc
CFLAGS  ?= -O2 -std=c99 -Wall -Wextra
PREFIX  ?= /usr/local
BINDIR   = $(PREFIX)/bin

SRC = src/main.c src/util.c src/names.c src/macbin.c src/vol.c src/mfs.c src/hfs.c src/btree.c
OBJ = $(SRC:.c=.o)

all: mac68k-disk

mac68k-disk: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ)

src/%.o: src/%.c src/*.h
	$(CC) $(CFLAGS) -c -o $@ $<

install: mac68k-disk
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 mac68k-disk $(DESTDIR)$(BINDIR)/mac68k-disk

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/mac68k-disk

test: mac68k-disk
	tests/run.sh

clean:
	rm -f mac68k-disk $(OBJ)

.PHONY: all install uninstall test clean

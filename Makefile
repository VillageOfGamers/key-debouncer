PREFIX ?= /usr
BINDIR ?= $(PREFIX)/bin
MANDIR ?= $(PREFIX)/share/man/man1
SYSTEMDDIR ?= $(PREFIX)/lib/systemd/system  # Debian package override

CC = gcc
CFLAGS = -O2 -Wall -Wextra
BINARIES = debounced debouncectl

all: $(BINARIES)

DEBDEV_CFLAGS := $(shell pkg-config --cflags libevdev)
DEBDEV_LIBS   := $(shell pkg-config --libs libevdev)

debounced: src/debounced.c
	$(CC) $(CFLAGS) $(DEBDEV_CFLAGS) -o $@ $< $(DEBDEV_LIBS)

debouncectl: src/debouncectl.c
	$(CC) $(CFLAGS) -o $@ $<

install: all
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(BINARIES) $(DESTDIR)$(BINDIR)
	install -d $(DESTDIR)$(MANDIR)
	install -m 644 debounced.1 debouncectl.1 $(DESTDIR)$(MANDIR)
	install -d $(DESTDIR)$(SYSTEMDDIR)
	install -m 644 debounced.service $(DESTDIR)$(SYSTEMDDIR)

clean:
	rm -f $(BINARIES)
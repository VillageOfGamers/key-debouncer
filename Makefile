# Config
SRC_DAEMON = debounced.c
SRC_CTL    = debouncectl.c
TARGET_DAEMON = debounced
TARGET_CTL    = debouncectl
OUTDIR = bin
OUTBIN_DAEMON = $(OUTDIR)/$(TARGET_DAEMON)
OUTBIN_CTL    = $(OUTDIR)/$(TARGET_CTL)

DEBUGSYM_DAEMON = $(OUTBIN_DAEMON).pdb
DEBUGSYM_CTL    = $(OUTBIN_CTL).pdb

PREFIX ?= /usr/local
BINDIR = $(PREFIX)/bin
SYSTEMD_UNITDIR = /etc/systemd/system
SERVICE = debounced.service

MODE ?= release
CC = gcc
CFLAGS_COMMON = -Wall -Wextra -std=gnu99

ifeq ($(MODE),release)
    CFLAGS = $(CFLAGS_COMMON) -O2 -march=native
    LDFLAGS = -s
else ifeq ($(MODE),dev)
    CFLAGS = $(CFLAGS_COMMON) -O0 -g
else
    $(error Unknown MODE '$(MODE)'; use 'release' or 'dev')
endif

# Targets
all: $(OUTBIN_DAEMON) $(OUTBIN_CTL)

$(OUTBIN_DAEMON): $(SRC_DAEMON)
	mkdir -p $(OUTDIR)
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

$(OUTBIN_CTL): $(SRC_CTL)
	mkdir -p $(OUTDIR)
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS) -lncurses

install: all
	install -Dm755 $(OUTBIN_DAEMON) $(DESTDIR)$(BINDIR)/$(TARGET_DAEMON)
	install -Dm755 $(OUTBIN_CTL) $(DESTDIR)$(BINDIR)/$(TARGET_CTL)
	install -Dm644 $(SERVICE) $(DESTDIR)$(SYSTEMD_UNITDIR)/$(SERVICE)
	@echo "Adding $(SUDO_USER) to input group..."
	@usermod -aG input $(SUDO_USER)
	@echo "You may need to log out and back in for group changes to take effect."

clean:
	$(RM) -r $(OUTDIR)

.PHONY: all clean install
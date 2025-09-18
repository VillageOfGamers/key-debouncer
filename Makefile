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
GROUP = input

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
	$(CC) $(CFLAGS) -I/usr/include/libevdev-1.0 $< -o $@ $(LDFLAGS) -levdev

$(OUTBIN_CTL): $(SRC_CTL)
	mkdir -p $(OUTDIR)
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

install: all
	@install -Dm755 $(OUTBIN_DAEMON) $(DESTDIR)$(BINDIR)/$(TARGET_DAEMON)
	@install -Dm755 $(OUTBIN_CTL) $(DESTDIR)$(BINDIR)/$(TARGET_CTL)
	@install -Dm644 $(SERVICE) $(DESTDIR)$(SYSTEMD_UNITDIR)/$(SERVICE)
	@if [ -n "$$SUDO_USER" ]; then \
		if ! id -nG "$$SUDO_USER" | grep -qw "$(GROUP)"; then \
			echo "Adding user $$SUDO_USER to group $(GROUP)..."; \
			usermod -aG $(GROUP) "$$SUDO_USER"; \
			echo "You may need to log out and log back in for the group changes to take effect."; \
		else \
			echo "User $$SUDO_USER is already in group $(GROUP), nothing to do."; \
		fi \
	else \
		echo "Not run via sudo, skipping user group check."; \
	fi

clean:
	$(RM) -r $(OUTDIR)

.PHONY: all clean install
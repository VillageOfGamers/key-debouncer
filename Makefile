# Config
SRC = debounce.c
TARGET = debounce
OUTDIR = bin
OUTBIN = $(OUTDIR)/$(TARGET)
DEBUGSYM = $(OUTBIN).pdb
SCRIPT = debounce.sh
KBSCRIPT = list-keyboards.sh
SERVICE = debounce.service

PREFIX ?= /usr/local
BINDIR = $(PREFIX)/bin
SYSTEMD_UNITDIR = /etc/systemd/system

MODE ?= release
CC = gcc
CFLAGS_COMMON = -Wall -Wextra -std=gnu99

ifeq ($(MODE),release)
    CFLAGS += $(CFLAGS_COMMON) -O3 -march=native
    LDFLAGS += -s
else ifeq ($(MODE),dev)
    CFLAGS += $(CFLAGS_COMMON) -O0 -g
else
    $(error Unknown MODE '$(MODE)'; use 'release' or 'dev')
endif

# Targets
all: $(OUTBIN)
ifeq ($(MODE),dev)
	@echo "Extracting debug symbols to $(DEBUGSYM)"
	objcopy --only-keep-debug $(OUTBIN) $(DEBUGSYM)
	objcopy --strip-debug --add-gnu-debuglink=$(DEBUGSYM) $(OUTBIN)
endif

$(OUTBIN): $(SRC)
	mkdir -p $(OUTDIR)
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

install: all
	install -Dm755 $(OUTBIN) $(DESTDIR)$(BINDIR)/$(TARGET)
	install -Dm755 $(SCRIPT) $(DESTDIR)$(BINDIR)/$(SCRIPT)
	install -Dm755 $(KBSCRIPT) $(DESTDIR)$(BINDIR)/$(KBSCRIPT)
	install -Dm644 $(SERVICE) $(DESTDIR)$(SYSTEMD_UNITDIR)/$(SERVICE)

clean:
	$(RM) $(OUTDIR)

.PHONY: all clean install

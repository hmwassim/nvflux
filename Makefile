# Minimal Makefile for nvflux

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -fstack-protector-strong -D_FORTIFY_SOURCE=2 -Wformat -Wformat-security

.PHONY: all install uninstall clean

all: nvflux

nvflux: nvflux.c
	$(CC) $(CFLAGS) -o $@ $<

install: nvflux
	install -Dm0755 nvflux $(DESTDIR)$(BINDIR)/nvflux
	chown root:root $(DESTDIR)$(BINDIR)/nvflux
	chmod 4755 $(DESTDIR)$(BINDIR)/nvflux
	@echo "nvflux installed to $(DESTDIR)$(BINDIR)/nvflux"

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/nvflux
	rm -rf ~/.local/state/nvflux
	@echo "nvflux uninstalled"

clean:
	rm -f nvflux

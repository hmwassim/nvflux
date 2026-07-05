# Minimal Makefile for nvflux

PREFIX    ?= /usr/local
BINDIR    ?= $(PREFIX)/bin
STATEDIR  ?= /var/lib/nvflux
AUTOSTART ?= /etc/xdg/autostart/nvflux-restore.desktop
BACKUP    ?= $(BINDIR)/nvflux.bak

CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -fstack-protector-strong -D_FORTIFY_SOURCE=2 -Wformat -Wformat-security

.PHONY: all install uninstall clean

all: nvflux

nvflux: nvflux.c
	$(CC) $(CFLAGS) -o $@ $<

install: nvflux
	install -Dm4755 nvflux $(DESTDIR)$(BINDIR)/nvflux
	install -dm755 $(DESTDIR)$(STATEDIR)
	install -dm755 $(dir $(DESTDIR)$(AUTOSTART))
	printf '[Desktop Entry]\nType=Application\nName=nvflux\nComment=Restore NVIDIA GPU clock profile\nExec=$(BINDIR)/nvflux --restore\nTerminal=false\nCategories=Utility;\nHidden=false\nX-GNOME-Autostart-enabled=true\nX-KDE-autostart-after=panel\n' > $(DESTDIR)$(AUTOSTART)
	chmod 644 $(DESTDIR)$(AUTOSTART)
	@echo "nvflux installed to $(DESTDIR)$(BINDIR)/nvflux"

uninstall:
	-$(DESTDIR)$(BINDIR)/nvflux auto 2>/dev/null
	rm -f $(DESTDIR)$(BINDIR)/nvflux
	rm -f $(DESTDIR)$(AUTOSTART)
	rm -rf $(DESTDIR)$(STATEDIR)
	rm -f $(DESTDIR)$(BACKUP)
	@echo "nvflux uninstalled"

clean:
	rm -f nvflux

VERSION = 2.0.0

PREFIX       ?= /usr/local
BINDIR       ?= $(PREFIX)/bin
STATEDIR     ?= /var/lib/nvflux
AUTOSTARTDIR ?= /etc/xdg/autostart
# Where to re-apply the profile after resume; install.sh picks what applies.
SYSTEMDUNITDIR ?= $(PREFIX)/lib/systemd/system
ELOGINDHOOKDIR ?= /etc/elogind/system-sleep
ZZZHOOKDIR     ?= /etc/zzz.d/resume
BUILDDIR     ?= build

CC       ?= cc
CFLAGS   ?= -O2
CFLAGS   += -std=c11 -Wall -Wextra -Wshadow -Wformat=2 -fstack-protector-strong -fPIE
CPPFLAGS += -D_FORTIFY_SOURCE=2 -DVERSION='"$(VERSION)"' -DSTATE_DIR='"$(STATEDIR)"'
LDFLAGS  += -pie -Wl,-z,relro,-z,now
LDLIBS   += -ldl

# Load NVML from a fixed path instead of the system one (used by the tests).
ifdef NVML_LIB
CPPFLAGS += -DNVML_LIB='"$(NVML_LIB)"'
endif

SRCS = $(wildcard src/*.c)
HDRS = $(wildcard src/*.h)
OBJS = $(SRCS:src/%.c=$(BUILDDIR)/%.o)

.PHONY: all install install-systemd install-elogind install-zzz uninstall test clean

GENERATED = $(BUILDDIR)/nvflux.desktop $(BUILDDIR)/nvflux-resume.service $(BUILDDIR)/nvflux-resume.sh

all: $(BUILDDIR)/nvflux $(GENERATED)

$(BUILDDIR)/nvflux: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILDDIR)/%.o: src/%.c $(HDRS) | $(BUILDDIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(GENERATED): $(BUILDDIR)/%: %.in | $(BUILDDIR)
	sed 's|@BINDIR@|$(BINDIR)|' $< > $@

$(BUILDDIR):
	mkdir -p $@

install: all
	install -Dm4755 $(BUILDDIR)/nvflux $(DESTDIR)$(BINDIR)/nvflux
	install -Dm644 $(BUILDDIR)/nvflux.desktop $(DESTDIR)$(AUTOSTARTDIR)/nvflux.desktop
	install -dm755 $(DESTDIR)$(STATEDIR)

install-systemd: $(BUILDDIR)/nvflux-resume.service
	install -Dm644 $< $(DESTDIR)$(SYSTEMDUNITDIR)/nvflux-resume.service

install-elogind: $(BUILDDIR)/nvflux-resume.sh
	install -Dm755 $< $(DESTDIR)$(ELOGINDHOOKDIR)/nvflux

install-zzz: $(BUILDDIR)/nvflux-resume.sh
	install -Dm755 $< $(DESTDIR)$(ZZZHOOKDIR)/nvflux

uninstall:
	-$(DESTDIR)$(BINDIR)/nvflux auto
	rm -f $(DESTDIR)$(BINDIR)/nvflux $(DESTDIR)$(AUTOSTARTDIR)/nvflux.desktop
	rm -f $(DESTDIR)$(SYSTEMDUNITDIR)/nvflux-resume.service
	rm -f $(DESTDIR)$(ELOGINDHOOKDIR)/nvflux $(DESTDIR)$(ZZZHOOKDIR)/nvflux
	rm -rf $(DESTDIR)$(STATEDIR)

# The tests run nvflux against a fake NVML inside a user namespace, so they
# need neither root nor an NVIDIA GPU, and never touch real clocks.
TESTDIR = $(abspath $(BUILDDIR))/test

test:
	@$(MAKE) --no-print-directory BUILDDIR=$(TESTDIR) STATEDIR=$(TESTDIR)/state \
		BINDIR=$(TESTDIR) NVML_LIB=$(TESTDIR)/libnvidia-ml.so.1 \
		$(TESTDIR)/nvflux $(TESTDIR)/libnvidia-ml.so.1 $(TESTDIR)/nvflux-resume.sh
	@tests/run.sh $(TESTDIR)

$(BUILDDIR)/libnvidia-ml.so.1: tests/fake_nvml.c | $(BUILDDIR)
	$(CC) -std=c11 -Wall -Wextra -fPIC -shared -DFAKE_DIR='"$(abspath $(BUILDDIR))"' -o $@ $<
	chmod 755 $@ # nvflux refuses libraries others could modify

clean:
	rm -rf $(BUILDDIR)

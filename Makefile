VERSION = 1.0.0

PREFIX     ?= /usr/local
BINDIR     ?= $(PREFIX)/bin
DATADIR    ?= $(PREFIX)/share
UNITDIR    ?= $(PREFIX)/lib/systemd/system
STATEDIR   ?= /var/lib/nvflux
RUNDIR     ?= /run/nvflux
BUILDDIR   ?= build

CC     ?= cc
CFLAGS ?= -O2 -g

# nvflux's own flags are kept apart from CFLAGS, CPPFLAGS and LDFLAGS, so a
# packager's flags add to them instead of replacing them.
NVFLUX_CFLAGS   = -std=c11 -Wall -Wextra -Wshadow -Wformat=2 -Wstrict-prototypes \
                  -fstack-protector-strong -fPIE
NVFLUX_CPPFLAGS = -DVERSION='"$(VERSION)"' -DSTATE_DIR='"$(STATEDIR)"' -DRUN_DIR='"$(RUNDIR)"'
NVFLUX_LDFLAGS  = -pie -Wl,-z,relro,-z,now
LDLIBS         += -ldl

ifeq (,$(findstring _FORTIFY_SOURCE,$(CPPFLAGS) $(CFLAGS)))
NVFLUX_CPPFLAGS += -D_FORTIFY_SOURCE=2
endif

# Test builds point these at fakes; normal builds never set them.
ifdef NVML_LIB
NVFLUX_CPPFLAGS += -DNVML_LIB='"$(NVML_LIB)"'
endif
ifdef SYSFS_ROOT
NVFLUX_CPPFLAGS += -DSYSFS_ROOT='"$(SYSFS_ROOT)"' -DPROC_ROOT='"$(PROC_ROOT)"'
endif

SRCS = $(wildcard src/*.c)
HDRS = $(wildcard src/*.h)
OBJS = $(SRCS:src/%.c=$(BUILDDIR)/%.o)

.PHONY: all install install-systemd test unit integration installer check fuzz clean

all: $(BUILDDIR)/nvflux

$(BUILDDIR)/nvflux: $(OBJS)
	$(CC) $(NVFLUX_CFLAGS) $(CFLAGS) $(NVFLUX_LDFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILDDIR)/%.o: src/%.c $(HDRS) | $(BUILDDIR)
	$(CC) $(NVFLUX_CPPFLAGS) $(CPPFLAGS) $(NVFLUX_CFLAGS) $(CFLAGS) -c -o $@ $<

$(BUILDDIR):
	mkdir -p $@

# ---------------------------------------------------------------------------
# For packagers. install.sh does all of this plus the group, the setuid bit
# and the right boot and sleep hooks for the running system. A package must
# create the nvflux group and install the binary as root:nvflux mode 4750.

install: all
	install -Dm755 $(BUILDDIR)/nvflux $(DESTDIR)$(BINDIR)/nvflux
	install -Dm644 man/nvflux.1 $(DESTDIR)$(DATADIR)/man/man1/nvflux.1
	install -Dm644 completions/nvflux.bash $(DESTDIR)$(DATADIR)/bash-completion/completions/nvflux
	install -Dm644 completions/_nvflux $(DESTDIR)$(DATADIR)/zsh/site-functions/_nvflux
	install -Dm644 completions/nvflux.fish $(DESTDIR)$(DATADIR)/fish/vendor_completions.d/nvflux.fish

install-systemd:
	@mkdir -p $(DESTDIR)$(UNITDIR)
	@for u in reload suspend resume; do \
		sed 's|@BINDIR@|$(BINDIR)|g' hooks/nvflux-$$u.service.in > $(DESTDIR)$(UNITDIR)/nvflux-$$u.service; \
	done

# Static analysis (GCC) and shell checks (shellcheck, or its container image).
SHELL_SCRIPTS = install.sh uninstall.sh tests/run.sh tests/hooks/run.sh tests/check_symbols.sh \
                tests/containers/run.sh tests/containers/inner.sh hooks/elogind.in hooks/zzz-suspend.in hooks/zzz-resume.in \
                hooks/pm-utils.in hooks/sysv.in hooks/rc.local.in hooks/local.d.in

check:
	@for f in $(SRCS); do $(CC) -std=c11 -fanalyzer -Wall -Wextra -Werror -O1 -c -o /dev/null $$f || exit 1; done
	@echo "analyzer: clean"
	@if command -v shellcheck >/dev/null 2>&1; then shellcheck -s sh -S warning $(SHELL_SCRIPTS); \
	elif command -v docker >/dev/null 2>&1; then \
		docker run --rm -v "$(CURDIR)":/src:ro -w /src koalaman/shellcheck:stable -s sh -S warning $(SHELL_SCRIPTS); \
	else echo "shellcheck: not available, skipped"; exit 0; fi
	@echo "shellcheck: clean"

# ---------------------------------------------------------------------------
# Tests: unit tests for the pure parts, then the whole program against a fake
# NVIDIA library inside a user namespace. No root, no GPU, real clocks are
# never touched. Everything is built with AddressSanitizer and UBSan.

TESTDIR  = $(abspath $(BUILDDIR))/test
SANITIZE = -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all

test: unit integration installer

unit: | $(TESTDIR)
	@for t in policy state cli; do \
		$(CC) -std=c11 -Wall -Wextra $(SANITIZE) -Isrc -o $(TESTDIR)/test_$$t \
			tests/unit/test_$$t.c src/policy.c src/state.c src/cli.c || exit 1; \
		$(TESTDIR)/test_$$t || exit 1; \
	done

integration: | $(TESTDIR)
	@$(MAKE) --no-print-directory BUILDDIR=$(TESTDIR) CFLAGS='$(SANITIZE)' \
		STATEDIR=$(TESTDIR)/var RUNDIR=$(TESTDIR)/run \
		NVML_LIB=$(TESTDIR)/libnvidia-ml.so.1 SYSFS_ROOT=$(TESTDIR)/sys PROC_ROOT=$(TESTDIR)/proc \
		$(TESTDIR)/nvflux $(TESTDIR)/libnvidia-ml.so.1
	@tests/check_symbols.sh src/nvml.c
	@tests/run.sh $(TESTDIR)

installer: all
	@tests/hooks/run.sh $(abspath $(BUILDDIR))/nvflux

$(TESTDIR):
	mkdir -p $@

$(BUILDDIR)/libnvidia-ml.so.1: tests/fake_nvml.c | $(BUILDDIR)
	$(CC) -std=c11 -Wall -Wextra -fPIC -shared -DFAKE_DIR='"$(abspath $(BUILDDIR))"' -o $@ $<
	chmod 755 $@ # nvflux refuses libraries others could modify

# Fuzz the state parser with libFuzzer (needs clang): make fuzz [FUZZTIME=60]
FUZZTIME ?= 60
fuzz: | $(TESTDIR)
	clang -std=c11 -g -O1 -fsanitize=fuzzer,address,undefined -Isrc -o $(TESTDIR)/state_fuzz \
		tests/fuzz/state_fuzz.c src/state.c src/policy.c
	mkdir -p $(TESTDIR)/fuzz-corpus
	$(TESTDIR)/state_fuzz -max_total_time=$(FUZZTIME) -max_len=4096 $(TESTDIR)/fuzz-corpus

clean:
	rm -rf $(BUILDDIR)

# devsoak - Makefile for bebbo amiga-gcc (m68k-amigaos-gcc)
#
# On a host with m68k-amigaos-gcc on PATH, `make` builds devsoak directly.
# Otherwise `make` shells out to the stefanreinauer/amiga-gcc docker image
# and runs `make devsoak-native` inside it, where the cross compiler is on
# PATH and the direct build rule applies.

CC       := m68k-amigaos-gcc
CFLAGS   := -mcrt=nix13 -m68000 -Os -Wall -Wextra -Wno-unused-parameter
LDFLAGS  := -mcrt=nix13 -m68000
LDLIBS   := -lamiga

SRCDIR   := src
OBJDIR   := obj
SOURCES  := main.c args.c output.c timer.c content.c ring.c ops.c buf.c \
            engine.c stripe.c stats.c worker.c audit.c invariant.c quirks.c \
            scsicmd.c removable.c soft64.c
OBJECTS  := $(SOURCES:%.c=$(OBJDIR)/%.o)
TARGET   := devsoak

DOCKER_IMAGE := stefanreinauer/amiga-gcc:gcc-v16.1
HOST_CC      := $(shell command -v m68k-amigaos-gcc 2>/dev/null)

.PHONY: all clean version devsoak-native dist

ifeq ($(HOST_CC),)

all:
	docker run --rm -v $(CURDIR):/work -w /work $(DOCKER_IMAGE) make devsoak-native

version:
	docker run --rm -v $(CURDIR):/work -w /work $(DOCKER_IMAGE) m68k-amigaos-gcc --version

else

all: $(TARGET)

version:
	$(CC) --version

endif

devsoak-native: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

$(OBJDIR)/%.o: $(SRCDIR)/%.c $(SRCDIR)/devsoak.h | $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJDIR):
	mkdir -p $(OBJDIR)

clean:
	rm -rf $(OBJDIR) $(TARGET) dist

# --- dist: assemble the Aminet upload pair (archive + .readme) --------------
#
# Homebrew's/Ubuntu's `lha` is Lhasa (extract-only) and the last real lha
# *release* tag no longer builds with modern compilers, so a pinned master
# commit is built from source into dist/tools/ instead. Override with a
# known-good archiver: `make dist LHA=/path/to/real/lha`.
LHA_REPO   := https://github.com/jca02266/lha.git
LHA_COMMIT := 86094cb56aba34de45668f39f74fcfb61e9d7fb6
LHA        ?= dist/tools/lha

dist/tools/lha:
	@mkdir -p dist/tools
	rm -rf dist/tools/lha-src
	git clone -q $(LHA_REPO) dist/tools/lha-src
	cd dist/tools/lha-src && \
		git -c advice.detachedHead=false checkout -q $(LHA_COMMIT) && \
		autoreconf -fi >/dev/null 2>&1 && ./configure >/dev/null && \
		$(MAKE) >/dev/null
	cp dist/tools/lha-src/src/lha dist/tools/lha
	rm -rf dist/tools/lha-src

# Rebuilds the binary itself (the release workflow's dist job runs `make
# dist` standalone). The $VER grep confirms the binary just built embeds the
# CURRENT src/version.h DEVSOAK_VERSION; the release workflow's tag-vs-source
# check (scripts/verify-version.sh) separately confirms the tag matches
# src/version.h, closing the loop: tag == src/version.h == the binary.
dist: $(LHA)
	rm -f $(TARGET)
	$(MAKE) all
	@v=$$(sed -n 's/^#define DEVSOAK_VERSION[[:space:]]*"\(.*\)"$$/\1/p' src/version.h); \
	grep -aqF "\$$VER: devsoak $$v (" $(TARGET) || { echo "dist: $(TARGET) lacks \"\$$VER: devsoak $$v (...)\" - stale build/?"; exit 1; }
	rm -rf dist/devsoak
	mkdir -p dist/devsoak
	cp $(TARGET) devsoak.quirks README.md LICENSE dist/devsoak/
	cp devsoak.readme dist/
	cd dist && $(abspath $(LHA)) aq devsoak.lha devsoak

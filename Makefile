# devsoak - Makefile for bebbo amiga-gcc (m68k-amigaos-gcc)
#
# On a host with m68k-amigaos-gcc on PATH, `make` builds devsoak directly.
# Otherwise `make` shells out to the stefanreinauer/amiga-gcc docker image
# and runs `make devsoak-native` inside it, where the cross compiler is on
# PATH and the direct build rule applies.

CC       := m68k-amigaos-gcc
CFLAGS   := -noixemul -m68000 -Os -Wall -Wextra -Wno-unused-parameter
LDFLAGS  := -noixemul -m68000
LDLIBS   := -lamiga

SRCDIR   := src
OBJDIR   := obj
SOURCES  := main.c args.c output.c timer.c content.c ring.c ops.c buf.c engine.c
OBJECTS  := $(SOURCES:%.c=$(OBJDIR)/%.o)
TARGET   := devsoak

DOCKER_IMAGE := stefanreinauer/amiga-gcc:gcc-v16.1
HOST_CC      := $(shell command -v m68k-amigaos-gcc 2>/dev/null)

.PHONY: all clean version devsoak-native

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
	rm -rf $(OBJDIR) $(TARGET)

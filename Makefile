# The make-provided flags like MAKE and CC aren't set, on purpose.
# This is Linux-specific software, so we can depend on GNU make.

# Options:
#	nls	enable translations, default on
#	debug	enable debug symbols, default off
#	nostrip	disable stripping, default off
#	plain	apply neither -g nor -s.
#	xcb	enable libxcb to run unprivileged in Xorg, default on
#	amdgpu	enable amdgpu usage reporting, default auto
#		it requires libdrm >= 2.4.63

PREFIX ?= /usr
INSTALL ?= install
LIBDIR ?= lib
MANDIR ?= share/man

nls ?= 1
xcb ?= 1

# VERSION stamps include/version.h and identifies the compiled sources.  A
# packaging recipe passes the revision it pinned; an empty value leaves
# getver.sh to query the surrounding checkout for a developer build.
VERSION ?=

bin = radeontop
xcblib = libradeontop_xcb.so
# Enumerate the production translation units.  A wildcard absorbs any stray .c
# file left in the root, so the build surface follows the directory contents
# rather than the recipe.  amdgpu.c joins below under its own option, and
# auth_xcb.c builds into the separate xcb shim.
src = auth.c collector.c collector_backend.c detect.c dump.c family_str.c radeon.c \
      radeontop.c ui.c
verh = include/version.h

CFLAGS_SECTIONED = -ffunction-sections -fdata-sections
LDFLAGS_SECTIONED = -Wl,-gc-sections

CFLAGS ?= -Os
# The sources use _GNU_SOURCE interfaces on a C11 base, so the language mode is
# part of the recipe rather than a property of whichever compiler default
# applies.
CFLAGS += -std=gnu11
CFLAGS += -Wall -Wextra -pthread
CFLAGS += -Iinclude
CFLAGS += $(CFLAGS_SECTIONED)
CFLAGS += $(shell pkg-config --cflags pciaccess)
CFLAGS += $(shell pkg-config --cflags libdrm)
ifeq ($(xcb), 1)
	CFLAGS += $(shell pkg-config --cflags xcb xcb-dri2)
	CFLAGS += -DENABLE_XCB=1
endif
CFLAGS += $(shell pkg-config --cflags ncurses 2>/dev/null)

# Comment this if you don't want translations
ifeq ($(nls), 1)
	CFLAGS += -DENABLE_NLS=1
endif

# autodetect libdrm features
ifeq ($(shell pkg-config --atleast-version=2.4.66 libdrm && echo ok), ok)
	CFLAGS += -DHAS_DRMGETDEVICE=1
endif

ifeq ($(shell pkg-config --atleast-version=2 libdrm_amdgpu && echo ok), ok)
	amdgpu ?= 1
else
	amdgpu ?= 0
endif

ifeq ($(amdgpu), 1)
	src += amdgpu.c
	CFLAGS += -DENABLE_AMDGPU=1
	LIBS += $(shell pkg-config --libs libdrm_amdgpu)

	ifeq ($(shell pkg-config --atleast-version=2.4.79 libdrm_amdgpu && echo ok), ok)
		CFLAGS += -DHAS_AMDGPU_QUERY_SENSOR_INFO=1
	endif
endif

ifndef plain
ifdef debug
	CFLAGS += -g
else ifndef nostrip
	CFLAGS += -s
endif
endif

obj = $(src:.c=.o)
LDFLAGS ?= -Wl,-O1
LDFLAGS += $(LDFLAGS_SECTIONED)
LIBS += $(shell pkg-config --libs pciaccess)
LIBS += $(shell pkg-config --libs libdrm)
LIBS += -lm
ifeq ($(xcb), 1)
	xcb_LIBS += $(shell pkg-config --libs xcb xcb-dri2)
	LIBS += -ldl
endif

# On some distros, you might have to change this to ncursesw
LIBS += $(shell pkg-config --libs ncursesw 2>/dev/null || \
		shell pkg-config --libs ncurses 2>/dev/null || \
		echo "-lncurses")

.PHONY: all clean install man dist FORCE

all: $(bin)

ifeq ($(xcb), 1)
all: $(xcblib)

$(xcblib): auth_xcb.c $(wildcard include/*.h) $(verh)
	$(CC) -shared -fPIC -Wl,-soname,$@ -o $@ $< $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $(xcb_LIBS)
endif

$(obj): $(wildcard include/*.h) $(verh)

$(bin): $(obj)
	$(CC) -o $(bin) $(obj) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $(LIBS)

clean:
	rm -f *.o $(bin) $(xcblib)

# FORCE runs getver.sh on every build so a changed VERSION reaches the header;
# getver.sh replaces the header on a changed value only, so the objects that
# include it rebuild on a version change and stay built otherwise.  A .git
# prerequisite instead ties the stamp to a checkout the package build lacks.
$(verh): FORCE
	./getver.sh '$(VERSION)'

FORCE:

trans:
	xgettext -o translations/radeontop.pot -k_ *.c \
	--package-name radeontop

install: all
	$(INSTALL) -D -m755 $(bin) $(DESTDIR)/$(PREFIX)/bin/$(bin)
ifeq ($(xcb), 1)
	$(INSTALL) -D -m755 $(xcblib) $(DESTDIR)/$(PREFIX)/$(LIBDIR)/$(xcblib)
endif
	$(INSTALL) -D -m644 radeontop.1 $(DESTDIR)/$(PREFIX)/$(MANDIR)/man1/radeontop.1
ifeq ($(nls), 1)
	$(MAKE) -C translations install PREFIX=$(PREFIX)
endif

man:
	a2x -f manpage radeontop.asc

dist: ver = $(shell git describe)
dist: name = $(bin)-$(ver)
dist: clean $(verh)
	sed -i '/\t\.\/getver.sh/d' Makefile
	cd .. && \
	ln -s $(bin) $(name) && \
	tar -h --numeric-owner --exclude-vcs -cvf - $(name) | pigz -9 > /tmp/$(name).tgz && \
	rm $(name)
	advdef -z4 /tmp/$(name).tgz
	git checkout Makefile
	cd /tmp && sha1sum $(name).tgz > $(name).tgz.sha1

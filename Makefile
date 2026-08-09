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
PYTHON ?= python3
LIBDIR ?= lib
MANDIR ?= share/man
DATADIR ?= share/radeontop
APPDATADIR ?= share/metainfo
DIST_OUTPUT_DIR ?=
# xgettext otherwise injects the wall clock into a tracked source artifact.
# This date identifies the template revision and changes with its messages.
POT_CREATION_DATE ?= 2026-08-09 00:00+0000

nls ?= 1
xcb ?= 1

# A Git archive carries this generated fragment so downstream builds retain
# the exact source object while regenerating identity for their own toolchain.
source_export_metadata = include/radeontop-source-export.mk
-include $(source_export_metadata)

# VERSION stamps include/version.h and identifies the compiled sources.  A
# packaging recipe passes the revision it pinned; an empty value leaves
# getver.sh to query the surrounding checkout for a developer build.
VERSION ?=
# A source export has no Git metadata.  Its packager supplies both fields so
# the capture header retains the immutable source object and tree state.
SOURCE_COMMIT ?=
SOURCE_STATE ?=

bin = radeontop
xcblib = libradeontop_xcb.so
# Enumerate the production translation units.  A wildcard absorbs any stray .c
# file left in the root, so the build surface follows the directory contents
# rather than the recipe.  amdgpu.c joins below under its own option, and
# auth_xcb.c builds into the separate xcb shim.
src = auth.c capture.c collector.c collector_backend.c detect.c device_model.c dump.c \
	      family_str.c privileges.c radeon.c radeontop.c rs480_observation.c ui.c
verh = include/version.h
source_manifest = include/radeontop-source-manifest.txt
build_manifest = include/radeontop-build-manifest.txt

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

# These files determine the production binary or its dynamically loaded XCB
# helper.  getver.sh hashes their names and contents into the capture identity;
# generated include/version.h stays out to avoid a circular digest.
identity_inputs = Makefile getver.sh $(src) \
	$(if $(filter 1,$(xcb)),auth_xcb.c) \
	$(wildcard $(source_export_metadata)) \
	$(filter-out $(verh),$(wildcard include/*.h))

# On some distros, you might have to change this to ncursesw
LIBS += $(shell pkg-config --libs ncursesw 2>/dev/null || \
		shell pkg-config --libs ncurses 2>/dev/null || \
		echo "-lncurses")

.PHONY: all check check-build-identity check-cli-docs check-dist \
	check-test-dependencies clean install man dist source-intelligence FORCE

all: $(bin)

ifeq ($(xcb), 1)
all: $(xcblib)

$(xcblib): auth_xcb.c $(wildcard include/*.h) $(verh)
	$(CC) -shared -fPIC -Wl,-soname,$@ -o $@ $< $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $(xcb_LIBS)
endif

$(obj): $(wildcard include/*.h) $(verh)

$(bin): $(obj)
	$(CC) -o $(bin) $(obj) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $(LIBS)

# The unit binaries link only the production modules their contracts exercise,
# so they build without ncurses, libdrm, libpciaccess, or a GPU.  Their flags
# stay separate from CFLAGS so a sanitizer lane can rebuild them without
# disturbing the production build.
tests = tests/capture_test tests/collector_test tests/detect_path_test \
	tests/device_model_test tests/privileges_test tests/rs480_observation_test
TEST_CFLAGS ?= -std=gnu11 -O1 -g -Wall -Wextra -Werror

check: $(tests)
	./tests/capture_test
	$(PYTHON) ./tests/capture_json_test.py ./tests/capture_test
	./tests/collector_test
	./tests/detect_path_test
	./tests/device_model_test
	./tests/privileges_test
	./tests/rs480_observation_test
	./tools/check-build-identity.sh --self-test
	./tools/check-dist.sh --self-test
	./tools/check-test-dependencies.sh --self-test

check-build-identity:
	./tools/check-build-identity.sh --self-test

check-cli-docs: $(bin)
	./tools/check-cli-docs.sh --self-test

check-dist:
	./tools/check-dist.sh --self-test

check-test-dependencies:
	./tools/check-test-dependencies.sh --self-test

tests/collector_test: tests/collector_test.c collector.c include/collector.h
	$(CC) $(TEST_CFLAGS) $(CPPFLAGS) -Iinclude -pthread \
		-o $@ tests/collector_test.c collector.c $(TEST_LDFLAGS) -lm

tests/device_model_test: tests/device_model_test.c device_model.c \
		include/device_model.h
	$(CC) $(TEST_CFLAGS) $(CPPFLAGS) -Iinclude \
		-o $@ tests/device_model_test.c device_model.c $(TEST_LDFLAGS)

tests/detect_path_test: tests/detect_path_test.c detect.c device_model.c \
		rs480_observation.c include/device_model.h include/radeontop.h \
		include/rs480_observation.h $(verh)
	$(CC) $(TEST_CFLAGS) $(CPPFLAGS) -Iinclude \
		$(shell pkg-config --cflags libdrm pciaccess) \
		-o $@ tests/detect_path_test.c device_model.c \
		rs480_observation.c $(TEST_LDFLAGS)

tests/privileges_test: tests/privileges_test.c privileges.c \
		include/privileges.h
	$(CC) $(TEST_CFLAGS) $(CPPFLAGS) -Iinclude \
		-o $@ tests/privileges_test.c $(TEST_LDFLAGS)

tests/capture_test: tests/capture_test.c capture.c collector.c device_model.c \
		include/capture.h include/collector.h include/device_model.h
	$(CC) $(TEST_CFLAGS) $(CPPFLAGS) -Iinclude -pthread \
		-o $@ tests/capture_test.c capture.c collector.c device_model.c \
		$(TEST_LDFLAGS) -lm

tests/rs480_observation_test: tests/rs480_observation_test.c \
		rs480_observation.c include/rs480_observation.h
	$(CC) $(TEST_CFLAGS) $(CPPFLAGS) -Iinclude \
		-o $@ tests/rs480_observation_test.c rs480_observation.c \
		$(TEST_LDFLAGS)

clean:
	rm -f *.o $(bin) $(xcblib) $(tests)

source-intelligence:
	@test -n "$(SOURCE_INTELLIGENCE_DIR)" || { \
		echo "set SOURCE_INTELLIGENCE_DIR to an empty output directory" >&2; \
		exit 2; \
	}
	./tools/radeontop-source-intelligence.sh "$(SOURCE_INTELLIGENCE_DIR)"

# FORCE runs getver.sh on every build so source dirtiness and changed build
# options reach the header.  The script replaces the header only when an
# identity field changes, so the objects stay built otherwise.
$(verh): export RADEONTOP_VERSION = $(VERSION)
$(verh): export RADEONTOP_SOURCE_COMMIT = $(SOURCE_COMMIT)
$(verh): export RADEONTOP_SOURCE_STATE = $(SOURCE_STATE)
$(verh): export RADEONTOP_BUILD_CC = $(CC)
$(verh): export RADEONTOP_BUILD_CC_VERSION = $(shell $(CC) --version 2>/dev/null | sed -n '1p')
$(verh): export RADEONTOP_BUILD_CPPFLAGS = $(CPPFLAGS)
$(verh): export RADEONTOP_BUILD_CFLAGS = $(CFLAGS)
$(verh): export RADEONTOP_BUILD_LDFLAGS = $(LDFLAGS)
$(verh): export RADEONTOP_BUILD_LIBS = $(LIBS) $(xcb_LIBS)
$(verh): export RADEONTOP_BUILD_OPTIONS = nls=$(nls) xcb=$(xcb) amdgpu=$(amdgpu)
$(verh): FORCE $(identity_inputs)
	./getver.sh $(sort $(identity_inputs))

FORCE:

trans:
	LC_ALL=C xgettext -o translations/radeontop.pot -k_ *.c \
		--package-name radeontop
	sed -i \
		's/^"POT-Creation-Date:.*$$/"POT-Creation-Date: $(POT_CREATION_DATE)\\n"/' \
		translations/radeontop.pot

install: all
	$(INSTALL) -D -m755 $(bin) $(DESTDIR)/$(PREFIX)/bin/$(bin)
ifeq ($(xcb), 1)
	$(INSTALL) -D -m755 $(xcblib) $(DESTDIR)/$(PREFIX)/$(LIBDIR)/$(xcblib)
endif
	$(INSTALL) -D -m644 radeontop.1 $(DESTDIR)/$(PREFIX)/$(MANDIR)/man1/radeontop.1
	$(INSTALL) -D -m644 $(source_manifest) \
		$(DESTDIR)/$(PREFIX)/$(DATADIR)/source-manifest.txt
	$(INSTALL) -D -m644 $(build_manifest) \
		$(DESTDIR)/$(PREFIX)/$(DATADIR)/build-manifest.txt
	$(INSTALL) -D -m644 radeontop.metainfo.xml \
		$(DESTDIR)/$(PREFIX)/$(APPDATADIR)/com.clbr.radeontop.metainfo.xml
ifeq ($(nls), 1)
	$(MAKE) -C translations install PREFIX=$(PREFIX)
endif

man:
	@diagnostics=$$(mktemp); \
	trap 'rm -f "$$diagnostics"' 0 1 2 15; \
	if ! a2x -f manpage radeontop.asc 2>"$$diagnostics"; then \
		cat "$$diagnostics" >&2; \
		exit 1; \
	fi; \
	if test -s "$$diagnostics"; then \
		cat "$$diagnostics" >&2; \
		exit 1; \
	fi

dist:
	@set -eu; \
	version=$$(git describe --always HEAD); \
	case "$$version" in \
		''|*[!A-Za-z0-9._+~:-]*) \
			echo "Git description cannot enter a source identity: $$version" >&2; \
			exit 2; \
			;; \
	esac; \
	commit=$$(git rev-parse --verify HEAD); \
	epoch=$$(git show -s --format=%ct HEAD); \
	name="$(bin)-$$version"; \
	output_dir="$(DIST_OUTPUT_DIR)"; \
	test -n "$$output_dir" || { \
		echo "DIST_OUTPUT_DIR must name an output directory" >&2; \
		exit 2; \
	}; \
	mkdir -p "$$output_dir"; \
	output_dir=$$(CDPATH='' cd -- "$$output_dir" && pwd -P); \
	scratch=$$(mktemp -d "$${TMPDIR:-/tmp}/radeontop-dist.XXXXXX"); \
	archive_output=; \
	sidecar_output=; \
	trap 'rm -rf "$$scratch"; test -z "$$archive_output" || rm -f "$$archive_output"; test -z "$$sidecar_output" || rm -f "$$sidecar_output"' 0 1 2 15; \
	export_root="$$scratch/$$name"; \
	mkdir -p "$$export_root/include"; \
	chmod 755 "$$export_root"; \
	git archive --format=tar --output="$$scratch/source.tar" "$$commit"; \
	tar -xf "$$scratch/source.tar" -C "$$export_root"; \
	{ \
		printf 'VERSION ?= %s\n' "$$version"; \
		printf 'SOURCE_COMMIT ?= %s\n' "$$commit"; \
		printf '%s\n' 'SOURCE_STATE ?= clean'; \
	} > "$$export_root/$(source_export_metadata)"; \
	tar --sort=name --mtime="@$$epoch" --owner=0 --group=0 --numeric-owner \
		-C "$$scratch" -cf "$$scratch/archive.tar" "$$name"; \
	archive_output=$$(mktemp "$$output_dir/.radeontop-dist.XXXXXX"); \
	gzip -n -9 -c "$$scratch/archive.tar" > "$$archive_output"; \
	mv -f "$$archive_output" "$$output_dir/$$name.tgz"; \
	archive_output=; \
	sidecar_output=$$(mktemp "$$output_dir/.radeontop-dist-sha256.XXXXXX"); \
	( cd "$$output_dir" && sha256sum "$$name.tgz" ) > "$$sidecar_output"; \
	mv -f "$$sidecar_output" "$$output_dir/$$name.tgz.sha256"; \
	sidecar_output=; \
	printf '%s\n' "$$output_dir/$$name.tgz" "$$output_dir/$$name.tgz.sha256"

/*
	Copyright (C) 2012 Lauri Kasanen
	Copyright (C) 2018 Genesis Cloud Ltd.

	This program is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, version 3 of the License.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef RADEONTOP_H
#define RADEONTOP_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "version.h"
#include "gettext.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <locale.h>
#include <stdint.h>

#include "collector.h"

enum {
	GRBM_STATUS = 0x8010,
	// R300-class (RS400/RS480/RS482/RS485) report engine-busy in RBBM_STATUS,
	// not the R600+ GRBM_STATUS.  0x0E40 sits inside the BAR2 SRBM map window,
	// so the MMIO path reads it from srbm_area without a second mmap.
	RBBM_STATUS = 0x0e40,
	SRBM_STATUS = 0xe50,
	SRBM_STATUS2 = 0xe4c,
	MMAP_SIZE = 0x14,
	SRBM_MMAP_SIZE = 0xe54,
	VENDOR_AMD = 0x1002
};

// auth.c
void authenticate_drm(int fd);

// radeontop.c
// die exits, so the noreturn attribute tells the compiler and the analyzers
// that control stops there.  Without it every `if (!p) die(...)` guard reads as
// a path that falls through to the dereference below, and cppcheck reports the
// guarded allocation in collect() as nullPointerOutOfMemory.  cppcheck 2.21.1
// honors the GNU attribute spelling and not the C11 _Noreturn keyword on a
// prototype, and the build already requires GNU C through -std=gnu11.
__attribute__((noreturn)) void die(const char *why);

// detect.c
void init_pci(const char *path, short *bus, unsigned int *device_id, const unsigned char forcemem);
int getfamily(unsigned int id);
void initbits(int fam);
void cleanup();

// The null readers stand for a signal this part does not have.  The backend
// adapter compares against them to build its capability mask, which is what
// separates unsupported from supported-but-failed.
int getuint32_null(uint32_t *out);
int getuint64_null(uint64_t *out);

extern int (*getgrbm)(uint32_t *out);
extern int (*getsrbm)(uint32_t *out);
extern int (*getsrbm2)(uint32_t *out);
extern int (*getvram)(uint64_t *out);
extern int (*getgtt)(uint64_t *out);
extern int (*getsclk)(uint32_t *out);
extern int (*getmclk)(uint32_t *out);

// collector_backend.c
struct collector_backend collector_backend_from_device(void);
struct engine_masks collector_masks_from_bits(void);

// radeontop.c
// A signal handler may only touch a volatile sig_atomic_t, and both output
// modes observe this one so an interrupt reaches the same orderly shutdown that
// a line limit or a UI quit does.
extern volatile sig_atomic_t terminate_requested;

// ui.c
// Returns the process exit status: nonzero when the collector lost its backend.
int present(struct collector *collector, const struct engine_masks *masks,
		const char card[], unsigned int color, unsigned int transparency,
		const unsigned char bus);

// dump.c
// Returns the process exit status: nonzero when the collector lost its backend
// or the output stream failed, because a truncated capture is not a successful
// run.
int dumpdata(struct collector *collector, const struct engine_masks *masks,
		const char file[], const unsigned int limit, const unsigned char bus);

// chips
enum radeon_family {
	UNKNOWN_CHIP,
	RS480,
	R600,
	RV610,
	RV630,
	RV670,
	RV620,
	RV635,
	RS780,
	RS880,
	RV770,
	RV730,
	RV710,
	RV740,
	CEDAR,
	REDWOOD,
	JUNIPER,
	CYPRESS,
	HEMLOCK,
	PALM,
	SUMO,
	SUMO2,
	BARTS,
	TURKS,
	CAICOS,
	CAYMAN,
	ARUBA,
	TAHITI,
	PITCAIRN,
	VERDE,
	OLAND,
	HAINAN,
	BONAIRE,
	KABINI,
	MULLINS,
	KAVERI,
	HAWAII,
	TOPAZ,
	TONGA,
	FIJI,
	CARRIZO,
	STONEY,
	POLARIS11,
	POLARIS10,
	POLARIS12,
	VEGAM,
	VEGA10,
	VEGA12,
	VEGA20,
	RAVEN,
	ARCTURUS,
	NAVI10,
	NAVI14,
	RENOIR,
	NAVI12,
	SIENNA_CICHLID,
	VANGOGH,
	YELLOW_CARP,
	NAVY_FLOUNDER,
	DIMGREY_CAVEFISH,
	ALDEBARAN,
	CYAN_SKILLFISH,
	BEIGE_GOBY,
};

extern const char * const family_str[];

// Register masks only.  A sample, an accumulated count, a point measurement,
// and a published window each have their own type in collector.h; one structure
// standing for all four made a mask and a hit count easy to misread.
struct bits_t {
	unsigned int ee;
	unsigned int vgt;
	unsigned int gui;
	unsigned int ta;
	unsigned int tc;
	unsigned int sx;
	unsigned int sh;
	unsigned int spi;
	unsigned int smx;
	unsigned int sc;
	unsigned int pa;
	unsigned int db;
	unsigned int cb;
	unsigned int cr;
	// R300-class RBBM_STATUS lanes (r300d.h R_000E40): the PM4 command
	// stream, the 2D draw engine, the 2D render backend, and the command-
	// fetch pipe have their own busy bits on these parts; zero masks on
	// every other family.
	unsigned int cp;
	unsigned int e2;
	unsigned int rb2d;
	unsigned int cf;
	unsigned int uvd;
	unsigned int vce0;
};

extern struct bits_t bits;
extern uint64_t vramsize;
extern uint64_t gttsize;
extern unsigned int sclk_max;
extern unsigned int mclk_max;

struct rs480_gart_observed_t {
	unsigned char valid;
	uint32_t agp_base_2;
	uint32_t gart_feature_id;
	uint32_t gart_base;
};

extern struct rs480_gart_observed_t rs480_gart_observed;

// radeon.c
void init_radeon(int fd, int drm_major, int drm_minor, int family);

// amdgpu.c
void init_amdgpu(int fd);
void cleanup_amdgpu();

#endif

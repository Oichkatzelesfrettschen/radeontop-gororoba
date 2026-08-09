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
#include "capture.h"
#include "device_model.h"
#include "privileges.h"
#include "rs480_observation.h"

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
	SRBM_STATUS = 0xe50,
	SRBM_STATUS2 = 0xe4c,
	// The R600+ MMIO path maps a window starting at GRBM_MMAP_BASE, so
	// GRBM_STATUS reads at GRBM_STATUS - GRBM_MMAP_BASE within it.  The
	// R300-class and SRBM registers read from a window based at zero.
	GRBM_MMAP_BASE = 0x8000,
	MMAP_SIZE = 0x14,
	SRBM_MMAP_SIZE = 0xe54,
	VENDOR_AMD = 0x1002
};

// auth.c
void authenticate_drm(int fd);

// radeontop.c
// die exits, so the noreturn attribute tells the compiler and the analyzers
// that control terminates at the call.  Without the attribute every
// `if (!p) die(...)` guard appears to fall through to its guarded dereference,
// and cppcheck reports collect() as nullPointerOutOfMemory.  cppcheck 2.21.1
// honors the GNU attribute spelling and not the C11 _Noreturn keyword on a
// prototype, and the build already requires GNU C through -std=gnu11.
__attribute__((noreturn)) void die(const char *why);

// detect.c
void init_pci(const char *path, short *bus,
		struct radeon_device_identity *identity,
		const unsigned char forcemem);
int getfamily(unsigned int id);
void initbits(int fam);
void cleanup(void);

// The null readers stand for a signal the selected part does not have.  The backend
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
// modes observe terminate_requested so an interrupt reaches the same orderly shutdown that
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
		const struct radeontop_capture_metadata *metadata,
		const char file[], const unsigned int limit, const unsigned char bus);

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

extern struct rs480_gart_observed_t rs480_gart_observed;

// radeon.c
void init_radeon(int fd, int drm_major, int drm_minor, int family);

// amdgpu.c
void init_amdgpu(int fd);
void cleanup_amdgpu(void);

#endif

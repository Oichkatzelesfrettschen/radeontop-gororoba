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

#include "radeontop.h"
#include <pciaccess.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <xf86drm.h>
#include <errno.h>

struct bits_t bits;
uint64_t vramsize;
uint64_t gttsize;
unsigned int sclk_max = 0; // kilohertz
unsigned int mclk_max = 0; // kilohertz
struct rs480_gart_observed_t rs480_gart_observed;
const void *area;
static const void *srbm_area;

// function pointers to the right backend
int (*getgrbm)(uint32_t *out);
int (*getsrbm)(uint32_t *out);
int (*getsrbm2)(uint32_t *out);
int (*getvram)(uint64_t *out);
int (*getgtt)(uint64_t *out);
int (*getsclk)(uint32_t *out);
int (*getmclk)(uint32_t *out);

static int find_pci(short bus, struct pci_device *pci_dev) {
	int ret = pci_system_init();
	if (ret)
		die(_("Failed to init pciaccess"));

	struct pci_device *dev;
	struct pci_id_match match;

	match.vendor_id = VENDOR_AMD;
	match.device_id = PCI_MATCH_ANY;
	match.subvendor_id = PCI_MATCH_ANY;
	match.subdevice_id = PCI_MATCH_ANY;
	match.device_class = 0;
	match.device_class_mask = 0;
	match.match_data = 0;

	struct pci_device_iterator *iter = pci_id_match_iterator_create(&match);

	while ((dev = pci_device_next(iter))) {
		pci_device_probe(dev);
		if ((dev->device_class & 0x00ffff00) != 0x00030000 &&
			(dev->device_class & 0x00ffff00) != 0x00038000)
			continue;
		if (bus < 0 || bus == dev->bus) {
			*pci_dev = *dev;
			break;
		}
	}

	pci_iterator_destroy(iter);
	pci_system_cleanup();
	return (dev == NULL);
}

static int getgrbm_pci(uint32_t *out) {
	*out = *(uint32_t *)((const char *) area + 0x10);
	return 0;
}

// R300-class engine-busy is RBBM_STATUS (0x0E40), inside the BAR2 SRBM window,
// so it reads from srbm_area without a second mmap.
static int getgrbm_pci_r300(uint32_t *out) {
	*out = *(uint32_t *)((const char *) srbm_area + RBBM_STATUS);
	return 0;
}

static int getsrbm_pci(uint32_t *out) {
	*out = *(uint32_t *)((const char *) srbm_area + SRBM_STATUS);
	return 0;
}

static int getsrbm2_pci(uint32_t *out) {
	*out = *(uint32_t *)((const char *) srbm_area + SRBM_STATUS2);
	return 0;
}

static void init_rs480_gart_observed(void) {
	static const char path[] = "/sys/kernel/debug/radeon_rs480_candidate_gart_mc_regs";
	FILE *f = fopen(path, "r");
	char line[256];
	unsigned int found = 0;

	rs480_gart_observed.valid = 0;
	if (!f)
		return;

	while (fgets(line, sizeof(line), f)) {
		char *sep = strstr(line, " = ");
		char *end = NULL;
		unsigned long value;

		if (!sep)
			continue;

		errno = 0;
		value = strtoul(sep + 3, &end, 0);
		if (errno || end == sep + 3 || value > UINT32_MAX)
			continue;

		if (!strncmp(line, "AGP_BASE_2", strlen("AGP_BASE_2"))) {
			rs480_gart_observed.agp_base_2 = (uint32_t) value;
			found |= 1U << 0;
		} else if (!strncmp(line, "GART_FEATURE_ID", strlen("GART_FEATURE_ID"))) {
			rs480_gart_observed.gart_feature_id = (uint32_t) value;
			found |= 1U << 1;
		} else if (!strncmp(line, "GART_BASE", strlen("GART_BASE"))) {
			rs480_gart_observed.gart_base = (uint32_t) value;
			found |= 1U << 2;
		}
	}

	fclose(f);
	rs480_gart_observed.valid = (found == 0x7);
}

static void open_pci(struct pci_device *gpu_device) {
	// Warning: gpu_device is a copy of a freed struct. Do not access any pointers!
	int reg = 2;
	if (getfamily(gpu_device->device_id) >= BONAIRE)
		reg = 5;

	if (!gpu_device->regions[reg].size) die(_("Can't get the register area size"));

//	printf("Found area %p, size %lu\n", area, dev->regions[reg].size);

	// Map the register BAR through the PCI sysfs resourceN node rather than
	// /dev/mem.  Modern kernels refuse /dev/mem access to driver-claimed device
	// MMIO (the STRICT_DEVMEM / IO_STRICT_DEVMEM family), so the legacy path dies;
	// the resourceN file is the BAR aperture itself.  Its file offset is therefore
	// BAR-relative (no base_addr term), and MAP_SHARED makes reads hit the device.
	// Only scalar fields of the freed gpu_device copy are read here.
	char respath[64];
	snprintf(respath, sizeof(respath),
			"/sys/bus/pci/devices/%04x:%02x:%02x.%u/resource%d",
			gpu_device->domain_16, gpu_device->bus, gpu_device->dev,
			gpu_device->func, reg);

	int mem = open(respath, O_RDONLY);
	if (mem < 0) die(_("Cannot access GPU registers, are you root?"));

	// RBBM_STATUS and the SRBM window sit at BAR offset 0; the GRBM status
	// window is at 0x8000.  Map offset 0 for every family.
	srbm_area = mmap(NULL, SRBM_MMAP_SIZE, PROT_READ, MAP_SHARED, mem, 0);
	if (srbm_area == MAP_FAILED) die(_("mmap failed"));

	if (getfamily(gpu_device->device_id) == RS480) {
		// R300-class engine-busy is RBBM_STATUS (0x0E40) in the offset-0 window
		// mapped above.  The GRBM window at 0x8000 is an R600+ construct, unused
		// here, so it is not mapped -- and a resourceN mmap of 0x8000 would fail
		// outright if the r300 register BAR is smaller than that.
		getgrbm = getgrbm_pci_r300;
		// This debugfs file is provided by the steinmarder RS480 candidate-regs
		// lane, not stock upstream radeon.  When it exists, capture the static
		// GART/MC observations once before dropping privileges.
		init_rs480_gart_observed();
	} else {
		area = mmap(NULL, MMAP_SIZE, PROT_READ, MAP_SHARED, mem, 0x8000);
		if (area == MAP_FAILED) die(_("mmap failed"));
		getgrbm = getgrbm_pci;
	}

	close(mem);

	getsrbm = getsrbm_pci;
	getsrbm2 = getsrbm2_pci;
}

static void cleanup_pci() {
	munmap((void *) area, MMAP_SIZE);
	munmap((void *) srbm_area, SRBM_MMAP_SIZE);
}

#ifdef HAS_DRMGETDEVICE
static void device_info_drm(int fd, short *bus, unsigned int *device_id);
#endif
static int getuint64_null(uint64_t *out);

static int init_drm(int drm_fd) {
	drmVersionPtr ver = drmGetVersion(drm_fd);
	int family = UNKNOWN_CHIP;

	if (!ver) {
		perror(_("Failed to query driver version"));
		close(drm_fd);
		return -1;
	}

	authenticate_drm(drm_fd);

#ifdef HAS_DRMGETDEVICE
	// Resolve the family up front so init_radeon can skip probes the
	// kernel rejects by design on this part (pre-R600 returns -EINVAL
	// for every RADEON_INFO_READ_REG register).
	{
		short bus_unused = -1;
		unsigned int device_id = 0;
		device_info_drm(drm_fd, &bus_unused, &device_id);
		family = getfamily(device_id);
	}
#endif

	if (strcmp(ver->name, "radeon") == 0)
		init_radeon(drm_fd, ver->version_major, ver->version_minor, family);
	else if (strcmp(ver->name, "amdgpu") == 0)
#ifdef ENABLE_AMDGPU
		init_amdgpu(drm_fd);
#else
		fprintf(stderr, _("amdgpu support is not compiled in (libdrm 2.4.63 required)\n"));
#endif
	else {
		fprintf(stderr, _("Unsupported driver %s\n"), ver->name);
		close(drm_fd);
		drm_fd = -1;
	}

/*	printf("Version %u.%u.%u, name %s\n",
		ver->version_major,
		ver->version_minor,
		ver->version_patchlevel,
		ver->name);*/

	drmFreeVersion(ver);
	return drm_fd;
}

static void open_drm_bus(const struct pci_device *dev) {
	char busid[32];
	snprintf(busid, sizeof(busid), "pci:%04x:%02x:%02x.%u",
			 dev->domain, dev->bus, dev->dev, dev->func);

	int fd = drmOpen(NULL, busid);

	if (fd >= 0)
		init_drm(fd);
	else if (getvram == getuint64_null)
		// Only worth reporting when no earlier DRM pass bound the
		// memory counters; on the R300-class fallback path this open
		// can fail after VRAM/GTT already bound through find_drm.
		printf(_("Failed to open DRM node, no VRAM support.\n"));
}

static int open_drm_path(const char *path) {
	int fd = open(path, O_RDWR);

	if (fd >= 0)
		fd = init_drm(fd);
	else
		fprintf(stderr, _("Failed to open %s: %s\n"),
			path, strerror(errno));

	return fd;
}

#ifdef DRM_DEVICE_GET_PCI_REVISION
#define DRMGETDEVICE(fd, dev)	drmGetDevice2(fd, 0, dev)
#define DRMGETDEVICES(dev, max)	drmGetDevices2(0, dev, max)
#else
#define DRMGETDEVICE(fd, dev)	drmGetDevice(fd, dev)
#define DRMGETDEVICES(dev, max)	drmGetDevices(dev, max)
#endif

#ifdef DRM_BUS_PCI
static int find_drm(short bus, short *device_bus, unsigned int *device_id) {
	drmDevicePtr *devs;
	int count, i, j, fd = -1;

	count = DRMGETDEVICES(NULL, 0);

	if (count <= 0) {
		if (count < 0)
			drmError(-count, _("Failed to find DRM devices"));
		return 1;
	}

	if (!(devs = calloc(count, sizeof(drmDevicePtr))))
		die(_("Failed to allocate memory for DRM\n"));

	if ((count = DRMGETDEVICES(devs, count)) < 0) {
		drmError(-count, _("Failed to get DRM devices"));
		return 1;
	}

	for (i = 0; i < count; i++) {
		if (devs[i]->bustype != DRM_BUS_PCI ||
			devs[i]->deviceinfo.pci->vendor_id != VENDOR_AMD ||
			(bus >= 0 && bus != devs[i]->businfo.pci->bus))
			continue;

		// try render node first, as it does not require to drop master
		for (j = DRM_NODE_MAX - 1; j >= 0; j--) {
			if (!(1 << j & devs[i]->available_nodes))
				continue;
			if ((fd = open_drm_path(devs[i]->nodes[j])) < 0)
				continue;

			*device_bus = devs[i]->businfo.pci->bus;
			*device_id = devs[i]->deviceinfo.pci->device_id;
			break;
		}

		if (fd >= 0)
			break;
	}

	drmFreeDevices(devs, count);
	free(devs);
	return (fd < 0);
}
#endif

#ifdef HAS_DRMGETDEVICE
static void device_info_drm(int fd, short *bus, unsigned int *device_id) {
	drmDevicePtr dev;
	int err;

	if ((err = DRMGETDEVICE(fd, &dev))) {
		drmError(err, _("Failed to get device info"));
		return;
	}

	if (dev->bustype != DRM_BUS_PCI) {
		fprintf(stderr, _("Unsupported bus type %d\n"),
			dev->bustype);
		return;
	}

	*bus = dev->businfo.pci->bus;
	*device_id = dev->deviceinfo.pci->device_id;
	drmFreeDevice(&dev);
}
#endif

// do-nothing backend used as fallback
#define UNUSED(v)	(void) v
static int getuint32_null(uint32_t *out) { UNUSED(out); return -1; }
static int getuint64_null(uint64_t *out) { UNUSED(out); return -1; }

void init_pci(const char *path, short *bus, unsigned int *device_id, const unsigned char forcemem) {
	short device_bus = -1;
	int err = 1;
	getgrbm = getsclk = getmclk = getuint32_null;
	getsrbm = getsrbm2 = getuint32_null;
	getvram = getgtt = getuint64_null;

	if (path) {
		int fd = open_drm_path(path);

		if (fd < 0 || getgrbm == getuint32_null)
			exit(1);

#ifdef HAS_DRMGETDEVICE
		device_info_drm(fd, &device_bus, device_id);
#else
		fprintf(stderr, _("DRM device detection is not compiled in (libdrm 2.4.66 required)\n"));
#endif
		err = 0;
	}

	// If a path was not specified, search and open the first AMD
	// video card, picking the correct PCI bus if provided.
#ifdef DRM_BUS_PCI
	if (!forcemem && err)
		err = find_drm(*bus, &device_bus, device_id);
#endif

	// This is the fallback method for older libdrm that doesn't
	// have drmGetDevices, operating systems where drmGetDevices
	// is not implemented, older radeon kernel driver without GRBM
	// readings, RS4xx/r300-class lanes where DRM read-reg rejects
	// RBBM_STATUS, AMD Catalyst driver or when no driver is loaded.
	if (getgrbm == getuint32_null) {
		struct pci_device pci_dev;
		memset(&pci_dev, 0, sizeof(struct pci_device));
		err = find_pci(*bus, &pci_dev);

		if (!err) {
			// DRM support for VRAM
			open_drm_bus(&pci_dev);

			if (forcemem)
				printf(_("Forcing the direct MMIO register path.\n"));

			if (forcemem || getgrbm == getuint32_null)
				open_pci(&pci_dev);

			device_bus = pci_dev.bus;
			*device_id = pci_dev.device_id;
		}
	}

	if (err)
		die(_("Can't find Radeon cards"));

	bits.vram = (getvram != getuint64_null);
	bits.gtt = (getgtt != getuint64_null);
	*bus = device_bus;
}

void cleanup() {
	cleanup_pci();

#ifdef ENABLE_AMDGPU
	cleanup_amdgpu();
#endif
}

int getfamily(unsigned int id) {

	switch(id) {
		#define CHIPSET(a,b,c) case a: return c;
		#include "r300_pci_ids.h"
		#include "r600_pci_ids.h"
		#undef CHIPSET
	}

	return 0;
}

void initbits(int fam) {

	bits.cp = bits.e2 = bits.rb2d = bits.cf = 0;

	if (fam == RS480) {
		// R300-class RBBM_STATUS (0x0E40) engine-busy layout, shared by
		// RS400/RS480/RS482/RS485.  Every bit position below is the
		// kernel's own decode (r300d.h S_000E40_* field macros).  Map the
		// R300 blocks onto radeontop's gauges and give the blocks with no
		// R600 analogue (the PM4 command stream and the 2D engine pair)
		// their own lanes; bits with no R300 analogue stay zero.  PB_BUSY
		// (bit 24) carries no kernel-documented block meaning and stays
		// unmapped.
		bits.gui = (1U << 31);  // GUI_ACTIVE -- any 2D/3D engine running
		bits.vgt = (1U << 20);  // VAP_BUSY -- vertex assembly front-end
		bits.pa  = (1U << 26);  // GA_BUSY -- geometry/primitive setup
		bits.ee  = (1U << 15);  // ENG_EV_BUSY -- the event engine proper
		bits.cp  = (1U << 16);  // CP_CMDSTRM_BUSY -- PM4 command stream executing
		bits.e2  = (1U << 17);  // E2_BUSY -- the 2D draw engine
		bits.rb2d = (1U << 18) | (1U << 27);  // RB2D|CBA2D -- 2D render backend
		bits.cf  = (1U << 14);  // CF_PIPE_BUSY -- the command/clause fetch pipe
		// A raw RBBM_STATUS histogram under a sustained load shows the busy
		// word saturating at GUI|GA|CP_CMDSTRM|ENG_EV|CF_PIPE -- each of
		// those now has its own lane (gpu|pa|cp|ee|cf).  The kernel decode
		// also names RE_BUSY (21), TAM|TDM|TIM (22|23|25), and RB3D_BUSY
		// (19), but on RS482 silicon those bits never assert (the
		// rasterizer, texture, and render-backend bits stay permanently
		// clear in that histogram).  Perpetual-zero gauges mislead, so
		// those lanes stay masked off; the per-block cache status registers
		// in the 0x4xxx window are the readable alternative and belong to
		// the gated lane, not a poller.
		bits.sc = bits.ta = bits.cb = bits.db = 0;
		bits.tc = bits.sx = bits.sh = bits.spi = bits.smx = bits.cr = 0;
		bits.uvd = 0;   // RS482 has no UVD -- the 3D pipe is the only decoder
		bits.vce0 = 0;  // no VCE
		return;
	}

	// The majority of these is the same from R600 to Southern Islands.

	bits.ee = (1U << 10);
	bits.vgt = (1U << 16) | (1U << 17);
	bits.ta = (1U << 14);
	bits.tc = (1U << 19);
	bits.sx = (1U << 20);
	bits.sh = (1U << 21);
	bits.spi = (1U << 22);
	bits.smx = (1U << 23);
	bits.sc = (1U << 24);
	bits.pa = (1U << 25);
	bits.db = (1U << 26);
	bits.cr = (1U << 27);
	bits.cb = (1U << 30);
	bits.gui = (1U << 31);
	bits.uvd = 0;
	bits.vce0 = 0;

	// R600 has a different texture bit, and only R600 has the TC, CR, SMX bits
	if (fam < RV770) {
		bits.ta = (1U << 18);
	} else {
		bits.tc = 0;
		bits.cr = 0;
		bits.smx = 0;
	}

	if (fam >= RV610 && fam < VEGAM) {
		bits.uvd = (1U << 19);
		if (fam >= CAYMAN) {
			bits.vce0 = (1U << 7);
		}
	}
}

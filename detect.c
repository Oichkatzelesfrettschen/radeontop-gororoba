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
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <xf86drm.h>

struct bits_t bits;
uint64_t vramsize;
uint64_t gttsize;
unsigned int sclk_max = 0; // kilohertz
unsigned int mclk_max = 0; // kilohertz
struct rs480_gart_observed_t rs480_gart_observed;
static const void *area;
static const void *srbm_area;
static int active_drm_fd = -1;

enum drm_open_result {
	DRM_OPEN_FAILED = -1,
	DRM_OPEN_IDENTITY_ONLY,
	DRM_OPEN_BACKEND_READY
};

_Static_assert(sizeof(((struct pci_device *) 0)->regions) /
	sizeof(((struct pci_device *) 0)->regions[0]) == RADEON_PCI_RESOURCE_COUNT,
	"the direct-MMIO resource bound must match libpciaccess");

static bool pci_resource_index_valid(int resource_index) {
	return resource_index >= 0 &&
		resource_index < RADEON_PCI_RESOURCE_COUNT;
}

// function pointers to the right backend
int (*getgrbm)(uint32_t *out);
int (*getsrbm)(uint32_t *out);
int (*getsrbm2)(uint32_t *out);
int (*getvram)(uint64_t *out);
int (*getgtt)(uint64_t *out);
int (*getsclk)(uint32_t *out);
int (*getmclk)(uint32_t *out);

static void set_pci_identity(struct radeon_device_identity *identity,
		uint16_t domain, uint8_t bus, uint8_t device, uint8_t function,
		uint16_t vendor_id, uint16_t device_id) {
	identity->pci_address_valid = true;
	identity->domain = domain;
	identity->bus = bus;
	identity->device = device;
	identity->function = function;
	identity->vendor_id = vendor_id;
	identity->device_id = device_id;
	identity->family = getfamily(device_id);
}

static int find_pci(short bus, struct radeon_device_identity *identity) {
	const bool bind_existing_identity = identity->pci_address_valid;
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
		ret = pci_device_probe(dev);
		if (ret) {
			fprintf(stderr, _("Failed to probe PCI device %04x:%02x:%02x.%u: %s\n"),
				dev->domain_16, dev->bus, dev->dev, dev->func,
				strerror(ret));
			continue;
		}

		if ((dev->device_class & 0x00ffff00) != 0x00030000 &&
			(dev->device_class & 0x00ffff00) != 0x00038000)
			continue;
		if (bind_existing_identity &&
			!radeon_pci_address_matches(identity, dev->domain_16, dev->bus,
				dev->dev, dev->func))
			continue;
		if (!bind_existing_identity && bus >= 0 && bus != dev->bus)
			continue;
		{
			struct radeon_mmio_layout layout;

			set_pci_identity(identity, dev->domain_16, dev->bus, dev->dev,
				dev->func, dev->vendor_id, dev->device_id);

			if (radeon_mmio_layout_for_family(identity->family, &layout)) {
				if (!pci_resource_index_valid(layout.resource_index))
					die(_("The family classifier returned an invalid PCI resource index"));
				identity->resource_index = layout.resource_index;
				identity->resource_size = dev->regions[layout.resource_index].size;
			}
			break;
		}
	}

	pci_iterator_destroy(iter);
	pci_system_cleanup();
	return (dev == NULL);
}

// A register load reaches the device on every call.  The volatile qualifier is
// what requires that: without it the compiler may fold two loads of the same
// mapped address into one, and a sampler that reads the same register every
// period is exactly the shape that permits it, so a window would report a value
// the silicon produced in a stale sample.
static inline uint32_t mmio_read32(const void *map, unsigned offset) {
	return *(const volatile uint32_t *)((const char *) map + offset);
}

// Each load lands inside the mapping it reads.  Both windows end at their last
// register, so a register or mapping change outside the bound stops the build
// rather than reading past the mmap at runtime.
_Static_assert(GRBM_STATUS >= GRBM_MMAP_BASE &&
		GRBM_STATUS - GRBM_MMAP_BASE + sizeof(uint32_t) <= MMAP_SIZE,
		"GRBM_STATUS reads outside the mapped R600+ register window");
_Static_assert(RBBM_STATUS + sizeof(uint32_t) <= SRBM_MMAP_SIZE,
		"RBBM_STATUS reads outside the mapped SRBM window");
_Static_assert(SRBM_STATUS + sizeof(uint32_t) <= SRBM_MMAP_SIZE,
		"SRBM_STATUS reads outside the mapped SRBM window");
_Static_assert(SRBM_STATUS2 + sizeof(uint32_t) <= SRBM_MMAP_SIZE,
		"SRBM_STATUS2 reads outside the mapped SRBM window");

static int getgrbm_pci(uint32_t *out) {
	*out = mmio_read32(area, GRBM_STATUS - GRBM_MMAP_BASE);
	return 0;
}

// R300-class engine-busy is RBBM_STATUS (0x0E40), inside the BAR2 SRBM window,
// so it reads from srbm_area without a second mmap.
static int getgrbm_pci_r300(uint32_t *out) {
	*out = mmio_read32(srbm_area, RBBM_STATUS);
	return 0;
}

static int getsrbm_pci(uint32_t *out) {
	*out = mmio_read32(srbm_area, SRBM_STATUS);
	return 0;
}

static int getsrbm2_pci(uint32_t *out) {
	*out = mmio_read32(srbm_area, SRBM_STATUS2);
	return 0;
}

static void init_rs480_gart_observed(void) {
	static const char path[] = "/sys/kernel/debug/radeon_rs480_candidate_gart_mc_regs";
	FILE *f = fopen(path, "r");
	char line[256];
	struct rs480_gart_parser parser;

	rs480_gart_observed.valid = 0;
	if (!f)
		return;

	rs480_gart_parser_init(&parser);
	while (fgets(line, sizeof(line), f)) {
		if (!strchr(line, '\n') && !feof(f))
			parser.malformed = true;
		rs480_gart_parser_consume(&parser, line);
	}

	if (ferror(f))
		parser.malformed = true;
	if (fclose(f))
		parser.malformed = true;
	rs480_gart_parser_finish(&parser, &rs480_gart_observed);
}

static void open_pci(struct radeon_device_identity *identity) {
	struct radeon_mmio_layout layout;
	const uint64_t barsize = identity->resource_size;
	char respath[96];
	int mem;

	// The family classifier completes before privilege elevation.  An unknown
	// PCI ID carries no proven BAR index, register, or map bound and therefore
	// cannot enter the direct MMIO path.
	if (!identity->pci_address_valid ||
		!radeon_mmio_layout_for_family(identity->family, &layout) ||
		!pci_resource_index_valid(layout.resource_index))
		die(_("Direct MMIO has no validated layout for the selected Radeon PCI ID"));

	if (!barsize) die(_("Can't get the register area size"));

	// Read only offsets the BAR decodes.  On the K8/RS482 platform a
	// non-completing northbridge read is unrecoverable: D18F3x44 reads
	// WdogTmrDis clear, so the northbridge watchdog is armed, and a gated
	// experiment showed both cores freezing with no MCE and no software exit
	// (AMD BKDG #32559 sections 4.4.5.3 and 4.6.4.7).  A mapping wider than the
	// aperture puts that read one dereference away, so the size check runs
	// before mmap rather than after a fault.
	if (barsize < SRBM_MMAP_SIZE)
		die(_("Register BAR is smaller than the status window"));

//	printf("Found area %p, size %lu\n", area, dev->regions[reg].size);

	// Map the register BAR through the PCI sysfs resourceN node rather than
	// /dev/mem.  Modern kernels refuse /dev/mem access to driver-claimed device
	// MMIO (the STRICT_DEVMEM / IO_STRICT_DEVMEM family), so the legacy path dies;
	// the resourceN file is the BAR aperture itself.  Its file offset is therefore
	// BAR-relative (no base_addr term), and MAP_SHARED makes reads hit the device.
	snprintf(respath, sizeof(respath),
			"/sys/bus/pci/devices/%04x:%02x:%02x.%u/resource%d",
			identity->domain, identity->bus, identity->device,
			identity->function, layout.resource_index);

	// Only the exact resource open, mapping, and optional RS480 debugfs read run
	// with the saved root identity.  DRM discovery, authentication, dynamic
	// loading, output, and sampling remain under the invoking user.
	if (privileges_raise_effective())
		die(_("Cannot elevate privileges for direct MMIO access"));

	mem = open(respath, O_RDONLY);
	if (mem < 0) die(_("Cannot access GPU registers, are you root?"));

	// RBBM_STATUS and the SRBM window sit at BAR offset 0; the GRBM status
	// window is at 0x8000.  Map offset 0 for every family.
	srbm_area = mmap(NULL, SRBM_MMAP_SIZE, PROT_READ, MAP_SHARED, mem, 0);
	if (srbm_area == MAP_FAILED) die(_("mmap failed"));

	if (layout.status_source == RADEON_STATUS_PCI_RESOURCE_RBBM) {
		// R300-class engine-busy is RBBM_STATUS (0x0E40) in the offset-0 window
		// held by srbm_area.  The GRBM window at 0x8000 is an R600+ construct,
		// so the R300 path leaves it unmapped.  A resourceN mmap of 0x8000 also
		// fails when the R300 register BAR ends before that offset.
		getgrbm = getgrbm_pci_r300;
		// The optional debugfs file comes from the steinmarder RS480
		// candidate-regs lane rather than stock upstream radeon.  Its static
		// GART/MC observations enter once before privilege drop.
		init_rs480_gart_observed();
	} else {
		// GRBM is an R600 construct, so the RS480 branch keeps the GRBM map off
		// R300-class parts on capability grounds.  The size check is the
		// separate guard: a BAR too small to decode 0x8000 maps out of range.
		// RS482 sysfs reports BAR2 as 0xc0100000-0xc010ffff, 64 KiB.  The GRBM
		// window fits that size, while the family capability still rejects it.
		if (barsize < GRBM_MMAP_BASE + MMAP_SIZE)
			die(_("Register BAR is smaller than the GRBM window"));

		area = mmap(NULL, MMAP_SIZE, PROT_READ, MAP_SHARED, mem, GRBM_MMAP_BASE);
		if (area == MAP_FAILED) die(_("mmap failed"));
		getgrbm = getgrbm_pci;
	}

	if (close(mem))
		die(_("Failed to close the GPU register resource"));
	if (privileges_drop_effective())
		die(_("Failed to drop effective privileges after direct MMIO setup"));

	getsrbm = getsrbm_pci;
	getsrbm2 = getsrbm2_pci;
	identity->status_source = layout.status_source;
	identity->status_register = layout.status_register;
	identity->resource_index = layout.resource_index;
}

static void cleanup_pci(void) {
	// The DRM path maps neither window, and the R300-class path maps only the
	// offset-0 one, so each unmap runs against the pointer that was set.
	if (area)
		munmap((void *) area, MMAP_SIZE);
	if (srbm_area)
		munmap((void *) srbm_area, SRBM_MMAP_SIZE);
}

#ifdef HAS_DRMGETDEVICE
static int device_info_drm(int fd, struct radeon_device_identity *identity);
#endif
static int device_info_busid(int fd, struct radeon_device_identity *identity);
int getuint64_null(uint64_t *out);

static enum drm_open_result init_drm(int drm_fd,
		struct radeon_device_identity *identity,
		bool allow_identity_only) {
	drmVersionPtr ver = drmGetVersion(drm_fd);
	bool backend_supported = false;

	if (!ver) {
		perror(_("Failed to query driver version"));
		close(drm_fd);
		return DRM_OPEN_FAILED;
	}
	if (!ver->name) {
		fprintf(stderr, _("The DRM driver returned no name\n"));
		drmFreeVersion(ver);
		close(drm_fd);
		return DRM_OPEN_FAILED;
	}

#ifdef HAS_DRMGETDEVICE
	// Resolve the family before init_radeon.  Pre-R600 kernels return -EINVAL
	// for every RADEON_INFO_READ_REG query, so the family gate skips them.
	if (device_info_drm(drm_fd, identity) &&
		device_info_busid(drm_fd, identity)) {
		drmFreeVersion(ver);
		close(drm_fd);
		return DRM_OPEN_FAILED;
	}
#else
	if (device_info_busid(drm_fd, identity)) {
		drmFreeVersion(ver);
		close(drm_fd);
		return DRM_OPEN_FAILED;
	}
#endif

	{
		const int driver_length = snprintf(identity->drm_driver,
			sizeof(identity->drm_driver), "%s", ver->name);

		if (driver_length < 0 ||
			(size_t) driver_length >= sizeof(identity->drm_driver)) {
			fprintf(stderr, _("The DRM driver name is too long\n"));
			drmFreeVersion(ver);
			close(drm_fd);
			return DRM_OPEN_FAILED;
		}
	}
	identity->drm_version_major = ver->version_major;
	identity->drm_version_minor = ver->version_minor;
	identity->drm_version_patchlevel = ver->version_patchlevel;

	if (strcmp(ver->name, "radeon") == 0) {
		backend_supported = true;
		authenticate_drm(drm_fd);
		init_radeon(drm_fd, ver->version_major, ver->version_minor,
			identity->family);
	} else if (strcmp(ver->name, "amdgpu") == 0) {
#ifdef ENABLE_AMDGPU
		backend_supported = true;
		authenticate_drm(drm_fd);
		init_amdgpu(drm_fd);
#else
		fprintf(stderr, _("amdgpu support is not compiled in (libdrm 2.4.63 required)\n"));
#endif
	} else {
		fprintf(stderr, _("Unsupported driver %s\n"), ver->name);
	}

	// A forced direct path needs the DRM node only to establish the exact PCI
	// identity.  A missing sampling backend closes the descriptor while the
	// validated identity remains available for same-BDF PCI resource binding.
	if (!backend_supported) {
		close(drm_fd);
		drmFreeVersion(ver);
		return allow_identity_only ? DRM_OPEN_IDENTITY_ONLY : DRM_OPEN_FAILED;
	}

	if (getgrbm != getuint32_null) {
		identity->status_source = !strcmp(ver->name, "radeon") ?
			RADEON_STATUS_RADEON_READ_REG :
			RADEON_STATUS_AMDGPU_READ_MM_REGISTERS;
		identity->status_register = GRBM_STATUS;
		identity->resource_index = RADEON_PCI_RESOURCE_NONE;
		identity->resource_size = 0;
	}

/*	printf("Version %u.%u.%u, name %s\n",
		ver->version_major,
		ver->version_minor,
		ver->version_patchlevel,
		ver->name);*/

	drmFreeVersion(ver);
	active_drm_fd = drm_fd;
	return DRM_OPEN_BACKEND_READY;
}

static int device_info_busid(int fd, struct radeon_device_identity *identity) {
	char *bus_id = drmGetBusid(fd);
	uint16_t domain;
	uint8_t bus, device, function;
	struct pci_device *pci_device;
	int result;

	if (!bus_id) {
		fprintf(stderr, _("The DRM device has no PCI bus identity\n"));
		return -1;
	}
	if (!radeon_parse_pci_bus_id(bus_id, &domain, &bus, &device, &function)) {
		fprintf(stderr, _("The DRM device returned an invalid PCI bus identity\n"));
		drmFreeBusid(bus_id);
		return -1;
	}
	drmFreeBusid(bus_id);

	result = pci_system_init();
	if (result) {
		fprintf(stderr, _("Failed to init pciaccess: %s\n"), strerror(result));
		return -1;
	}
	pci_device = pci_device_find_by_slot(domain, bus, device, function);
	if (!pci_device || pci_device_probe(pci_device)) {
		fprintf(stderr, _("Failed to resolve the DRM device PCI identity\n"));
		pci_system_cleanup();
		return -1;
	}

	set_pci_identity(identity, domain, bus, device, function,
		pci_device->vendor_id, pci_device->device_id);
	pci_system_cleanup();
	return 0;
}

static void open_drm_bus(struct radeon_device_identity *identity) {
	char busid[32];
	const int resource_index = identity->resource_index;
	const uint64_t resource_size = identity->resource_size;
	snprintf(busid, sizeof(busid), "pci:%04x:%02x:%02x.%u",
			identity->domain, identity->bus, identity->device,
			identity->function);

	int fd = drmOpen(NULL, busid);

	if (fd >= 0) {
		(void) init_drm(fd, identity, false);
		// PCI discovery owns the pending direct layout.  DRM binds optional
		// counters and a status reader without discarding that validated BAR.
		identity->resource_index = resource_index;
		identity->resource_size = resource_size;
	} else if (getvram == getuint64_null)
		// Only worth reporting when no established DRM pass bound the
		// memory counters; on the R300-class fallback path the bus open
		// can fail after VRAM/GTT already bound through find_drm.
		fputs(_("Failed to open DRM node, no VRAM support.\n"), stderr);
}

static enum drm_open_result open_drm_path(const char *path,
		struct radeon_device_identity *identity,
		bool allow_identity_only) {
	int fd = open(path, O_RDWR);

	if (fd >= 0)
		return init_drm(fd, identity, allow_identity_only);
	else
		fprintf(stderr, _("Failed to open %s: %s\n"),
			path, strerror(errno));

	return DRM_OPEN_FAILED;
}

#ifdef DRM_DEVICE_GET_PCI_REVISION
#define DRMGETDEVICE(fd, dev)	drmGetDevice2(fd, 0, dev)
#define DRMGETDEVICES(dev, max)	drmGetDevices2(0, dev, max)
#else
#define DRMGETDEVICE(fd, dev)	drmGetDevice(fd, dev)
#define DRMGETDEVICES(dev, max)	drmGetDevices(dev, max)
#endif

#ifdef DRM_BUS_PCI
static int find_drm(short bus, struct radeon_device_identity *identity) {
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
		free(devs);
		return 1;
	}

	for (i = 0; i < count; i++) {
		if (devs[i]->bustype != DRM_BUS_PCI ||
			devs[i]->deviceinfo.pci->vendor_id != VENDOR_AMD ||
			(bus >= 0 && bus != devs[i]->businfo.pci->bus))
			continue;

		// try render node first, as it does not require to drop master
		for (j = DRM_NODE_MAX - 1; j >= 0; j--) {
			struct radeon_device_identity candidate = *identity;

			if (!(1 << j & devs[i]->available_nodes))
				continue;

			set_pci_identity(&candidate, devs[i]->businfo.pci->domain,
				devs[i]->businfo.pci->bus, devs[i]->businfo.pci->dev,
				devs[i]->businfo.pci->func,
				devs[i]->deviceinfo.pci->vendor_id,
				devs[i]->deviceinfo.pci->device_id);

			if (open_drm_path(devs[i]->nodes[j], &candidate, false) !=
				DRM_OPEN_BACKEND_READY)
				continue;
			fd = active_drm_fd;

			*identity = candidate;
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
static int device_info_drm(int fd, struct radeon_device_identity *identity) {
	drmDevicePtr dev;
	int err;

	if ((err = DRMGETDEVICE(fd, &dev))) {
		drmError(err, _("Failed to get device info"));
		return -1;
	}

	if (dev->bustype != DRM_BUS_PCI) {
		fprintf(stderr, _("Unsupported bus type %d\n"),
			dev->bustype);
		drmFreeDevice(&dev);
		return -1;
	}

	set_pci_identity(identity, dev->businfo.pci->domain,
		dev->businfo.pci->bus, dev->businfo.pci->dev,
		dev->businfo.pci->func, dev->deviceinfo.pci->vendor_id,
		dev->deviceinfo.pci->device_id);
	drmFreeDevice(&dev);
	return 0;
}
#endif

// do-nothing backend used as fallback
#define UNUSED(v)	(void) v
int getuint32_null(uint32_t *out) { UNUSED(out); return -1; }
int getuint64_null(uint64_t *out) { UNUSED(out); return -1; }

void init_pci(const char *path, short *bus,
		struct radeon_device_identity *identity,
		const unsigned char forcemem) {
	int err = 1;

	radeon_device_identity_init(identity);
	getgrbm = getsclk = getmclk = getuint32_null;
	getsrbm = getsrbm2 = getuint32_null;
	getvram = getgtt = getuint64_null;

	if (path) {
		enum drm_open_result drm_result = open_drm_path(path, identity,
			forcemem != 0);

		if (drm_result == DRM_OPEN_FAILED)
			exit(1);
		err = 0;
	}

	// If a path was not specified, search and open the first AMD
	// video card, picking the correct PCI bus if provided.
#ifdef DRM_BUS_PCI
	if (!forcemem && err)
		err = find_drm(*bus, identity);
#endif

	// This is the fallback method for older libdrm that doesn't
	// have drmGetDevices, operating systems where drmGetDevices
	// is not implemented, older radeon kernel driver without GRBM
	// readings, RS4xx/r300-class lanes where DRM read-reg rejects
	// RBBM_STATUS, or when no driver is loaded.
	if (radeon_direct_status_path_required(forcemem,
			getgrbm != getuint32_null)) {
		err = find_pci(*bus, identity);

		if (!err) {
			// A prior DRM pass owns its descriptor and memory counters.  The
			// direct-path search opens a DRM node only when that pass did not run.
			if (!path && active_drm_fd < 0 && getvram == getuint64_null)
				open_drm_bus(identity);

			if (forcemem)
				fputs(_("Forcing the direct MMIO register path.\n"), stderr);

			if (forcemem || getgrbm == getuint32_null)
				open_pci(identity);
		}
	}

	if (err)
		die(_("Can't find Radeon cards"));

	if (!identity->pci_address_valid)
		die(_("The selected Radeon device has no PCI identity"));

	*bus = identity->bus;
}

void cleanup(void) {
	cleanup_pci();

#ifdef ENABLE_AMDGPU
	cleanup_amdgpu();
#endif

	if (active_drm_fd >= 0) {
		close(active_drm_fd);
		active_drm_fd = -1;
	}
}

int getfamily(unsigned int id) {

	switch(id) {
		#define CHIPSET(a,b,c) case a: return c;
		#include "r300_pci_ids.h"
		#include "r600_pci_ids.h"
		#undef CHIPSET
	}

	return UNKNOWN_CHIP;
}

void initbits(int fam) {

	bits.cp = bits.e2 = bits.rb2d = bits.cf = 0;

	if (fam == RS480) {
		// R300-class RBBM_STATUS (0x0E40) engine-busy layout, shared by
		// RS400/RS480/RS482/RS485.  Every assigned bit position is the
		// kernel's own decode (r300d.h S_000E40_* field macros).  Map the
		// R300 blocks onto radeontop's gauges and give the blocks with no
		// R600 analogue (the PM4 command stream and the 2D engine pair)
		// their own lanes; bits with no R300 analogue stay zero.
		bits.gui = (1U << 31);  // GUI_ACTIVE -- any 2D/3D engine running
		bits.vgt = (1U << 20);  // VAP_BUSY -- vertex assembly front-end
		bits.pa  = (1U << 26);  // GA_BUSY -- geometry/primitive setup
		bits.ee  = (1U << 15);  // ENG_EV_BUSY -- the event engine proper
		bits.cp  = (1U << 16);  // CP_CMDSTRM_BUSY -- PM4 command stream executing
		bits.e2  = (1U << 17);  // E2_BUSY -- the 2D draw engine
		bits.rb2d = (1U << 18) | (1U << 27);  // RB2D|CBA2D -- 2D render backend
		bits.cf  = (1U << 14);  // CF_PIPE_BUSY -- the command/clause fetch pipe
		// A retained RS482 RBBM_STATUS histogram observes GUI, GA,
		// CP_CMDSTRM, ENG_EV, CF_PIPE, and VAP assertions under its named
		// loads and sample rates.  The same finite record does not observe
		// RB3D, RE, TAM, TDM, PB, or TIM.  Those lanes stay masked because a
		// permanent zero would overstate the bounded non-observation.  Active
		// reads in the GA and ZB 0x4xxx blocks remain probe-only: an RS482
		// read of 0x42d0 under GUI activity deep-wedged the host.
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

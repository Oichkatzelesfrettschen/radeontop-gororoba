/*
	Copyright (C) 2012 Lauri Kasanen

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

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xf86drm.h>

static int test_open(const char *path, int flags, ...);
static int test_close(int fd);
static void *test_mmap(void *address, size_t length, int protection,
	int flags, int fd, off_t offset);
static int test_munmap(void *address, size_t length);

// TEST_DRM_BUS_DISCOVERY selects the DRM enumeration fixture.
// The default build uses legacy PCI iteration.
#ifdef TEST_DRM_BUS_DISCOVERY
#define HAS_DRMGETDEVICE 1
#else
#undef DRM_BUS_PCI
#undef HAS_DRMGETDEVICE
#endif
#undef ENABLE_AMDGPU
#define open test_open
#define close test_close
#define mmap test_mmap
#define munmap test_munmap
#include "../detect.c"
#undef munmap
#undef mmap
#undef close
#undef open

static unsigned int checks;
static unsigned int failures;
static unsigned int explicit_open_count;
static unsigned int unknown_drm_node_open_count;
static unsigned int supported_drm_node_open_count;
static unsigned int resource_open_count;
static unsigned int close_count;
static unsigned int drm_open_count;
static unsigned int authenticate_count;
static unsigned int init_radeon_count;
static unsigned int privilege_raise_count;
static unsigned int privilege_drop_count;
static unsigned int unmap_count;
static bool drm_bus_open_succeeds;
static bool drm_driver_is_radeon;
static size_t iterator_device_count;
static size_t iterator_device_index;
static unsigned char iterator_token;
static uint32_t srbm_window[SRBM_MMAP_SIZE / sizeof(uint32_t)];
static uint32_t grbm_window[MMAP_SIZE / sizeof(uint32_t)];
static struct pci_device selected_device;
static struct pci_device iterator_devices[2];

#ifdef TEST_DRM_BUS_DISCOVERY
static drmPciBusInfo unknown_drm_bus_info;
static drmPciBusInfo supported_drm_bus_info;
static drmPciDeviceInfo unknown_drm_device_info;
static drmPciDeviceInfo supported_drm_device_info;
static char *unknown_drm_nodes[DRM_NODE_MAX];
static char *supported_drm_nodes[DRM_NODE_MAX];
static drmDevice unknown_drm_device;
static drmDevice supported_drm_device;
static drmDevicePtr enumerated_drm_devices[2];
static int enumerated_drm_device_count;
#endif

#define CHECK(condition) do { \
	checks++; \
	if (!(condition)) { \
		fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
		failures++; \
	} \
} while (0)

static int test_drm_status(uint32_t *out) {
	if (!out)
		return -1;
	*out = UINT32_C(0xcafef00d);
	return 0;
}

static int test_open(const char *path, int flags, ...) {
	(void) flags;

	if (!strcmp(path, "/dev/dri/renderD128")) {
		explicit_open_count++;
		return 10;
	}
	if (!strcmp(path, "/dev/dri/unknown")) {
		unknown_drm_node_open_count++;
		return 12;
	}
	if (!strcmp(path, "/dev/dri/supported")) {
		supported_drm_node_open_count++;
		return 10;
	}
	if (!strcmp(path,
		"/sys/bus/pci/devices/0000:01:00.0/resource5")) {
		resource_open_count++;
		return 11;
	}

	errno = ENOENT;
	return -1;
}

static int test_close(int fd) {
	CHECK(fd == 10 || fd == 11 || fd == 12);
	close_count++;
	return 0;
}

static void *test_mmap(void *address, size_t length, int protection,
		int flags, int fd, off_t offset) {
	(void) address;
	CHECK(protection == PROT_READ);
	CHECK(flags == MAP_SHARED);
	CHECK(fd == 11);

	if (offset == 0 && length == SRBM_MMAP_SIZE)
		return srbm_window;
	if (offset == GRBM_MMAP_BASE && length == MMAP_SIZE)
		return grbm_window;
	return MAP_FAILED;
}

static int test_munmap(void *address, size_t length) {
	CHECK((address == srbm_window && length == SRBM_MMAP_SIZE) ||
		(address == grbm_window && length == MMAP_SIZE));
	unmap_count++;
	return 0;
}

drmVersionPtr drmGetVersion(int fd) {
	static char amdgpu_driver_name[] = "amdgpu";
	static char radeon_driver_name[] = "radeon";
	static drmVersion version = {
		.version_major = 3,
		.version_minor = 64,
		.version_patchlevel = 0,
		.name_len = 6,
		.name = amdgpu_driver_name
	};

	CHECK(fd == 10 || fd == 12);
	version.name = drm_driver_is_radeon ? radeon_driver_name :
		amdgpu_driver_name;
	version.name_len = strlen(version.name);
	return &version;
}

void drmFreeVersion(drmVersionPtr version) {
	CHECK(version != NULL);
}

char *drmGetBusid(int fd) {
	CHECK(fd == 10);
	return strdup("pci:0000:01:00.0");
}

void drmFreeBusid(const char *bus_id) {
	free((void *) bus_id);
}

int drmOpen(const char *name, const char *bus_id) {
	(void) name;
	CHECK(!strcmp(bus_id, "pci:0000:01:00.0"));
	drm_open_count++;
	return drm_bus_open_succeeds ? 10 : -1;
}

int pci_system_init(void) {
	return 0;
}

void pci_system_cleanup(void) {
}

struct pci_device *pci_device_find_by_slot(uint32_t domain, uint32_t bus,
		uint32_t device, uint32_t function) {
	CHECK(domain == 0 && bus == 1 && device == 0 && function == 0);
	return &selected_device;
}

int pci_device_probe(struct pci_device *device) {
	bool fixture_device = device == &selected_device;
	size_t device_index;

	for (device_index = 0; device_index < iterator_device_count; device_index++) {
		if (device == &iterator_devices[device_index])
			fixture_device = true;
	}
	CHECK(fixture_device);
	return 0;
}

struct pci_device_iterator *pci_id_match_iterator_create(
		const struct pci_id_match *match) {
	CHECK(match->vendor_id == VENDOR_AMD);
	iterator_device_index = 0;
	return (struct pci_device_iterator *) &iterator_token;
}

struct pci_device *pci_device_next(struct pci_device_iterator *iterator) {
	CHECK(iterator == (struct pci_device_iterator *) &iterator_token);
	if (iterator_device_index >= iterator_device_count)
		return NULL;
	return &iterator_devices[iterator_device_index++];
}

void pci_iterator_destroy(struct pci_device_iterator *iterator) {
	CHECK(iterator == (struct pci_device_iterator *) &iterator_token);
}

#ifndef TEST_DRM_BUS_DISCOVERY
static void initialize_pci_device(struct pci_device *device, uint8_t pci_device,
		uint16_t device_id) {
	memset(device, 0, sizeof(*device));
	device->domain_16 = 0;
	device->bus = 1;
	device->dev = pci_device;
	device->func = 0;
	device->vendor_id = VENDOR_AMD;
	device->device_id = device_id;
	device->device_class = 0x00030000;
	device->regions[5].size = 0x10000;
}

static void configure_pci_iterator(bool include_supported) {
	initialize_pci_device(&iterator_devices[0], 1, 0xffff);
	iterator_device_count = 1;
	if (include_supported) {
		iterator_devices[1] = selected_device;
		iterator_device_count = 2;
	}
}
#endif

#ifdef TEST_DRM_BUS_DISCOVERY
int drmError(int error, const char *label) {
	(void) error;
	(void) label;
	CHECK(false);
	return -1;
}

int drmGetDevice2(int fd, uint32_t flags, drmDevicePtr *device) {
	CHECK(flags == 0);
	CHECK(device != NULL);
	if (!device)
		return -EINVAL;
	if (fd == 10) {
		*device = &supported_drm_device;
		return 0;
	}
	if (fd == 12) {
		*device = &unknown_drm_device;
		return 0;
	}
	return -EINVAL;
}

void drmFreeDevice(drmDevicePtr *device) {
	CHECK(device != NULL && *device != NULL);
	if (!device || !*device)
		return;
	*device = NULL;
}

int drmGetDevices2(uint32_t flags, drmDevicePtr devices[], int max_devices) {
	int device_index;

	CHECK(flags == 0);
	if (!devices)
		return enumerated_drm_device_count;
	CHECK(max_devices >= enumerated_drm_device_count);
	if (max_devices < enumerated_drm_device_count)
		return -EINVAL;
	for (device_index = 0; device_index < enumerated_drm_device_count;
			device_index++)
		devices[device_index] = enumerated_drm_devices[device_index];
	return enumerated_drm_device_count;
}

void drmFreeDevices(drmDevicePtr devices[], int count) {
	CHECK(devices != NULL);
	CHECK(count == enumerated_drm_device_count);
	if (!devices)
		return;
}

static void configure_drm_devices(bool include_supported) {
	memset(&unknown_drm_device, 0, sizeof(unknown_drm_device));
	memset(&supported_drm_device, 0, sizeof(supported_drm_device));
	memset(unknown_drm_nodes, 0, sizeof(unknown_drm_nodes));
	memset(supported_drm_nodes, 0, sizeof(supported_drm_nodes));

	unknown_drm_bus_info = (drmPciBusInfo) {
		.domain = 0, .bus = 1, .dev = 1, .func = 0
	};
	supported_drm_bus_info = (drmPciBusInfo) {
		.domain = 0, .bus = 1, .dev = 0, .func = 0
	};
	unknown_drm_device_info = (drmPciDeviceInfo) {
		.vendor_id = VENDOR_AMD, .device_id = 0xffff
	};
	supported_drm_device_info = (drmPciDeviceInfo) {
		.vendor_id = VENDOR_AMD, .device_id = 0x6650
	};
	unknown_drm_nodes[DRM_NODE_RENDER] = "/dev/dri/unknown";
	supported_drm_nodes[DRM_NODE_RENDER] = "/dev/dri/supported";
	unknown_drm_device.nodes = unknown_drm_nodes;
	unknown_drm_device.available_nodes = 1 << DRM_NODE_RENDER;
	unknown_drm_device.bustype = DRM_BUS_PCI;
	unknown_drm_device.businfo.pci = &unknown_drm_bus_info;
	unknown_drm_device.deviceinfo.pci = &unknown_drm_device_info;
	supported_drm_device.nodes = supported_drm_nodes;
	supported_drm_device.available_nodes = 1 << DRM_NODE_RENDER;
	supported_drm_device.bustype = DRM_BUS_PCI;
	supported_drm_device.businfo.pci = &supported_drm_bus_info;
	supported_drm_device.deviceinfo.pci = &supported_drm_device_info;

	enumerated_drm_devices[0] = &unknown_drm_device;
	enumerated_drm_device_count = 1;
	if (include_supported) {
		enumerated_drm_devices[1] = &supported_drm_device;
		enumerated_drm_device_count = 2;
	}
}
#endif

void authenticate_drm(int fd) {
	(void) fd;
	authenticate_count++;
}

void init_radeon(int fd, int drm_major, int drm_minor, int family) {
	(void) fd;
	(void) drm_major;
	(void) drm_minor;
	(void) family;
	init_radeon_count++;
	getgrbm = test_drm_status;
}

int privileges_raise_effective(void) {
	privilege_raise_count++;
	return 0;
}

int privileges_drop_effective(void) {
	privilege_drop_count++;
	return 0;
}

__attribute__((noreturn)) void die(const char *why) {
	fprintf(stderr, "unexpected die: %s\n", why);
	exit(2);
}

#ifndef TEST_DRM_BUS_DISCOVERY
// Every RS480 mask pinned to the rs400d.h S_000E40_* position it decodes, and
// every masked lane pinned to zero.  A gauge reads the bit its mask selects, so
// a mask that moves silently renames a lane's block and reports one engine's
// activity under another's label; a masked lane that gains a bit renders a
// number the exposure evidence does not support.  Comment and structure edits
// in initbits pass through this check rather than through a reader.
static void check_rs480_masks(void) {
	initbits(RS480);

	CHECK(bits.gui == (1U << 31));   // GUI_ACTIVE
	CHECK(bits.cf == (1U << 14));    // CF_PIPE_BUSY
	CHECK(bits.ee == (1U << 15));    // ENG_EV_BUSY
	CHECK(bits.cp == (1U << 16));    // CP_CMDSTRM_BUSY
	CHECK(bits.e2 == (1U << 17));    // E2_BUSY
	CHECK(bits.vgt == (1U << 20));   // VAP_BUSY
	CHECK(bits.pa == (1U << 26));    // GA_BUSY

	// RB2D_BUSY(18) stays in the union because the decode names the field;
	// CBA2D_BUSY(27) is what asserts on RS482.
	CHECK(bits.rb2d == ((1U << 18) | (1U << 27)));

	// RB3D_BUSY(19), RE_BUSY(21), and TAM_BUSY(22) back these three lanes and
	// stay clear under loads driving their blocks, so the lanes stay masked.
	CHECK(bits.cb == 0);
	CHECK(bits.sc == 0);
	CHECK(bits.ta == 0);

	// No RS400 field reports these blocks, so no bit can carry their lanes.
	CHECK(bits.db == 0);
	CHECK(bits.tc == 0);
	CHECK(bits.sx == 0);
	CHECK(bits.sh == 0);
	CHECK(bits.spi == 0);
	CHECK(bits.smx == 0);
	CHECK(bits.cr == 0);
	CHECK(bits.uvd == 0);
	CHECK(bits.vce0 == 0);

	// The R600 layout shares only GUI_ACTIVE with the R300 one, so a family
	// that reaches the R600 branch proves the RS480 masks stayed inside it.
	initbits(BONAIRE);
	CHECK(bits.gui == (1U << 31));
	CHECK(bits.cp == 0);
	CHECK(bits.e2 == 0);
	CHECK(bits.rb2d == 0);
	CHECK(bits.cf == 0);
	CHECK(bits.cb == (1U << 30));
	CHECK(bits.pa == (1U << 25));
}

int main(void) {
	struct radeon_device_identity identity;
	short bus = -1;
	uint32_t status = 0;

	initialize_pci_device(&selected_device, 0, 0x6650);
	configure_pci_iterator(true);
	grbm_window[(GRBM_STATUS - GRBM_MMAP_BASE) / sizeof(uint32_t)] =
		UINT32_C(0xdeadbeef);

	radeon_device_identity_init(&identity);
	init_pci("/dev/dri/renderD128", &bus, &identity, 1);

	CHECK(bus == 1);
	CHECK(identity.pci_address_valid);
	CHECK(identity.domain == 0 && identity.bus == 1 &&
		identity.device == 0 && identity.function == 0);
	CHECK(identity.vendor_id == VENDOR_AMD);
	CHECK(identity.device_id == 0x6650);
	CHECK(identity.family == BONAIRE);
	CHECK(!strcmp(identity.drm_driver, "amdgpu"));
	CHECK(identity.status_source == RADEON_STATUS_PCI_RESOURCE_GRBM);
	CHECK(identity.status_register == GRBM_STATUS);
	CHECK(identity.resource_index == 5);
	CHECK(identity.resource_size == 0x10000);
	CHECK(active_drm_fd == -1);
	CHECK(getgrbm != getuint32_null);
	CHECK(getgrbm(&status) == 0);
	CHECK(status == UINT32_C(0xdeadbeef));
	CHECK(explicit_open_count == 1);
	CHECK(resource_open_count == 1);
	CHECK(close_count == 2);
	CHECK(drm_open_count == 0);
	CHECK(authenticate_count == 0);
	CHECK(init_radeon_count == 0);
	CHECK(privilege_raise_count == 1);
	CHECK(privilege_drop_count == 1);

	cleanup();
	CHECK(unmap_count == 2);

	// A matching Radeon DRM node initializes optional counters.
	// The forced direct path retains the validated BAR for GRBM_STATUS reads.
	drm_bus_open_succeeds = true;
	drm_driver_is_radeon = true;
	bus = -1;
	status = 0;
	radeon_device_identity_init(&identity);
	init_pci(NULL, &bus, &identity, 1);

	CHECK(bus == 1);
	CHECK(identity.pci_address_valid);
	CHECK(identity.domain == 0 && identity.bus == 1 &&
		identity.device == 0 && identity.function == 0);
	CHECK(identity.vendor_id == VENDOR_AMD);
	CHECK(identity.device_id == 0x6650);
	CHECK(identity.family == BONAIRE);
	CHECK(!strcmp(identity.drm_driver, "radeon"));
	CHECK(identity.status_source == RADEON_STATUS_PCI_RESOURCE_GRBM);
	CHECK(identity.status_register == GRBM_STATUS);
	CHECK(identity.resource_index == 5);
	CHECK(identity.resource_size == 0x10000);
	CHECK(active_drm_fd == 10);
	CHECK(getgrbm != getuint32_null);
	CHECK(getgrbm(&status) == 0);
	CHECK(status == UINT32_C(0xdeadbeef));
	CHECK(explicit_open_count == 1);
	CHECK(resource_open_count == 2);
	CHECK(close_count == 3);
	CHECK(drm_open_count == 1);
	CHECK(authenticate_count == 1);
	CHECK(init_radeon_count == 1);
	CHECK(privilege_raise_count == 2);
	CHECK(privilege_drop_count == 2);

	cleanup();
	CHECK(close_count == 4);
	CHECK(unmap_count == 4);

	configure_pci_iterator(false);
	radeon_device_identity_init(&identity);
	CHECK(find_pci(-1, &identity) == 1);
	CHECK(!identity.pci_address_valid);
	CHECK(identity.family == UNKNOWN_CHIP);
	CHECK(resource_open_count == 2);
	CHECK(privilege_raise_count == 2);
	CHECK(privilege_drop_count == 2);

	check_rs480_masks();

	printf("detect path: %u checks, %u failed\n", checks, failures);
	return failures ? 1 : 0;
}
#else
int main(void) {
	struct radeon_device_identity identity;
	unsigned int supported_opens_before_unknown_only;

	drm_driver_is_radeon = true;
	configure_drm_devices(true);
	radeon_device_identity_init(&identity);
	getgrbm = getsclk = getmclk = getuint32_null;
	getsrbm = getsrbm2 = getuint32_null;
	getvram = getgtt = getuint64_null;

	CHECK(find_drm(-1, &identity) == 0);
	CHECK(identity.pci_address_valid);
	CHECK(identity.domain == 0 && identity.bus == 1 &&
		identity.device == 0 && identity.function == 0);
	CHECK(identity.vendor_id == VENDOR_AMD);
	CHECK(identity.device_id == 0x6650);
	CHECK(identity.family == BONAIRE);
	CHECK(!strcmp(identity.drm_driver, "radeon"));
	CHECK(identity.status_source == RADEON_STATUS_RADEON_READ_REG);
	CHECK(active_drm_fd == 10);
	CHECK(unknown_drm_node_open_count == 0);
	CHECK(supported_drm_node_open_count == 1);
	cleanup();

	supported_opens_before_unknown_only = supported_drm_node_open_count;
	configure_drm_devices(false);
	radeon_device_identity_init(&identity);
	getgrbm = getsclk = getmclk = getuint32_null;
	getsrbm = getsrbm2 = getuint32_null;
	getvram = getgtt = getuint64_null;
	CHECK(find_drm(-1, &identity) == 1);
	CHECK(!identity.pci_address_valid);
	CHECK(identity.family == UNKNOWN_CHIP);
	CHECK(active_drm_fd == -1);
	CHECK(unknown_drm_node_open_count == 0);
	CHECK(supported_drm_node_open_count ==
		supported_opens_before_unknown_only);

	printf("detect DRM discovery: %u checks, %u failed\n", checks, failures);
	return failures ? 1 : 0;
}
#endif

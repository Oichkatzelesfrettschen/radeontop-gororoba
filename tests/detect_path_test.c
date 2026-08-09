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
#include <xf86drm.h>

static int test_open(const char *path, int flags, ...);
static int test_close(int fd);
static void *test_mmap(void *address, size_t length, int protection,
	int flags, int fd, off_t offset);
static int test_munmap(void *address, size_t length);

// The test compiles the real selection unit with only its operating-system
// boundaries replaced.  Omitting DRM_BUS_PCI selects the legacy bus-ID lane,
// which is the compatibility path used when drmGetDevice is unavailable.
#undef DRM_BUS_PCI
#undef HAS_DRMGETDEVICE
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
static unsigned int resource_open_count;
static unsigned int close_count;
static unsigned int drm_open_count;
static unsigned int authenticate_count;
static unsigned int init_radeon_count;
static unsigned int privilege_raise_count;
static unsigned int privilege_drop_count;
static unsigned int unmap_count;
static bool iterator_has_device;
static unsigned char iterator_token;
static uint32_t srbm_window[SRBM_MMAP_SIZE / sizeof(uint32_t)];
static uint32_t grbm_window[MMAP_SIZE / sizeof(uint32_t)];
static struct pci_device selected_device;

#define CHECK(condition) do { \
	checks++; \
	if (!(condition)) { \
		fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
		failures++; \
	} \
} while (0)

static int test_open(const char *path, int flags, ...) {
	(void) flags;

	if (!strcmp(path, "/dev/dri/renderD128")) {
		explicit_open_count++;
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
	CHECK(fd == 10 || fd == 11);
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
	static char driver_name[] = "amdgpu";
	static drmVersion version = {
		.version_major = 3,
		.version_minor = 64,
		.version_patchlevel = 0,
		.name_len = 6,
		.name = driver_name
	};

	CHECK(fd == 10);
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
	(void) bus_id;
	drm_open_count++;
	return -1;
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
	CHECK(device == &selected_device);
	return 0;
}

struct pci_device_iterator *pci_id_match_iterator_create(
		const struct pci_id_match *match) {
	CHECK(match->vendor_id == VENDOR_AMD);
	iterator_has_device = true;
	return (struct pci_device_iterator *) &iterator_token;
}

struct pci_device *pci_device_next(struct pci_device_iterator *iterator) {
	CHECK(iterator == (struct pci_device_iterator *) &iterator_token);
	if (!iterator_has_device)
		return NULL;
	iterator_has_device = false;
	return &selected_device;
}

void pci_iterator_destroy(struct pci_device_iterator *iterator) {
	CHECK(iterator == (struct pci_device_iterator *) &iterator_token);
}

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

int main(void) {
	struct radeon_device_identity identity;
	short bus = -1;
	uint32_t status = 0;

	memset(&selected_device, 0, sizeof(selected_device));
	selected_device.domain_16 = 0;
	selected_device.bus = 1;
	selected_device.dev = 0;
	selected_device.func = 0;
	selected_device.vendor_id = VENDOR_AMD;
	selected_device.device_id = 0x6650;
	selected_device.device_class = 0x00030000;
	selected_device.regions[5].size = 0x10000;
	grbm_window[(GRBM_STATUS - GRBM_MMAP_BASE) / sizeof(uint32_t)] =
		UINT32_C(0xdeadbeef);

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

	printf("detect path: %u checks, %u failed\n", checks, failures);
	return failures ? 1 : 0;
}

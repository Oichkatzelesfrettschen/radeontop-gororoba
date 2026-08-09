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

#ifndef DEVICE_MODEL_H
#define DEVICE_MODEL_H

#include <stdbool.h>
#include <stdint.h>

// Linux r300d.h names R_000E40_RBBM_STATUS for R300-class engine state.
// R600 and later expose the corresponding engine state through GRBM_STATUS.
#define GRBM_STATUS UINT32_C(0x8010)
#define RBBM_STATUS UINT32_C(0x0e40)

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
	RADEON_FAMILY_COUNT
};

enum radeon_status_source {
	RADEON_STATUS_UNAVAILABLE,
	RADEON_STATUS_PCI_RESOURCE_RBBM,
	RADEON_STATUS_PCI_RESOURCE_GRBM,
	RADEON_STATUS_RADEON_READ_REG,
	RADEON_STATUS_AMDGPU_READ_MM_REGISTERS
};

enum {
	RADEON_PCI_RESOURCE_NONE = -1,
	RADEON_PCI_RESOURCE_COUNT = 6
};

struct radeon_mmio_layout {
	int resource_index;
	enum radeon_status_source status_source;
	uint32_t status_register;
};

struct radeon_device_identity {
	bool pci_address_valid;
	uint16_t domain;
	uint8_t bus;
	uint8_t device;
	uint8_t function;
	uint16_t vendor_id;
	uint16_t device_id;
	enum radeon_family family;

	enum radeon_status_source status_source;
	uint32_t status_register;
	int resource_index;
	uint64_t resource_size;

	char drm_driver[32];
	unsigned int drm_version_major;
	unsigned int drm_version_minor;
	unsigned int drm_version_patchlevel;
};

void radeon_device_identity_init(struct radeon_device_identity *identity);
bool radeon_mmio_layout_for_family(enum radeon_family family,
		struct radeon_mmio_layout *layout);
bool radeon_pci_address_matches(const struct radeon_device_identity *identity,
		uint16_t domain, uint8_t bus, uint8_t device, uint8_t function);
bool radeon_parse_pci_bus_id(const char *bus_id, uint16_t *domain,
		uint8_t *bus, uint8_t *device, uint8_t *function);
bool radeon_direct_status_path_required(bool force_direct,
		bool drm_status_reader_available);
const char *radeon_status_source_name(enum radeon_status_source source);
const char *radeon_status_register_name(uint32_t status_register);
bool radeon_clock_mhz_to_khz(uint32_t mhz, uint32_t *khz);

#endif

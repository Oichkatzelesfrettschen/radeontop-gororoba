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

#include "device_model.h"

#include <stdio.h>
#include <string.h>

static unsigned int checks;
static unsigned int failures;

static const enum radeon_family resource2_grbm_families[] = {
	R600, RV610, RV630, RV670, RV620, RV635, RS780, RS880, RV770, RV730,
	RV710, RV740, CEDAR, REDWOOD, JUNIPER, CYPRESS, HEMLOCK, PALM, SUMO,
	SUMO2, BARTS, TURKS, CAICOS, CAYMAN, ARUBA, TAHITI, PITCAIRN, VERDE,
	OLAND, HAINAN
};

static const enum radeon_family resource5_grbm_families[] = {
	BONAIRE, KABINI, MULLINS, KAVERI, HAWAII, TOPAZ, TONGA, FIJI, CARRIZO,
	STONEY, POLARIS11, POLARIS10, POLARIS12, VEGAM, VEGA10, VEGA12, VEGA20,
	RAVEN, ARCTURUS, NAVI10, NAVI14, RENOIR, NAVI12, SIENNA_CICHLID,
	VANGOGH, YELLOW_CARP, NAVY_FLOUNDER, DIMGREY_CAVEFISH, ALDEBARAN,
	CYAN_SKILLFISH, BEIGE_GOBY
};

enum {
	RS480_RBBM_FAMILY_COUNT = 1,
	UNKNOWN_CHIP_FAMILY_COUNT = 1,
	NON_GRBM_FAMILY_COUNT = RS480_RBBM_FAMILY_COUNT +
		UNKNOWN_CHIP_FAMILY_COUNT
};

_Static_assert(
	sizeof(resource2_grbm_families) / sizeof(resource2_grbm_families[0]) +
	sizeof(resource5_grbm_families) / sizeof(resource5_grbm_families[0]) +
	NON_GRBM_FAMILY_COUNT ==
	RADEON_FAMILY_COUNT,
	"the test denominator must classify every radeon family");

#define CHECK(condition) do { \
	checks++; \
	if (!(condition)) { \
		fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
		failures++; \
	} \
} while (0)

static void check_layout(enum radeon_family family, int resource_index,
		enum radeon_status_source source, uint32_t status_register) {
	struct radeon_mmio_layout layout;

	memset(&layout, 0xa5, sizeof(layout));
	CHECK(radeon_mmio_layout_for_family(family, &layout));
	CHECK(layout.resource_index == resource_index);
	CHECK(layout.status_source == source);
	CHECK(layout.status_register == status_register);
	CHECK(radeon_mmio_layout_for_family(family, NULL));
}

static void check_rejected_layout(enum radeon_family family) {
	struct radeon_mmio_layout layout;

	memset(&layout, 0xa5, sizeof(layout));
	CHECK(!radeon_mmio_layout_for_family(family, &layout));
	CHECK(layout.resource_index == RADEON_PCI_RESOURCE_NONE);
	CHECK(layout.status_source == RADEON_STATUS_UNAVAILABLE);
	CHECK(layout.status_register == 0);
}

int main(void) {
	struct radeon_device_identity identity;
	uint32_t khz = 0;
	uint16_t domain = 0;
	uint8_t bus = 0, device = 0, function = 0;

	check_rejected_layout((enum radeon_family) -1);
	check_rejected_layout(UNKNOWN_CHIP);
	check_rejected_layout(RADEON_FAMILY_COUNT);
	check_rejected_layout((enum radeon_family) (RADEON_FAMILY_COUNT + 1));

	check_layout(RS480, 2, RADEON_STATUS_PCI_RESOURCE_RBBM, RBBM_STATUS);
	for (size_t index = 0;
		index < sizeof(resource2_grbm_families) /
			sizeof(resource2_grbm_families[0]); index++)
		check_layout(resource2_grbm_families[index], 2,
			RADEON_STATUS_PCI_RESOURCE_GRBM, GRBM_STATUS);
	for (size_t index = 0;
		index < sizeof(resource5_grbm_families) /
			sizeof(resource5_grbm_families[0]); index++)
		check_layout(resource5_grbm_families[index], 5,
			RADEON_STATUS_PCI_RESOURCE_GRBM, GRBM_STATUS);

	CHECK(!strcmp(radeon_status_source_name(RADEON_STATUS_UNAVAILABLE),
		"unavailable"));
	CHECK(!strcmp(radeon_status_source_name(RADEON_STATUS_PCI_RESOURCE_RBBM),
		"pci-resource-rbbm-status"));
	CHECK(!strcmp(radeon_status_source_name(RADEON_STATUS_PCI_RESOURCE_GRBM),
		"pci-resource-grbm-status"));
	CHECK(!strcmp(radeon_status_source_name(RADEON_STATUS_RADEON_READ_REG),
		"radeon-read-reg-ioctl"));
	CHECK(!strcmp(radeon_status_source_name(RADEON_STATUS_AMDGPU_READ_MM_REGISTERS),
		"amdgpu-read-mm-registers"));
	CHECK(!strcmp(radeon_status_source_name((enum radeon_status_source) 99),
		"unavailable"));
	CHECK(!strcmp(radeon_status_register_name(RBBM_STATUS), "RBBM_STATUS"));
	CHECK(!strcmp(radeon_status_register_name(GRBM_STATUS), "GRBM_STATUS"));
	CHECK(!strcmp(radeon_status_register_name(0), "unavailable"));

	CHECK(radeon_clock_mhz_to_khz(0, &khz));
	CHECK(khz == 0);
	CHECK(radeon_clock_mhz_to_khz(350, &khz));
	CHECK(khz == 350000);
	CHECK(radeon_clock_mhz_to_khz(UINT32_MAX / 1000U, &khz));
	CHECK(khz == (UINT32_MAX / 1000U) * 1000U);
	khz = 7;
	CHECK(!radeon_clock_mhz_to_khz(UINT32_MAX / 1000U + 1U, &khz));
	CHECK(khz == 7);
	CHECK(!radeon_clock_mhz_to_khz(1, NULL));

	memset(&identity, 0xa5, sizeof(identity));
	radeon_device_identity_init(&identity);
	CHECK(!identity.pci_address_valid);
	CHECK(identity.family == UNKNOWN_CHIP);
	CHECK(identity.status_source == RADEON_STATUS_UNAVAILABLE);
	CHECK(identity.status_register == 0);
	CHECK(identity.resource_index == RADEON_PCI_RESOURCE_NONE);
	CHECK(identity.resource_size == 0);
	CHECK(identity.drm_driver[0] == '\0');
	CHECK(!radeon_pci_address_matches(&identity, 0, 1, 5, 0));
	CHECK(!radeon_pci_address_matches(NULL, 0, 1, 5, 0));

	identity.pci_address_valid = true;
	identity.domain = 0;
	identity.bus = 1;
	identity.device = 5;
	identity.function = 0;
	CHECK(radeon_pci_address_matches(&identity, 0, 1, 5, 0));
	CHECK(!radeon_pci_address_matches(&identity, 1, 1, 5, 0));
	CHECK(!radeon_pci_address_matches(&identity, 0, 2, 5, 0));
	CHECK(!radeon_pci_address_matches(&identity, 0, 1, 6, 0));
	CHECK(!radeon_pci_address_matches(&identity, 0, 1, 5, 1));

	CHECK(radeon_parse_pci_bus_id("pci:0000:01:05.0", &domain, &bus,
		&device, &function));
	CHECK(domain == 0 && bus == 1 && device == 5 && function == 0);
	CHECK(radeon_parse_pci_bus_id("pci:ffff:ff:1f.7", &domain, &bus,
		&device, &function));
	CHECK(domain == UINT16_MAX && bus == UINT8_MAX && device == 0x1f &&
		function == 7);
	CHECK(!radeon_parse_pci_bus_id("0000:01:05.0", &domain, &bus,
		&device, &function));
	CHECK(!radeon_parse_pci_bus_id("pci:0000:01:20.0", &domain, &bus,
		&device, &function));
	CHECK(!radeon_parse_pci_bus_id("pci:0000:01:05.8", &domain, &bus,
		&device, &function));
	CHECK(!radeon_parse_pci_bus_id("pci:0000:01:05.0x", &domain, &bus,
		&device, &function));
	CHECK(!radeon_parse_pci_bus_id(NULL, &domain, &bus, &device, &function));
	CHECK(!radeon_parse_pci_bus_id("pci:0000:01:05.0", NULL, &bus,
		&device, &function));

	CHECK(!radeon_direct_status_path_required(false, true));
	CHECK(radeon_direct_status_path_required(false, false));
	CHECK(radeon_direct_status_path_required(true, true));
	CHECK(radeon_direct_status_path_required(true, false));

	printf("device model: %u checks, %u failed\n", checks, failures);
	return failures ? 1 : 0;
}

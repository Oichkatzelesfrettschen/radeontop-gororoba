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

#include <limits.h>
#include <string.h>

void radeon_device_identity_init(struct radeon_device_identity *identity) {
	memset(identity, 0, sizeof(*identity));
	identity->family = UNKNOWN_CHIP;
	identity->status_source = RADEON_STATUS_UNAVAILABLE;
	identity->resource_index = -1;
}

bool radeon_mmio_layout_for_family(enum radeon_family family,
		struct radeon_mmio_layout *layout) {
	struct radeon_mmio_layout selected;

	memset(&selected, 0, sizeof(selected));

	// Every admitted family appears as a case.  An enum range silently grants a
	// newly inserted family an inherited BAR layout before its PCI aperture has
	// been reviewed, which turns enum maintenance into privileged MMIO policy.
	switch (family) {
		case RS480:
			selected.resource_index = 2;
			selected.status_source = RADEON_STATUS_PCI_RESOURCE_RBBM;
			selected.status_register = RBBM_STATUS;
			break;

		case R600:
		case RV610:
		case RV630:
		case RV670:
		case RV620:
		case RV635:
		case RS780:
		case RS880:
		case RV770:
		case RV730:
		case RV710:
		case RV740:
		case CEDAR:
		case REDWOOD:
		case JUNIPER:
		case CYPRESS:
		case HEMLOCK:
		case PALM:
		case SUMO:
		case SUMO2:
		case BARTS:
		case TURKS:
		case CAICOS:
		case CAYMAN:
		case ARUBA:
		case TAHITI:
		case PITCAIRN:
		case VERDE:
		case OLAND:
		case HAINAN:
			selected.resource_index = 2;
			selected.status_source = RADEON_STATUS_PCI_RESOURCE_GRBM;
			selected.status_register = GRBM_STATUS;
			break;

		case BONAIRE:
		case KABINI:
		case MULLINS:
		case KAVERI:
		case HAWAII:
		case TOPAZ:
		case TONGA:
		case FIJI:
		case CARRIZO:
		case STONEY:
		case POLARIS11:
		case POLARIS10:
		case POLARIS12:
		case VEGAM:
		case VEGA10:
		case VEGA12:
		case VEGA20:
		case RAVEN:
		case ARCTURUS:
		case NAVI10:
		case NAVI14:
		case RENOIR:
		case NAVI12:
		case SIENNA_CICHLID:
		case VANGOGH:
		case YELLOW_CARP:
		case NAVY_FLOUNDER:
		case DIMGREY_CAVEFISH:
		case ALDEBARAN:
		case CYAN_SKILLFISH:
		case BEIGE_GOBY:
			selected.resource_index = 5;
			selected.status_source = RADEON_STATUS_PCI_RESOURCE_GRBM;
			selected.status_register = GRBM_STATUS;
			break;

		case UNKNOWN_CHIP:
		case RADEON_FAMILY_COUNT:
		default:
			if (layout)
				*layout = selected;
			return false;
	}

	if (layout)
		*layout = selected;
	return true;
}

bool radeon_pci_address_matches(const struct radeon_device_identity *identity,
		uint16_t domain, uint8_t bus, uint8_t device, uint8_t function) {
	return identity && identity->pci_address_valid &&
		identity->domain == domain && identity->bus == bus &&
		identity->device == device && identity->function == function;
}

static bool parse_hex_field(const char *text, size_t length,
		unsigned int *value) {
	unsigned int parsed = 0;

	for (size_t index = 0; index < length; index++) {
		unsigned int digit;

		if (text[index] >= '0' && text[index] <= '9')
			digit = (unsigned int) (text[index] - '0');
		else if (text[index] >= 'a' && text[index] <= 'f')
			digit = (unsigned int) (text[index] - 'a' + 10);
		else if (text[index] >= 'A' && text[index] <= 'F')
			digit = (unsigned int) (text[index] - 'A' + 10);
		else
			return false;
		parsed = parsed * 16U + digit;
	}

	*value = parsed;
	return true;
}

bool radeon_parse_pci_bus_id(const char *bus_id, uint16_t *domain,
		uint8_t *bus, uint8_t *device, uint8_t *function) {
	unsigned int parsed_domain, parsed_bus, parsed_device, parsed_function;

	if (!bus_id || !domain || !bus || !device || !function ||
		strlen(bus_id) != 16 || memcmp(bus_id, "pci:", 4) ||
		bus_id[8] != ':' || bus_id[11] != ':' || bus_id[14] != '.' ||
		!parse_hex_field(bus_id + 4, 4, &parsed_domain) ||
		!parse_hex_field(bus_id + 9, 2, &parsed_bus) ||
		!parse_hex_field(bus_id + 12, 2, &parsed_device) ||
		!parse_hex_field(bus_id + 15, 1, &parsed_function) ||
		parsed_device > 0x1f || parsed_function > 7)
		return false;

	*domain = (uint16_t) parsed_domain;
	*bus = (uint8_t) parsed_bus;
	*device = (uint8_t) parsed_device;
	*function = (uint8_t) parsed_function;
	return true;
}

bool radeon_direct_status_path_required(bool force_direct,
		bool drm_status_reader_available) {
	return force_direct || !drm_status_reader_available;
}

const char *radeon_status_source_name(enum radeon_status_source source) {
	switch (source) {
		case RADEON_STATUS_PCI_RESOURCE_RBBM:
			return "pci-resource-rbbm-status";
		case RADEON_STATUS_PCI_RESOURCE_GRBM:
			return "pci-resource-grbm-status";
		case RADEON_STATUS_RADEON_READ_REG:
			return "radeon-read-reg-ioctl";
		case RADEON_STATUS_AMDGPU_READ_MM_REGISTERS:
			return "amdgpu-read-mm-registers";
		case RADEON_STATUS_UNAVAILABLE:
		default:
			return "unavailable";
	}
}

const char *radeon_status_register_name(uint32_t status_register) {
	switch (status_register) {
		case RBBM_STATUS:
			return "RBBM_STATUS";
		case GRBM_STATUS:
			return "GRBM_STATUS";
		default:
			return "unavailable";
	}
}

bool radeon_clock_mhz_to_khz(uint32_t mhz, uint32_t *khz) {
	if (!khz || mhz > UINT32_MAX / 1000U)
		return false;

	*khz = mhz * 1000U;
	return true;
}

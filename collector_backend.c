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

// collector_backend_from_device binds the family-selected device readers and
// RBBM/GRBM masks to the collector's injectable interfaces.  classify interprets
// each reader's return convention: direct MMIO readers return 0 for every
// completed load, while drmCommandWriteRead and the libdrm_amdgpu queries return
// -errno and carry terminal device-removal failures.

#include "radeontop.h"

#include <errno.h>
#include <string.h>

// ENODEV and ENXIO follow device removal, so a retry cannot succeed and the
// collector stops rather than accumulating failures forever.  Every other errno
// is transient: an isolated ioctl refusal is counted and the window continues.
static int classify(int rc) {
	int err;

	if (rc == 0)
		return COLLECTOR_READ_OK;

	err = rc < 0 ? -rc : errno;

	if (err == ENODEV || err == ENXIO)
		return COLLECTOR_READ_FATAL;

	return COLLECTOR_READ_TRANSIENT;
}

static int read_status(void *context, uint32_t *value) {
	(void) context;
	return classify(getgrbm(value));
}

static int read_uvd_status(void *context, uint32_t *value) {
	(void) context;
	return classify(getsrbm(value));
}

static int read_vce_status(void *context, uint32_t *value) {
	(void) context;
	return classify(getsrbm2(value));
}

static int read_sclk(void *context, uint32_t *value) {
	(void) context;
	return classify(getsclk(value));
}

static int read_mclk(void *context, uint32_t *value) {
	(void) context;
	return classify(getmclk(value));
}

static int read_vram(void *context, uint64_t *value) {
	(void) context;
	return classify(getvram(value));
}

static int read_gtt(void *context, uint64_t *value) {
	(void) context;
	return classify(getgtt(value));
}

struct collector_backend collector_backend_from_device(void) {
	struct collector_backend backend;

	memset(&backend, 0, sizeof(backend));

	backend.context = NULL;
	backend.read_status = read_status;
	backend.read_uvd_status = read_uvd_status;
	backend.read_vce_status = read_vce_status;
	backend.read_sclk = read_sclk;
	backend.read_mclk = read_mclk;
	backend.read_vram = read_vram;
	backend.read_gtt = read_gtt;

	// A capability needs both a reader the family bound and, for the two
	// separately-read engine lanes, a mask that names a bit in that
	// register.  A part with the reader and no mask has nothing to count.
	if (getgrbm != getuint32_null)
		backend.capabilities |= COLLECTOR_CAP_STATUS;

	if (bits.uvd && getsrbm != getuint32_null)
		backend.capabilities |= COLLECTOR_CAP_UVD;

	if (bits.vce0 && getsrbm2 != getuint32_null)
		backend.capabilities |= COLLECTOR_CAP_VCE;

	if (getsclk != getuint32_null)
		backend.capabilities |= COLLECTOR_CAP_SCLK;

	if (getmclk != getuint32_null)
		backend.capabilities |= COLLECTOR_CAP_MCLK;

	if (getvram != getuint64_null)
		backend.capabilities |= COLLECTOR_CAP_VRAM;

	if (getgtt != getuint64_null)
		backend.capabilities |= COLLECTOR_CAP_GTT;

	return backend;
}

struct engine_masks collector_masks_from_bits(void) {
	struct engine_masks masks;

	memset(&masks, 0, sizeof(masks));

	masks.lane[COLLECTOR_LANE_EE] = bits.ee;
	masks.lane[COLLECTOR_LANE_VGT] = bits.vgt;
	masks.lane[COLLECTOR_LANE_GUI] = bits.gui;
	masks.lane[COLLECTOR_LANE_TA] = bits.ta;
	masks.lane[COLLECTOR_LANE_TC] = bits.tc;
	masks.lane[COLLECTOR_LANE_SX] = bits.sx;
	masks.lane[COLLECTOR_LANE_SH] = bits.sh;
	masks.lane[COLLECTOR_LANE_SPI] = bits.spi;
	masks.lane[COLLECTOR_LANE_SMX] = bits.smx;
	masks.lane[COLLECTOR_LANE_SC] = bits.sc;
	masks.lane[COLLECTOR_LANE_PA] = bits.pa;
	masks.lane[COLLECTOR_LANE_DB] = bits.db;
	masks.lane[COLLECTOR_LANE_CB] = bits.cb;
	masks.lane[COLLECTOR_LANE_CR] = bits.cr;
	masks.lane[COLLECTOR_LANE_CP] = bits.cp;
	masks.lane[COLLECTOR_LANE_E2] = bits.e2;
	masks.lane[COLLECTOR_LANE_RB2D] = bits.rb2d;
	masks.lane[COLLECTOR_LANE_CF] = bits.cf;
	masks.lane[COLLECTOR_LANE_UVD] = bits.uvd;
	masks.lane[COLLECTOR_LANE_VCE0] = bits.vce0;

	return masks;
}

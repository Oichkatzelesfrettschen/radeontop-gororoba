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
#include <xf86drm.h>
#include <libdrm/amdgpu_drm.h>
#include <libdrm/amdgpu.h>

static amdgpu_device_handle amdgpu_dev;

static int getgrbm_amdgpu(uint32_t *out) {
	return amdgpu_read_mm_registers(amdgpu_dev, GRBM_STATUS / 4, 1,
					0xffffffff, 0, out);
}

static int getsrbm_amdgpu(uint32_t *out) {
	return amdgpu_read_mm_registers(amdgpu_dev, SRBM_STATUS / 4, 1,
					0xffffffff, 0, out);
}

static int getsrbm2_amdgpu(uint32_t *out) {
	return amdgpu_read_mm_registers(amdgpu_dev, SRBM_STATUS2 / 4, 1,
					0xffffffff, 0, out);
}

static int getvram_amdgpu(uint64_t *out) {
	return amdgpu_query_info(amdgpu_dev, AMDGPU_INFO_VRAM_USAGE,
				sizeof(uint64_t), out);
}

static int getgtt_amdgpu(uint64_t *out) {
	return amdgpu_query_info(amdgpu_dev, AMDGPU_INFO_GTT_USAGE,
				sizeof(uint64_t), out);
}

#ifdef HAS_AMDGPU_QUERY_SENSOR_INFO
static int getclock_amdgpu(unsigned int sensor, uint32_t *out) {
	uint32_t mhz;
	int result;

	result = amdgpu_query_sensor_info(amdgpu_dev, sensor, sizeof(mhz), &mhz);
	if (result)
		return result;

	// Linux amdgpu_kms.c returns the GFX_SCLK and GFX_MCLK sensors in
	// megahertz, while AMDGPU_INFO_DEV_INFO returns maximum clocks in
	// kilohertz.  The collector keeps both sides in kilohertz.
	if (!radeon_clock_mhz_to_khz(mhz, out))
		return -ERANGE;

	return 0;
}

static int getsclk_amdgpu(uint32_t *out) {
	return getclock_amdgpu(AMDGPU_INFO_SENSOR_GFX_SCLK, out);
}

static int getmclk_amdgpu(uint32_t *out) {
	return getclock_amdgpu(AMDGPU_INFO_SENSOR_GFX_MCLK, out);
}
#endif

#define DRM_ATLEAST_VERSION(maj, min) \
	(drm_major > maj || (drm_major == maj && drm_minor >= min))

void init_amdgpu(int fd) {
	uint32_t drm_major, drm_minor, out32;
	uint64_t out64;
	int ret;

	if (amdgpu_device_initialize(fd, &drm_major, &drm_minor, &amdgpu_dev))
		return;

	if (!(ret = getgrbm_amdgpu(&out32))) {
		getgrbm = getgrbm_amdgpu;
		getsrbm = getsrbm_amdgpu;
		getsrbm2 = getsrbm2_amdgpu;
	} else
		drmError(ret, _("Failed to get GPU usage"));

#ifdef HAS_AMDGPU_QUERY_SENSOR_INFO
	if (DRM_ATLEAST_VERSION(3, 11)) {
		struct amdgpu_gpu_info gpu;

		memset(&gpu, 0, sizeof(gpu));
		ret = amdgpu_query_gpu_info(amdgpu_dev, &gpu);
		if (ret) {
			drmError(ret, _("Failed to get GPU clock limits"));
		} else if (gpu.max_engine_clk > UINT32_MAX ||
			gpu.max_memory_clk > UINT32_MAX) {
			drmError(-ERANGE, _("GPU clock limits exceed the supported range"));
		} else {
			sclk_max = (uint32_t) gpu.max_engine_clk;
			mclk_max = (uint32_t) gpu.max_memory_clk;

			if (!(ret = getsclk_amdgpu(&out32)))
				getsclk = getsclk_amdgpu;
			else
				drmError(ret, _("Failed to get shader clock"));

			if (!(ret = getmclk_amdgpu(&out32)))
				getmclk = getmclk_amdgpu;
			else if (!(gpu.ids_flags & AMDGPU_IDS_FLAGS_FUSION))
				// Memory-clock reporting is unavailable on APUs.
				drmError(ret, _("Failed to get memory clock"));
		}
	} else
		fprintf(stderr, _("Clock frequency reporting is disabled (amdgpu kernel driver 3.11.0 required)\n"));
#else
	fprintf(stderr, _("Clock frequency reporting is not compiled in (libdrm 2.4.79 required)\n"));
#endif

	struct drm_amdgpu_info_vram_gtt vram_gtt;

	if ((ret = amdgpu_query_info(amdgpu_dev, AMDGPU_INFO_VRAM_GTT,
				sizeof(vram_gtt), &vram_gtt))) {
		drmError(ret, _("Failed to get VRAM size"));
		return;
	}

	vramsize = vram_gtt.vram_size;
	gttsize = vram_gtt.gtt_size;

	if (!(ret = getvram_amdgpu(&out64)))
		getvram = getvram_amdgpu;
	else
		drmError(ret, _("Failed to get VRAM usage"));

	if (!(ret = getgtt_amdgpu(&out64)))
		getgtt = getgtt_amdgpu;
	else
		drmError(ret, _("Failed to get GTT usage"));
}

void cleanup_amdgpu(void) {
	if (amdgpu_dev)
		amdgpu_device_deinitialize(amdgpu_dev);
}

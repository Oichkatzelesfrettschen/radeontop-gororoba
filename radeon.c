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
#include <radeon_drm.h>
#include <xf86drm.h>

// It's safe to make these queries on older kernels.
#ifndef RADEON_INFO_VRAM_USAGE
#define RADEON_INFO_VRAM_USAGE 0x1e
#endif
#ifndef RADEON_INFO_READ_REG
#define RADEON_INFO_READ_REG 0x24
#endif
#ifndef RADEON_INFO_CURRENT_GPU_SCLK
#define RADEON_INFO_CURRENT_GPU_SCLK 0x22
#endif
#ifndef RADEON_INFO_CURRENT_GPU_MCLK
#define RADEON_INFO_CURRENT_GPU_MCLK 0x23
#endif

static int drm_fd;

static int radeon_get_drm_value(int fd, unsigned request, uint32_t *out) {
	struct drm_radeon_info info;

	memset(&info, 0, sizeof(info));
	info.value = (unsigned long) out;
	info.request = request;

	return drmCommandWriteRead(fd, DRM_RADEON_INFO, &info, sizeof(info));
}

#ifdef RADEON_INFO_READ_REG
static int getgrbm_radeon(uint32_t *out) {
	// RADEON_INFO_READ_REG is gated per-asic by get_allowed_info_register.  For
	// the whole pre-R600 family (r100..rs480 in radeon_asic.c) that callback is
	// radeon_invalid_get_allowed_info_register, which returns -EINVAL for every
	// register -- so the libdrm path binds only on R600+, where engine-busy is
	// GRBM_STATUS.  R300-class engine-busy (RBBM_STATUS) is read through the PCI
	// BAR path (getgrbm_pci_r300); getgrbm_radeon never handles it.
	*out = GRBM_STATUS;
	return radeon_get_drm_value(drm_fd, RADEON_INFO_READ_REG, out);
}

static int getsrbm_radeon(uint32_t *out) {
	*out = SRBM_STATUS;
	return radeon_get_drm_value(drm_fd, RADEON_INFO_READ_REG, out);
}

static int getsrbm2_radeon(uint32_t *out) {
	*out = SRBM_STATUS2;
	return radeon_get_drm_value(drm_fd, RADEON_INFO_READ_REG, out);
}

static int getclock_radeon(unsigned request, uint32_t *out) {
	uint32_t mhz;
	int result;

	result = radeon_get_drm_value(drm_fd, request, &mhz);
	if (result)
		return result;

	// The radeon kernel ABI returns CURRENT_GPU_SCLK and CURRENT_GPU_MCLK in
	// megahertz.  The collector and the maximum-clock values use kilohertz.
	if (!radeon_clock_mhz_to_khz(mhz, out))
		return -ERANGE;

	return 0;
}

static int getsclk_radeon(uint32_t *out) {
	return getclock_radeon(RADEON_INFO_CURRENT_GPU_SCLK, out);
}

static int getmclk_radeon(uint32_t *out) {
	return getclock_radeon(RADEON_INFO_CURRENT_GPU_MCLK, out);
}
#endif

#ifdef RADEON_INFO_VRAM_USAGE
static int getvram_radeon(uint64_t *out) {
	return radeon_get_drm_value(drm_fd, RADEON_INFO_VRAM_USAGE,
				(uint32_t *) out);
}

static int getgtt_radeon(uint64_t *out) {
	return radeon_get_drm_value(drm_fd, RADEON_INFO_GTT_USAGE,
				(uint32_t *) out);
}
#endif

#define DRM_ATLEAST_VERSION(maj, min) \
	(drm_major > maj || (drm_major == maj && drm_minor >= min))

void init_radeon(int fd, int drm_major, int drm_minor, int family) {
	int ret;
	uint32_t out32 __attribute__((unused));
	uint64_t out64 __attribute__((unused));

	drm_fd = fd;

#ifdef RADEON_INFO_READ_REG
	if (DRM_ATLEAST_VERSION(2, 42)) {
		// Pre-R600 families reject RADEON_INFO_READ_REG for every
		// register by design (radeon_invalid_get_allowed_info_register),
		// so probing would only print a misleading EINVAL error; the
		// R300-class engine-busy read binds through the PCI BAR path
		// instead.  An unknown family keeps the probe.
		if (family != UNKNOWN_CHIP && family < R600) {
			;
		} else if (!(ret = getgrbm_radeon(&out32))) {
			getgrbm = getgrbm_radeon;
			getsrbm = getsrbm_radeon;
			getsrbm2 = getsrbm2_radeon;
		} else
			drmError(ret, _("Failed to get GPU usage"));

		if ((ret = radeon_get_drm_value(drm_fd, RADEON_INFO_MAX_SCLK,
						&sclk_max)))
			drmError(ret, _("Failed to get maximum shader clock"));

		if (!(ret = getsclk_radeon(&out32)))
			getsclk = getsclk_radeon;
		else
			drmError(ret, _("Failed to get shader clock"));

		if (!(ret = getmclk_radeon(&out32))) {
			getmclk = getmclk_radeon;
			// The radeon ABI has no MAX_MCLK query.  RS480 has a fixed
			// memory clock, so its current value is also its nominal maximum.
			// Dynamic-clock R600+ parts retain the absolute kHz sample while
			// the unknown maximum stays zero and the percentage stays hidden.
			if (family == RS480)
				mclk_max = out32;
		} else
			drmError(ret, _("Failed to get memory clock"));
	} else
		fprintf(stderr, _("GPU usage reporting via libdrm is disabled (radeon kernel driver 2.42.0 required), attempting memory path\n"));
#else
	fprintf(stderr, _("GPU usage reporting via libdrm is not compiled in (libdrm 2.4.71 required), attempting memory path\n"));
#endif

#ifdef RADEON_INFO_VRAM_USAGE
	if (DRM_ATLEAST_VERSION(2, 39)) {
		struct drm_radeon_gem_info gem;

		if ((ret = drmCommandWriteRead(fd, DRM_RADEON_GEM_INFO,
					 &gem, sizeof(gem)))) {
			drmError(ret, _("Failed to get VRAM size"));
			return;
		}

		vramsize = gem.vram_size;
		gttsize = gem.gart_size;

		if (!(ret = getvram_radeon(&out64)))
			getvram = getvram_radeon;
		else
			drmError(ret, _("Failed to get VRAM usage"));

		if (!(ret = getgtt_radeon(&out64)))
			getgtt = getgtt_radeon;
		else
			drmError(ret, _("Failed to get GTT usage"));
	} else
		fprintf(stderr, _("Memory usage reporting is disabled (radeon kernel driver 2.39.0 required)\n"));
#else
	fprintf(stderr, _("Memory usage reporting is not compiled in (libdrm 2.4.53 required)\n"));
#endif
}

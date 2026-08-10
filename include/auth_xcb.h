/*
	Copyright (C) 2016 Peter Wu <peter@lekensteyn.nl>

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

#ifndef AUTH_XCB_H
#define AUTH_XCB_H

#include <xf86drm.h>

#define RADEONTOP_XCB_AUTH_SYMBOL "authenticate_drm_xcb"

void authenticate_drm_xcb(drm_magic_t magic);

#endif

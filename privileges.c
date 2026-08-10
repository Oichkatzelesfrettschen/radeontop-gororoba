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

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "privileges.h"

#include <unistd.h>

int privileges_drop_effective(void) {
	const uid_t real_uid = getuid();

	if (seteuid(real_uid))
		return -1;

	return geteuid() == real_uid ? 0 : -1;
}

int privileges_raise_effective(void) {
	if (seteuid(0))
		return -1;

	return geteuid() == 0 ? 0 : -1;
}

int privileges_drop_permanently(void) {
	const uid_t real_uid = getuid();
	uid_t resulting_real, resulting_effective, resulting_saved;

	if (setresuid(real_uid, real_uid, real_uid))
		return -1;
	if (getresuid(&resulting_real, &resulting_effective, &resulting_saved))
		return -1;

	return resulting_real == real_uid && resulting_effective == real_uid &&
		resulting_saved == real_uid ? 0 : -1;
}

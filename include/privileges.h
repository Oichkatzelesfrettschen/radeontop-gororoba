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

#ifndef PRIVILEGES_H
#define PRIVILEGES_H

// The effective drop selects the invoking real UID and preserves the saved
// set-user-ID.  The raise selects UID 0 and therefore requires a saved
// set-user-ID of 0.  Each transition returns zero only after the effective UID
// matches its target; -1 leaves an unverified UID state that requires fatal
// handling before any privileged resource operation.
int privileges_drop_effective(void);
int privileges_raise_effective(void);

// The permanent transition returns zero only after the real, effective, and
// saved UIDs all match the invoking real UID.  It removes any distinct saved
// identity, and no raise operation follows a successful return.
int privileges_drop_permanently(void);

#endif

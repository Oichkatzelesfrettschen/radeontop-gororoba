#!/bin/sh

# Copyright (C) 2012 Lauri Kasanen
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, version 3 of the License.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.

set -eu

if [ "${1:-}" != "--self-test" ] || [ "$#" -gt 2 ]; then
	echo "usage: $0 --self-test [ELF_OBJECT]" >&2
	exit 2
fi

scratch=$(mktemp -d "${TMPDIR:-/tmp}/radeontop-xcb-abi.XXXXXX")
trap 'rm -rf "$scratch"' 0
trap 'exit 129' 1
trap 'exit 130' 2
trap 'exit 143' 15

symbol_table_exports_helper() {
	symbol_table=$1

	awk '
		function helper_name(name) {
			return name == "authenticate_drm_xcb" ||
				name ~ /^authenticate_drm_xcb@@[^@]+$/
		}
		helper_name($8) {
			rows++
			if ($4 == "FUNC" && ($5 == "GLOBAL" || $5 == "WEAK") &&
					$7 != "UND")
				valid++
		}
		END { exit rows == 1 && valid == 1 ? 0 : 1 }
	' "$symbol_table"
}

known_good="$scratch/known-good.txt"
known_good_versioned="$scratch/known-good-versioned.txt"
known_bad_undefined="$scratch/known-bad-undefined.txt"
known_bad_object="$scratch/known-bad-object.txt"
known_bad_local="$scratch/known-bad-local.txt"
known_bad_nondefault="$scratch/known-bad-nondefault.txt"
known_bad_duplicate="$scratch/known-bad-duplicate.txt"

printf '%s\n' \
	'1: 0000000000001000 42 FUNC GLOBAL DEFAULT 12 authenticate_drm_xcb' \
	> "$known_good"
printf '%s\n' \
	'1: 0000000000001000 42 FUNC WEAK DEFAULT 12 authenticate_drm_xcb@@RADEONTOP_1' \
	> "$known_good_versioned"
printf '%s\n' \
	'1: 0000000000000000 0 FUNC GLOBAL DEFAULT UND authenticate_drm_xcb' \
	> "$known_bad_undefined"
printf '%s\n' \
	'1: 0000000000001000 8 OBJECT GLOBAL DEFAULT 12 authenticate_drm_xcb' \
	> "$known_bad_object"
printf '%s\n' \
	'1: 0000000000001000 42 FUNC LOCAL DEFAULT 12 authenticate_drm_xcb' \
	> "$known_bad_local"
printf '%s\n' \
	'1: 0000000000001000 42 FUNC GLOBAL DEFAULT 12 authenticate_drm_xcb@RADEONTOP_1' \
	> "$known_bad_nondefault"
{
	printf '%s\n' \
		'1: 0000000000001000 42 FUNC GLOBAL DEFAULT 12 authenticate_drm_xcb'
	printf '%s\n' \
		'2: 0000000000001100 42 FUNC GLOBAL DEFAULT 13 authenticate_drm_xcb'
} > "$known_bad_duplicate"

symbol_table_exports_helper "$known_good"
symbol_table_exports_helper "$known_good_versioned"
for known_bad in "$known_bad_undefined" "$known_bad_object" \
		"$known_bad_local" "$known_bad_nondefault" "$known_bad_duplicate"; do
	if symbol_table_exports_helper "$known_bad"; then
		echo "invalid XCB helper symbol table was accepted: $known_bad" >&2
		exit 1
	fi
done

if [ "$#" -eq 2 ]; then
	elf_object=$2
	if [ ! -f "$elf_object" ]; then
		echo "XCB helper object is unavailable: $elf_object" >&2
		exit 2
	fi
	if ! command -v readelf >/dev/null 2>&1; then
		echo "readelf is unavailable" >&2
		exit 2
	fi
	readelf --dyn-syms --wide "$elf_object" > "$scratch/dynamic-symbols.txt"
	if ! symbol_table_exports_helper "$scratch/dynamic-symbols.txt"; then
		echo "XCB helper ABI does not export one defined function" >&2
		exit 1
	fi
fi

echo "XCB helper ABI: one defined global or weak function exported"

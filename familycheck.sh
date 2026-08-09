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

# Every family named by a PCI ID table reaches the family enum and the
# display-name table.  Each input table contributes at least one CHIPSET row,
# so a missing, empty, or unparsable denominator is a failing verdict.

set -eu

scratch=$(mktemp -d "${TMPDIR:-/tmp}/radeontop-familycheck.XXXXXX")
trap 'rm -rf "$scratch"' 0 1 2 15

extract_families() {
	input_file=$1
	output_file=$2
	parsed_file=$output_file.parsed

	[ -r "$input_file" ] && [ -f "$input_file" ] || {
		echo "family input is unavailable: $input_file" >&2
		return 1
	}

	if ! awk -F, '
		/^[[:space:]]*CHIPSET[[:space:]]*\(/ {
			if (NF < 3) {
				malformed = 1
				next
			}
			family = $3
			sub(/\).*/, "", family)
			gsub(/^[[:space:]]+|[[:space:]]+$/, "", family)
			if (family != "")
				print family
		}
		END { if (malformed) exit 2 }
	' "$input_file" > "$parsed_file"; then
		echo "family parser failed: $input_file" >&2
		return 1
	fi
	if ! LC_ALL=C sort -u "$parsed_file" > "$output_file"; then
		echo "family sorter failed: $input_file" >&2
		return 1
	fi

	[ -s "$output_file" ] || {
		echo "family denominator is empty: $input_file" >&2
		return 1
	}
}

verify_families() {
	r300_table=$1
	r600_table=$2
	name_table=$3
	enum_header=$4
	r300_families=$scratch/r300-families
	r600_families=$scratch/r600-families
	all_families=$scratch/all-families
	result=0

	for required_input in "$name_table" "$enum_header"; do
		[ -r "$required_input" ] && [ -f "$required_input" ] || {
			echo "family input is unavailable: $required_input" >&2
			return 1
		}
	done

	extract_families "$r300_table" "$r300_families" || return 1
	extract_families "$r600_table" "$r600_families" || return 1
	LC_ALL=C sort -u "$r300_families" "$r600_families" > "$all_families"
	[ -s "$all_families" ] || {
		echo "combined family denominator is empty" >&2
		return 1
	}

	while IFS= read -r family; do
		grep -qw -- "$family" "$name_table" || {
			echo "$family missing from $name_table" >&2
			result=1
		}
		grep -qw -- "$family" "$enum_header" || {
			echo "$family missing from $enum_header" >&2
			result=1
		}
	done < "$all_families"

	return "$result"
}

repo_root=$(git rev-parse --show-toplevel)
cd "$repo_root"

case "${1:-}" in
	'')
		[ "$#" -eq 0 ] || exit 2
		;;
	--self-test)
		[ "$#" -eq 1 ] || exit 2
		;;
	*)
		echo "usage: $0 [--self-test]" >&2
		exit 2
		;;
esac

verify_families include/r300_pci_ids.h include/r600_pci_ids.h \
	family_str.c include/device_model.h

if [ "${1:-}" = "--self-test" ]; then
	printf '%s\n' '/* no chipset rows */' > "$scratch/empty-r300.h"
	printf '%s\n' '/* no chipset rows */' > "$scratch/empty-r600.h"
	if verify_families "$scratch/empty-r300.h" "$scratch/empty-r600.h" \
		family_str.c include/device_model.h >/dev/null 2>&1; then
		echo "negative control accepted an empty family denominator" >&2
		exit 1
	fi
	sed '/^[[:space:]]*RS480,/d' include/device_model.h \
		> "$scratch/missing-rs480.h"
	if verify_families include/r300_pci_ids.h include/r600_pci_ids.h \
		family_str.c "$scratch/missing-rs480.h" >/dev/null 2>&1; then
		echo "negative control accepted a missing RS480 enum" >&2
		exit 1
	fi
	printf '%s\n' \
		'CHIPSET(0x5974, 0, RS480)' \
		'CHIPSET(malformed)' > "$scratch/malformed-r300.h"
	if verify_families "$scratch/malformed-r300.h" include/r600_pci_ids.h \
		family_str.c include/device_model.h >/dev/null 2>&1; then
		echo "negative control masked a family-parser failure" >&2
		exit 1
	fi
	printf '%s\n' \
		"family check: known-good accepted, empty, missing-family, and parser-failure inputs rejected"
fi

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

if [ "${1:-}" != "--self-test" ] || [ "$#" -ne 1 ]; then
	echo "usage: $0 --self-test" >&2
	exit 2
fi

repo_root=$(git rev-parse --show-toplevel)
scratch=$(mktemp -d "${TMPDIR:-/tmp}/radeontop-test-dependencies.XXXXXX")
trap 'rm -rf "$scratch"' 0 1 2 15
make_database="$scratch/make-database.txt"
known_good_database="$scratch/known-good.txt"
known_bad_database="$scratch/known-bad.txt"

rule_has_prerequisite() {
	database_path=$1
	target_name=$2
	required_path=$3

	awk -v target="$target_name:" -v required="$required_path" '
		$1 == target {
			for (field = 2; field <= NF; field++)
				if ($field == required)
					found = 1
		}
		END { exit found ? 0 : 1 }
	' "$database_path"
}

printf '%s\n' \
	'tests/example: tests/example.c implementation.c' > "$known_good_database"
printf '%s\n' 'tests/example: tests/example.c' > "$known_bad_database"
if ! rule_has_prerequisite "$known_good_database" tests/example \
		implementation.c; then
	echo "known-good dependency fixture was rejected" >&2
	exit 1
fi
if rule_has_prerequisite "$known_bad_database" tests/example \
		implementation.c; then
	echo "known-bad dependency fixture was accepted" >&2
	exit 1
fi

set +e
make -C "$repo_root" -qp > "$make_database" 2>/dev/null
make_status=$?
set -e
if [ "$make_status" -gt 1 ]; then
	echo "GNU make could not produce its dependency database" >&2
	exit 2
fi

textual_include_count=0
for test_source in "$repo_root"/tests/*.c; do
	relative_test_source=${test_source#"$repo_root"/}
	test_target=${relative_test_source%.c}
	included_sources=$(sed -n \
		's@^#include "\.\./\([^"[:space:]]*[.]c\)"@\1@p' "$test_source")
	for included_source in $included_sources; do
		textual_include_count=$((textual_include_count + 1))
		if ! rule_has_prerequisite "$make_database" "$test_target" \
				"$included_source"; then
			echo "$test_target omits textual include $included_source from its prerequisites" >&2
			exit 1
		fi
	done
done

if [ "$textual_include_count" -eq 0 ]; then
	echo "no textual production includes entered the dependency denominator" >&2
	exit 1
fi

echo "test dependencies: textual includes bind to Make prerequisites"

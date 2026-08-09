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

for required_tool in git gzip make sha256sum tar; do
	if ! command -v "$required_tool" >/dev/null 2>&1; then
		echo "required distribution tool is unavailable: $required_tool" >&2
		exit 2
	fi
done

repo_root=$(git rev-parse --show-toplevel)
scratch=$(mktemp -d "${TMPDIR:-/tmp}/radeontop-dist-test.XXXXXX")
trap 'rm -rf "$scratch"' 0 1 2 15
fixture="$scratch/repo"
working_tree_paths="$scratch/working-tree-paths"
working_tree_tar="$scratch/working-tree.tar"

mkdir -p "$fixture"
git -C "$repo_root" ls-files -z --cached > "$working_tree_paths"
tar --null -C "$repo_root" --files-from="$working_tree_paths" \
	-cf "$working_tree_tar"
tar -xf "$working_tree_tar" -C "$fixture"

git -C "$fixture" init -q
git -C "$fixture" config user.name 'RadeonTop distribution test'
git -C "$fixture" config user.email 'distribution-test@example.invalid'
git -C "$fixture" add -A
git -C "$fixture" commit -qm 'Add distribution fixture'
fixture_commit=$(git -C "$fixture" rev-parse HEAD)
fixture_version=$(git -C "$fixture" describe --always HEAD)
archive_name="radeontop-$fixture_version.tgz"

# The dirty marker calibrates the old in-place sed and git-checkout failure:
# the source archive contains committed HEAD while the worktree stays untouched.
printf '%s\n' '# distribution self-test dirty marker' >> "$fixture/Makefile"
dirty_makefile_sha256=$(sha256sum "$fixture/Makefile" | awk '{print $1}')

first_output="$scratch/output-one"
second_output="$scratch/output-two"
mkdir -p "$first_output" "$second_output"
make -C "$fixture" DIST_OUTPUT_DIR="$first_output" dist >/dev/null
post_dist_makefile_sha256=$(sha256sum "$fixture/Makefile" | awk '{print $1}')
if [ "$post_dist_makefile_sha256" != "$dirty_makefile_sha256" ]; then
	echo "distribution changed the tracked Makefile" >&2
	exit 1
fi

make -C "$fixture" DIST_OUTPUT_DIR="$second_output" dist >/dev/null
first_archive="$first_output/$archive_name"
second_archive="$second_output/$archive_name"
first_sidecar="$first_archive.sha256"
second_sidecar="$second_archive.sha256"
for required_output in "$first_archive" "$second_archive" \
		"$first_sidecar" "$second_sidecar"; do
	if [ ! -f "$required_output" ]; then
		echo "distribution output is missing: $required_output" >&2
		exit 1
	fi
done

(cd "$first_output" && sha256sum -c "$(basename "$first_sidecar")") \
	>/dev/null
(cd "$second_output" && sha256sum -c "$(basename "$second_sidecar")") \
	>/dev/null
cmp "$first_archive" "$second_archive"

git -C "$fixture" tag -a invalid/name -m 'Invalid archive name fixture'
if make -C "$fixture" DIST_OUTPUT_DIR="$scratch/invalid-output" dist \
		>/dev/null 2>&1; then
	echo "distribution accepted a path-bearing Git description" >&2
	exit 1
fi
git -C "$fixture" tag -d invalid/name >/dev/null

expected_archive_sha256=$(awk '{print $1}' "$first_sidecar")
tampered_archive="$scratch/tampered.tgz"
cp "$first_archive" "$tampered_archive"
printf '%s' x >> "$tampered_archive"
tampered_sidecar="$scratch/tampered.tgz.sha256"
printf '%s  %s\n' "$expected_archive_sha256" \
	"$(basename "$tampered_archive")" > "$tampered_sidecar"
if (cd "$scratch" && sha256sum -c "$(basename "$tampered_sidecar")") \
		>/dev/null 2>&1; then
	echo "tampered distribution passed the accepted digest" >&2
	exit 1
fi

extract_root="$scratch/extract"
mkdir -p "$extract_root"
tar -xzf "$first_archive" -C "$extract_root"
export_root="$extract_root/radeontop-$fixture_version"
export_metadata="$export_root/include/radeontop-source-export.mk"
if [ ! -f "$export_metadata" ]; then
	echo "distribution source identity is missing" >&2
	exit 1
fi
grep -Fxq "VERSION ?= $fixture_version" "$export_metadata"
grep -Fxq "SOURCE_COMMIT ?= $fixture_commit" "$export_metadata"
grep -Fxq 'SOURCE_STATE ?= clean' "$export_metadata"
if grep -Fq 'distribution self-test dirty marker' "$export_root/Makefile"; then
	echo "distribution captured dirty worktree content" >&2
	exit 1
fi
grep -Fq "./getver.sh \$(sort \$(identity_inputs))" "$export_root/Makefile"

make -C "$export_root" include/version.h nls=0 xcb=0 amdgpu=0 \
	CFLAGS=-O2 LDFLAGS=-Wl,-O1 LIBS=-lm >/dev/null
read_macro() {
	awk -v key="$1" '
		$1 == "#define" && $2 == key {
			value = $3
			sub(/^"/, "", value)
			sub(/"$/, "", value)
			print value
		}
	' "$export_root/include/version.h"
}

if [ "$(read_macro RADEONTOP_SOURCE_COMMIT)" != "$fixture_commit" ]; then
	echo "distribution commit identity does not reach the generated header" >&2
	exit 1
fi
if [ "$(read_macro RADEONTOP_SOURCE_STATE)" != clean ]; then
	echo "distribution source state does not reach the generated header" >&2
	exit 1
fi
first_source_sha256=$(read_macro RADEONTOP_SOURCE_SHA256)
first_build_sha256=$(read_macro RADEONTOP_BUILD_MANIFEST_SHA256)
(cd "$export_root" && \
	sha256sum -c include/radeontop-source-manifest.txt) >/dev/null
grep -Eq '^[0-9a-f]{64}  Makefile$' \
	"$export_root/include/radeontop-source-manifest.txt"
grep -Eq '^[0-9a-f]{64}  include/radeontop-source-export[.]mk$' \
	"$export_root/include/radeontop-source-manifest.txt"
retained_source_sha256=$(sha256sum \
	"$export_root/include/radeontop-source-manifest.txt" | awk '{print $1}')
if [ "$retained_source_sha256" != "$first_source_sha256" ]; then
	echo "distribution source-manifest digest does not reproduce" >&2
	exit 1
fi

make -C "$export_root" include/version.h nls=0 xcb=0 amdgpu=0 \
	CFLAGS=-O3 LDFLAGS=-Wl,-O1 LIBS=-lm >/dev/null
if [ "$(read_macro RADEONTOP_SOURCE_SHA256)" != "$first_source_sha256" ]; then
	echo "build flags changed the distribution source identity" >&2
	exit 1
fi
if [ "$(read_macro RADEONTOP_BUILD_MANIFEST_SHA256)" = \
		"$first_build_sha256" ]; then
	echo "build flags did not change the distribution build identity" >&2
	exit 1
fi

invalid_commit=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcde
if make -C "$export_root" include/version.h nls=0 xcb=0 amdgpu=0 \
		SOURCE_COMMIT="$invalid_commit" >/dev/null 2>&1; then
	echo "distribution accepted an invalid source object" >&2
	exit 1
fi

echo "distribution: deterministic export accepted, mutations rejected"

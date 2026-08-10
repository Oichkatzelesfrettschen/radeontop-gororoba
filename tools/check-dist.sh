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

for required_tool in git gzip make sha256sum stat tar; do
	if ! command -v "$required_tool" >/dev/null 2>&1; then
		echo "required distribution tool is unavailable: $required_tool" >&2
		exit 2
	fi
done

repo_root=$(git rev-parse --show-toplevel)
scratch=$(mktemp -d "${TMPDIR:-/tmp}/radeontop-dist-test.XXXXXX")
trap 'rm -rf "$scratch"' 0
trap 'exit 129' 1
trap 'exit 130' 2
trap 'exit 143' 15
fixture="$scratch/repo"
working_tree_paths="$scratch/working-tree-paths"
working_tree_tar="$scratch/working-tree.tar"

mkdir -p "$fixture"
git -C "$repo_root" ls-files -z --cached > "$working_tree_paths"
tar --null -C "$repo_root" --files-from="$working_tree_paths" \
	-cf "$working_tree_tar"
tar --same-permissions -xf "$working_tree_tar" -C "$fixture"

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
if make -C "$fixture" DIST_OUTPUT_DIR= dist >/dev/null 2>&1; then
	echo "distribution accepted an implicit output directory" >&2
	exit 1
fi
(umask 077; make -C "$fixture" DIST_OUTPUT_DIR="$first_output" dist >/dev/null)
post_dist_makefile_sha256=$(sha256sum "$fixture/Makefile" | awk '{print $1}')
if [ "$post_dist_makefile_sha256" != "$dirty_makefile_sha256" ]; then
	echo "distribution changed the tracked Makefile" >&2
	exit 1
fi

(umask 022; make -C "$fixture" DIST_OUTPUT_DIR="$second_output" dist >/dev/null)
first_archive="$first_output/$archive_name"
second_archive="$second_output/$archive_name"
first_sidecar="$first_archive.sha256"
second_sidecar="$second_archive.sha256"
first_baseline_sidecar="$first_output/radeontop-$fixture_version.source-baseline.sha256"
second_baseline_sidecar="$second_output/radeontop-$fixture_version.source-baseline.sha256"
for required_output in "$first_archive" "$second_archive" \
		"$first_sidecar" "$second_sidecar" \
		"$first_baseline_sidecar" "$second_baseline_sidecar"; do
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
cmp "$first_baseline_sidecar" "$second_baseline_sidecar"
if [ "$(wc -l < "$first_sidecar")" -ne 2 ]; then
	echo "distribution digest manifest does not contain the exact output pair" >&2
	exit 1
fi

tampered_pair="$scratch/tampered-pair"
mkdir -p "$tampered_pair"
cp "$first_archive" "$first_sidecar" "$first_baseline_sidecar" "$tampered_pair/"
printf '%s' x >> "$tampered_pair/$(basename "$first_baseline_sidecar")"
if (cd "$tampered_pair" && sha256sum -c "$(basename "$first_sidecar")") \
		>/dev/null 2>&1; then
	echo "tampered source-baseline sidecar passed the distribution digest manifest" >&2
	exit 1
fi

git -C "$fixture" tag -a invalid/name -m 'Invalid archive name fixture'
if make -C "$fixture" DIST_OUTPUT_DIR="$scratch/invalid-output" dist \
		>/dev/null 2>&1; then
	echo "distribution accepted a path-bearing Git description" >&2
	exit 1
fi
git -C "$fixture" tag -d invalid/name >/dev/null

expected_archive_sha256=$(awk -v name="$archive_name" '
	NF == 2 && $2 == name { print $1 }
' "$first_sidecar")
case "$expected_archive_sha256" in
	*[!0-9a-f]*|'')
		echo "distribution archive digest entry is malformed" >&2
		exit 1
		;;
esac
if [ "${#expected_archive_sha256}" -ne 64 ]; then
	echo "distribution archive digest entry length differs" >&2
	exit 1
fi
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
tar --same-permissions -xzf "$first_archive" -C "$extract_root"
export_root="$extract_root/radeontop-$fixture_version"
export_metadata="$export_root/include/radeontop-source-export.mk"
export_baseline="$export_root/include/radeontop-source-export-baseline.sha256"
if [ ! -f "$export_metadata" ]; then
	echo "distribution source identity is missing" >&2
	exit 1
fi
if [ ! -f "$export_baseline" ]; then
	echo "distribution source baseline is missing" >&2
	exit 1
fi
if [ "$(stat -c %a "$export_metadata")" != 644 ]; then
	echo "distribution source identity mode is not 0644" >&2
	exit 1
fi
if [ "$(stat -c %a "$export_baseline")" != 644 ]; then
	echo "distribution source baseline mode is not 0644" >&2
	exit 1
fi
grep -Fxq "VERSION ?= $fixture_version" "$export_metadata"
grep -Fxq "SOURCE_COMMIT ?= $fixture_commit" "$export_metadata"
grep -Fxq 'SOURCE_STATE ?= unknown' "$export_metadata"
grep -Fxq \
	'SOURCE_BASELINE ?= include/radeontop-source-export-baseline.sha256' \
	"$export_metadata"
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
if [ "$(read_macro RADEONTOP_SOURCE_STATE)" != unknown ]; then
	echo "unanchored distribution source state is not unknown" >&2
	exit 1
fi
grep -Fq 'source_baseline_sha256=none' \
	"$export_root/include/radeontop-build-manifest.txt"
expected_baseline_sha256=$(awk '
	NF == 2 && $2 == "include/radeontop-source-export-baseline.sha256" {
		print $1
	}
' "$first_baseline_sidecar")
case "$expected_baseline_sha256" in
	*[!0-9a-f]*|'')
		echo "distribution source-baseline sidecar is malformed" >&2
		exit 1
		;;
esac
if [ "${#expected_baseline_sha256}" -ne 64 ] ||
		[ "$(sha256sum "$export_baseline" | awk '{print $1}')" != \
		"$expected_baseline_sha256" ]; then
	echo "distribution source-baseline sidecar differs from the extracted baseline" >&2
	exit 1
fi

make -C "$export_root" include/version.h nls=0 xcb=0 amdgpu=0 \
	SOURCE_BASELINE_SHA256="$expected_baseline_sha256" \
	CFLAGS=-O2 LDFLAGS=-Wl,-O1 LIBS=-lm >/dev/null
if [ "$(read_macro RADEONTOP_SOURCE_STATE)" != clean ]; then
	echo "externally anchored distribution source state is not clean" >&2
	exit 1
fi
grep -Fq "source_baseline_sha256=$expected_baseline_sha256" \
	"$export_root/include/radeontop-build-manifest.txt"
first_source_sha256=$(read_macro RADEONTOP_SOURCE_SHA256)
first_build_sha256=$(read_macro RADEONTOP_BUILD_MANIFEST_SHA256)
(cd "$export_root" && \
	sha256sum -c include/radeontop-source-manifest.txt) >/dev/null
grep -Eq '^[0-9a-f]{64}  Makefile$' \
	"$export_root/include/radeontop-source-manifest.txt"
grep -Eq '^[0-9a-f]{64}  include/radeontop-source-export[.]mk$' \
	"$export_root/include/radeontop-source-manifest.txt"
grep -Eq \
	'^[0-9a-f]{64}  include/radeontop-source-export-baseline[.]sha256$' \
	"$export_root/include/radeontop-source-manifest.txt"
retained_source_sha256=$(sha256sum \
	"$export_root/include/radeontop-source-manifest.txt" | awk '{print $1}')
if [ "$retained_source_sha256" != "$first_source_sha256" ]; then
	echo "distribution source-manifest digest does not reproduce" >&2
	exit 1
fi

saved_auth="$scratch/export-auth.c"
saved_baseline="$scratch/source-export-baseline.sha256"
cp "$export_root/auth.c" "$saved_auth"
cp "$export_baseline" "$saved_baseline"
printf '%s\n' '// distribution source mutation' >> "$export_root/auth.c"
make -C "$export_root" include/version.h nls=0 xcb=0 amdgpu=0 \
	SOURCE_BASELINE_SHA256="$expected_baseline_sha256" \
	CFLAGS=-O2 LDFLAGS=-Wl,-O1 LIBS=-lm >/dev/null
if [ "$(read_macro RADEONTOP_SOURCE_STATE)" != dirty ]; then
	echo "anchored distribution source mutation is not dirty" >&2
	exit 1
fi
if [ "$(read_macro RADEONTOP_SOURCE_SHA256)" = "$first_source_sha256" ]; then
	echo "distribution source mutation retained the clean digest" >&2
	exit 1
fi

mutated_auth_sha256=$(sha256sum "$export_root/auth.c" | awk '{print $1}')
awk -v digest="$mutated_auth_sha256" '
	$2 == "auth.c" { $1 = digest }
	{ printf "%s  %s\n", $1, $2 }
' "$saved_baseline" > "$export_baseline"
make -C "$export_root" include/version.h nls=0 xcb=0 amdgpu=0 \
	CFLAGS=-O2 LDFLAGS=-Wl,-O1 LIBS=-lm >/dev/null
if [ "$(read_macro RADEONTOP_SOURCE_STATE)" != unknown ]; then
	echo "unanchored synchronized source mutation is not unknown" >&2
	exit 1
fi
if [ "$(read_macro RADEONTOP_SOURCE_SHA256)" = "$first_source_sha256" ]; then
	echo "synchronized source mutation retained the clean digest" >&2
	exit 1
fi
if make -C "$export_root" include/version.h nls=0 xcb=0 amdgpu=0 \
		SOURCE_BASELINE_SHA256="$expected_baseline_sha256" \
		CFLAGS=-O2 LDFLAGS=-Wl,-O1 LIBS=-lm >/dev/null 2>&1; then
	echo "synchronized source and baseline mutation passed the original anchor" >&2
	exit 1
fi

cp "$saved_auth" "$export_root/auth.c"
if make -C "$export_root" include/version.h nls=0 xcb=0 amdgpu=0 \
		SOURCE_BASELINE_SHA256="$expected_baseline_sha256" \
		CFLAGS=-O2 LDFLAGS=-Wl,-O1 LIBS=-lm >/dev/null 2>&1; then
	echo "baseline-only mutation passed the original anchor" >&2
	exit 1
fi
cp "$saved_baseline" "$export_baseline"
make -C "$export_root" include/version.h nls=0 xcb=0 amdgpu=0 \
	SOURCE_BASELINE_SHA256="$expected_baseline_sha256" \
	CFLAGS=-O2 LDFLAGS=-Wl,-O1 LIBS=-lm >/dev/null
if [ "$(read_macro RADEONTOP_SOURCE_STATE)" != clean ] ||
		[ "$(read_macro RADEONTOP_SOURCE_SHA256)" != "$first_source_sha256" ]; then
	echo "restored distribution source did not recover its clean identity" >&2
	exit 1
fi

make -C "$export_root" include/version.h nls=0 xcb=0 amdgpu=0 \
	SOURCE_BASELINE_SHA256="$expected_baseline_sha256" \
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

fixture_commit_first=$(printf '%s' "$fixture_commit" | cut -c1)
if [ "$fixture_commit_first" = 0 ]; then
	wrong_commit_first=1
else
	wrong_commit_first=0
fi
wrong_valid_commit=$wrong_commit_first$(printf '%s' "$fixture_commit" | cut -c2-)
if make -C "$export_root" include/version.h nls=0 xcb=0 amdgpu=0 \
		SOURCE_COMMIT="$wrong_valid_commit" \
		SOURCE_BASELINE_SHA256="$expected_baseline_sha256" >/dev/null 2>&1; then
	echo "valid but false source commit passed authenticated export metadata" >&2
	exit 1
fi

wrong_valid_version="$fixture_version.wrong"
if make -C "$export_root" include/version.h nls=0 xcb=0 amdgpu=0 \
		VERSION="$wrong_valid_version" \
		SOURCE_BASELINE_SHA256="$expected_baseline_sha256" >/dev/null 2>&1; then
	echo "valid but false version passed authenticated export metadata" >&2
	exit 1
fi

invalid_commit=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcde
if make -C "$export_root" include/version.h nls=0 xcb=0 amdgpu=0 \
		SOURCE_COMMIT="$invalid_commit" \
		SOURCE_BASELINE_SHA256="$expected_baseline_sha256" >/dev/null 2>&1; then
	echo "distribution accepted an invalid source object" >&2
	exit 1
fi

rm -f "$export_baseline"
if make -C "$export_root" include/version.h nls=0 xcb=0 amdgpu=0 \
		SOURCE_BASELINE_SHA256="$expected_baseline_sha256" >/dev/null 2>&1; then
	echo "distribution accepted a missing source baseline" >&2
	exit 1
fi
cp "$saved_baseline" "$export_baseline"
sed '$d' "$saved_baseline" > "$export_baseline"
incomplete_baseline_sha256=$(sha256sum "$export_baseline" | awk '{print $1}')
if make -C "$export_root" include/version.h nls=0 xcb=0 amdgpu=0 \
		SOURCE_BASELINE_SHA256="$incomplete_baseline_sha256" >/dev/null 2>&1; then
	echo "distribution accepted an incomplete source baseline" >&2
	exit 1
fi
cp "$saved_baseline" "$export_baseline"

if make -C "$export_root" include/version.h nls=0 xcb=0 amdgpu=0 \
		SOURCE_BASELINE_SHA256=ABCDEF >/dev/null 2>&1; then
	echo "distribution accepted a malformed external baseline digest" >&2
	exit 1
fi

echo "distribution: explicit deterministic export accepted, implicit and mutated outputs rejected"

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
scratch=$(mktemp -d "${TMPDIR:-/tmp}/radeontop-build-identity-test.XXXXXX")
trap 'rm -rf "$scratch"' 0 1 2 15
fixture="$scratch/repo"

mkdir -p "$fixture/include"
cp "$repo_root/getver.sh" "$fixture/getver.sh"
chmod +x "$fixture/getver.sh"
printf '%s\n' \
	'include/version.h' \
	'include/radeontop-source-manifest.txt' \
	'include/radeontop-build-manifest.txt' > "$fixture/.gitignore"
printf '%s\n' 'int identity_fixture(void) { return 1; }' > "$fixture/input.c"

git -C "$fixture" init -q
git -C "$fixture" config user.name 'RadeonTop identity test'
git -C "$fixture" config user.email 'identity-test@example.invalid'
git -C "$fixture" add .gitignore getver.sh input.c
git -C "$fixture" commit -qm 'Add identity fixture'
fixture_commit=$(git -C "$fixture" rev-parse HEAD)

generate_identity() {
	(
		cd "$fixture"
		RADEONTOP_VERSION=1.4.rtest.g000000000000 \
		RADEONTOP_BUILD_CC=cc \
		RADEONTOP_BUILD_CC_VERSION='fixture compiler 1' \
		RADEONTOP_BUILD_CPPFLAGS='' \
		RADEONTOP_BUILD_CFLAGS="${identity_cflags:--O2}" \
		RADEONTOP_BUILD_LDFLAGS=-Wl,-O1 \
		RADEONTOP_BUILD_LIBS=-lm \
		RADEONTOP_BUILD_OPTIONS='nls=0 xcb=0 amdgpu=0' \
		./getver.sh getver.sh input.c
	)
}

read_macro() {
	awk -v key="$1" '
		$1 == "#define" && $2 == key {
			value = $3
			sub(/^"/, "", value)
			sub(/"$/, "", value)
			print value
		}
	' "$fixture/include/version.h"
}

assert_sha256() {
	case "$1" in
		*[!0-9a-f]*|'') return 1 ;;
	esac
	[ "${#1}" -eq 64 ]
}

generate_identity
[ "$(read_macro RADEONTOP_SOURCE_COMMIT)" = "$fixture_commit" ]
[ "$(read_macro RADEONTOP_SOURCE_STATE)" = clean ]
clean_source_sha256=$(read_macro RADEONTOP_SOURCE_SHA256)
clean_build_sha256=$(read_macro RADEONTOP_BUILD_MANIFEST_SHA256)
assert_sha256 "$clean_source_sha256"
assert_sha256 "$clean_build_sha256"
retained_source_sha256=$(sha256sum \
	"$fixture/include/radeontop-source-manifest.txt" | awk '{print $1}')
retained_build_sha256=$(sha256sum \
	"$fixture/include/radeontop-build-manifest.txt" | awk '{print $1}')
[ "$retained_source_sha256" = "$clean_source_sha256" ]
[ "$retained_build_sha256" = "$clean_build_sha256" ]
grep -Fq 'schema=radeontop_build_manifest_v1' \
	"$fixture/include/radeontop-build-manifest.txt"
grep -Fq "source_commit=$fixture_commit" \
	"$fixture/include/radeontop-build-manifest.txt"
grep -Fq 'version_hex=312e342e72746573742e67303030303030303030303030' \
	"$fixture/include/radeontop-build-manifest.txt"
grep -Eq '^[0-9a-f]{64}  input.c$' \
	"$fixture/include/radeontop-source-manifest.txt"

cat > "$scratch/emit-manifest.c" << 'EOF'
#include "version.h"

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
	const char *manifest;

	if (argc != 2 || (argv[1][0] != 's' && argv[1][0] != 'b') || argv[1][1])
		return 2;
	manifest = argv[1][0] == 's' ? RADEONTOP_SOURCE_MANIFEST :
		RADEONTOP_BUILD_MANIFEST;
	return fwrite(manifest, 1, strlen(manifest), stdout) == strlen(manifest) ?
		0 : 1;
}
EOF
identity_cc=${CC:-cc}
"$identity_cc" -std=c11 -Wall -Wextra -Werror -I "$fixture/include" \
	-o "$scratch/emit-manifest" "$scratch/emit-manifest.c"
"$scratch/emit-manifest" s > "$scratch/embedded-source-manifest.txt"
"$scratch/emit-manifest" b > "$scratch/embedded-build-manifest.txt"
cmp "$scratch/embedded-source-manifest.txt" \
	"$fixture/include/radeontop-source-manifest.txt"
cmp "$scratch/embedded-build-manifest.txt" \
	"$fixture/include/radeontop-build-manifest.txt"

printf '%s\n' 'int identity_fixture(void) { return 2; }' > "$fixture/input.c"
if RADEONTOP_SOURCE_STATE=clean generate_identity >/dev/null 2>&1; then
	echo "dirty checkout accepted an asserted clean state" >&2
	exit 1
fi
generate_identity
[ "$(read_macro RADEONTOP_SOURCE_STATE)" = dirty ]
dirty_source_sha256=$(read_macro RADEONTOP_SOURCE_SHA256)
[ "$dirty_source_sha256" != "$clean_source_sha256" ]

git -C "$fixture" restore input.c
identity_cflags=-O3 generate_identity
[ "$(read_macro RADEONTOP_SOURCE_STATE)" = clean ]
[ "$(read_macro RADEONTOP_SOURCE_SHA256)" = "$clean_source_sha256" ]
[ "$(read_macro RADEONTOP_BUILD_MANIFEST_SHA256)" != "$clean_build_sha256" ]

exported="$scratch/export"
mkdir -p "$exported/include"
cp "$fixture/getver.sh" "$exported/getver.sh"
cp "$fixture/input.c" "$exported/input.c"
(
	cd "$exported"
	RADEONTOP_VERSION=1.4.rexport.g000000000000 \
	RADEONTOP_SOURCE_COMMIT="$fixture_commit" \
	RADEONTOP_SOURCE_STATE=clean \
	./getver.sh getver.sh input.c
)
grep -Fq "#define RADEONTOP_SOURCE_COMMIT \"$fixture_commit\"" \
	"$exported/include/version.h"
grep -Fq '#define RADEONTOP_SOURCE_STATE "clean"' \
	"$exported/include/version.h"
exported_source_sha256=$(sha256sum \
	"$exported/include/radeontop-source-manifest.txt" | awk '{print $1}')
exported_header_sha256=$(awk '
	$1 == "#define" && $2 == "RADEONTOP_SOURCE_SHA256" {
		gsub(/"/, "", $3)
		print $3
	}
' "$exported/include/version.h")
[ "$exported_source_sha256" = "$exported_header_sha256" ]

if (
	cd "$exported"
	RADEONTOP_VERSION='invalid version' ./getver.sh getver.sh input.c
) >/dev/null 2>&1; then
	echo "invalid version accepted" >&2
	exit 1
fi

echo "build identity: known-good accepted, known-bad rejected"

#!/bin/sh

# The Makefile passes every source input as a repository-relative argument and
# exports the expanded compiler, flags, link inputs, and feature selections.
# This script binds those byte streams and options to a deterministic identity.

set -eu

out=include/version.h
tmp="$out.tmp"
source_manifest_out=include/radeontop-source-manifest.txt
build_manifest_out=include/radeontop-build-manifest.txt
scratch=$(mktemp -d "${TMPDIR:-/tmp}/radeontop-build-identity.XXXXXX")
trap 'rm -rf "$scratch" "$tmp" "$source_manifest_out.tmp" "$build_manifest_out.tmp"' 0 1 2 15

version=${RADEONTOP_VERSION:-}
source_commit=${RADEONTOP_SOURCE_COMMIT:-}
source_state=${RADEONTOP_SOURCE_STATE:-}
git_root=
live_commit=
live_state=

if command -v git >/dev/null 2>&1; then
	git_root=$(git rev-parse --show-toplevel 2>/dev/null || true)
fi

# An exported tree can sit inside an unrelated checkout.  Git metadata applies
# only when its root equals the source root.
if [ -n "$git_root" ] && [ "$(CDPATH='' cd -- "$git_root" && pwd -P)" = "$(pwd -P)" ]; then
	live_commit=$(git rev-parse --verify HEAD)
	if [ -n "$(git status --porcelain=v1 --untracked-files=all)" ]; then
		live_state=dirty
	else
		live_state=clean
	fi

	if [ -n "$source_commit" ] && [ "$source_commit" != "$live_commit" ]; then
		echo "SOURCE_COMMIT does not match the checkout HEAD" >&2
		exit 2
	fi
	if [ -n "$source_state" ] && [ "$source_state" != "$live_state" ]; then
		echo "SOURCE_STATE does not match the checkout state" >&2
		exit 2
	fi

	source_commit=$live_commit
	source_state=$live_state
	if [ -z "$version" ]; then
		version=$(git describe --always --dirty=-dirty 2>/dev/null || printf unknown)
	fi
else
	: "${source_commit:=unknown}"
	: "${source_state:=unknown}"
	: "${version:=unknown}"
fi

case "$version" in
	''|*[!A-Za-z0-9._+~/:-]*)
		echo "VERSION contains a character outside the capture identity alphabet" >&2
		exit 2
		;;
esac
case "$source_commit" in
	unknown) ;;
	*[!0-9a-f]*|'')
		echo "SOURCE_COMMIT must be unknown or a lowercase hexadecimal object id" >&2
		exit 2
		;;
	*)
		case "${#source_commit}" in
			40|64) ;;
			*)
				echo "SOURCE_COMMIT must contain 40 or 64 hexadecimal characters" >&2
				exit 2
				;;
		esac
		;;
esac
case "$source_state" in
	clean|dirty|unknown) ;;
	*)
		echo "SOURCE_STATE must be clean, dirty, or unknown" >&2
		exit 2
		;;
esac

if [ "$#" -eq 0 ]; then
	echo "getver.sh requires the production source-input denominator" >&2
	exit 2
fi

source_manifest="$scratch/source-inputs.sha256"
for input_path do
	case "$input_path" in
		/*|*'..'*|*[!A-Za-z0-9_./+-]*)
			echo "invalid source-input path: $input_path" >&2
			exit 2
			;;
	esac
	if [ ! -f "$input_path" ]; then
		echo "missing source-input path: $input_path" >&2
		exit 2
	fi
	sha256sum -- "$input_path"
done | LC_ALL=C sort > "$source_manifest"

source_sha256=$(sha256sum -- "$source_manifest" | awk '{print $1}')

hex_field() {
	field_name=$1
	field_value=$2
	field_hex=$(printf '%s' "$field_value" | od -An -v -tx1 | tr -d ' \n')
	printf '%s_hex=%s\n' "$field_name" "$field_hex"
}

build_manifest="$scratch/build-manifest.txt"
{
	printf '%s\n' 'schema=radeontop_build_manifest_v1'
	printf 'source_manifest_sha256=%s\n' "$source_sha256"
	printf 'source_commit=%s\n' "$source_commit"
	printf 'source_state=%s\n' "$source_state"
	hex_field version "$version"
	hex_field compiler "${RADEONTOP_BUILD_CC:-}"
	hex_field compiler_version "${RADEONTOP_BUILD_CC_VERSION:-}"
	hex_field cppflags "${RADEONTOP_BUILD_CPPFLAGS:-}"
	hex_field cflags "${RADEONTOP_BUILD_CFLAGS:-}"
	hex_field ldflags "${RADEONTOP_BUILD_LDFLAGS:-}"
	hex_field libs "${RADEONTOP_BUILD_LIBS:-}"
	hex_field options "${RADEONTOP_BUILD_OPTIONS:-}"
} > "$build_manifest"

build_manifest_sha256=$(sha256sum -- "$build_manifest" | awk '{print $1}')

write_c_string_macro() {
	macro_name=$1
	manifest_path=$2

	printf '#define %s \\\n' "$macro_name"
	od -An -v -t u1 "$manifest_path" | awk '
		{
			encoded = ""
			for (field = 1; field <= NF; field++)
				encoded = encoded sprintf("\\%03o", $field)
			printf "\"%s\" \\\n", encoded
		}
		END { print "\"\"" }
	'
}

{
cat << EOF
#ifndef VER_H
#define VER_H

#define VERSION "$version"
#define RADEONTOP_SOURCE_COMMIT "$source_commit"
#define RADEONTOP_SOURCE_STATE "$source_state"
#define RADEONTOP_SOURCE_SHA256 "$source_sha256"
#define RADEONTOP_BUILD_MANIFEST_SHA256 "$build_manifest_sha256"
EOF
	write_c_string_macro RADEONTOP_SOURCE_MANIFEST "$source_manifest"
	write_c_string_macro RADEONTOP_BUILD_MANIFEST "$build_manifest"
cat << EOF
#endif
EOF
} > "$tmp"

update_output() {
	input_file=$1
	output_file=$2

	cp -- "$input_file" "$output_file.tmp"
	if cmp -s "$output_file.tmp" "$output_file"; then
		rm -f "$output_file.tmp"
	else
		mv -f "$output_file.tmp" "$output_file"
	fi
}

update_output "$source_manifest" "$source_manifest_out"
update_output "$build_manifest" "$build_manifest_out"

# Replace the header on a changed value only.  Every object includes it, so an
# unconditional write moves the mtime and rebuilds the whole tree each time the
# version target runs.
if cmp -s "$tmp" "$out"; then
	rm -f "$tmp"
else
	mv -f "$tmp" "$out"
fi

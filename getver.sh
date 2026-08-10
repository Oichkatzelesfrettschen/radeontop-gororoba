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
trap 'rm -rf "$scratch" "$tmp" "$source_manifest_out.tmp" "$build_manifest_out.tmp"' 0
trap 'exit 129' 1
trap 'exit 130' 2
trap 'exit 143' 15

version=${RADEONTOP_VERSION:-}
source_commit=${RADEONTOP_SOURCE_COMMIT:-}
source_state=${RADEONTOP_SOURCE_STATE:-}
source_baseline=${RADEONTOP_SOURCE_BASELINE:-}
source_baseline_sha256=${RADEONTOP_SOURCE_BASELINE_SHA256:-}
admitted_baseline_sha256=none
source_export_metadata=include/radeontop-source-export.mk
git_root=
live_commit=
live_state=

case "$source_state" in
	''|clean|dirty|unknown) ;;
	*)
		echo "SOURCE_STATE must be clean, dirty, or unknown" >&2
		exit 2
		;;
esac

case "$source_baseline_sha256" in
	'') ;;
	*[!0-9a-f]*)
		echo "SOURCE_BASELINE_SHA256 must contain 64 lowercase hexadecimal characters" >&2
		exit 2
		;;
	*)
		if [ "${#source_baseline_sha256}" -ne 64 ]; then
			echo "SOURCE_BASELINE_SHA256 must contain 64 lowercase hexadecimal characters" >&2
			exit 2
		fi
		;;
esac

validate_input_path() {
	candidate_path=$1

	case "$candidate_path" in
		/*|*'..'*|*[!A-Za-z0-9_./+-]*)
			echo "invalid source-input path: $candidate_path" >&2
			exit 2
			;;
	esac
}

source_manifest_unsorted="$scratch/source-inputs.unsorted.sha256"
identity_paths="$scratch/identity-paths.txt"
state_paths_unsorted="$scratch/state-paths.unsorted.txt"
source_manifest="$scratch/source-inputs.sha256"
state_paths="$scratch/state-paths.txt"
argument_class=identity
separator_count=0
identity_count=0
state_count=0
: > "$source_manifest_unsorted"
: > "$identity_paths"
: > "$state_paths_unsorted"

for input_path do
	if [ "$input_path" = -- ]; then
		separator_count=$((separator_count + 1))
		argument_class=state
		continue
	fi

	validate_input_path "$input_path"
	if [ "$argument_class" = identity ]; then
		if [ ! -f "$input_path" ]; then
			echo "missing source-input path: $input_path" >&2
			exit 2
		fi
		sha256sum -- "$input_path" >> "$source_manifest_unsorted"
		printf '%s\n' "$input_path" >> "$identity_paths"
		identity_count=$((identity_count + 1))
	else
		printf '%s\n' "$input_path" >> "$state_paths_unsorted"
		state_count=$((state_count + 1))
	fi
done

if [ "$separator_count" -ne 1 ] || [ "$identity_count" -eq 0 ] ||
		[ "$state_count" -eq 0 ]; then
	echo "getver.sh requires identity and source-state input denominators" >&2
	exit 2
fi

if [ -n "$(LC_ALL=C sort "$identity_paths" | uniq -d)" ] ||
		[ -n "$(LC_ALL=C sort "$state_paths_unsorted" | uniq -d)" ]; then
	echo "source-input denominators contain a duplicate path" >&2
	exit 2
fi

LC_ALL=C sort "$source_manifest_unsorted" > "$source_manifest"
LC_ALL=C sort "$state_paths_unsorted" > "$state_paths"
source_sha256=$(sha256sum -- "$source_manifest" | awk '{print $1}')

if command -v git >/dev/null 2>&1; then
	git_root=$(git rev-parse --show-toplevel 2>/dev/null || true)
fi

# An exported tree can sit inside an unrelated checkout.  Git metadata applies
# only when its root equals the source root.
if [ -n "$git_root" ] && [ "$(CDPATH='' cd -- "$git_root" && pwd -P)" = "$(pwd -P)" ]; then
	if [ -n "$source_baseline" ] || [ -n "$source_baseline_sha256" ]; then
		echo "SOURCE_BASELINE and SOURCE_BASELINE_SHA256 apply only to exported trees" >&2
		exit 2
	fi
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
	: "${version:=unknown}"

	if [ -n "$source_baseline" ]; then
		validate_input_path "$source_baseline"
		if [ ! -f "$source_baseline" ] || [ -L "$source_baseline" ]; then
			echo "source export baseline is not a regular file" >&2
			exit 2
		fi
		baseline_snapshot="$scratch/source-export-baseline.sha256"
		cp -- "$source_baseline" "$baseline_snapshot"
		actual_baseline_sha256=$(sha256sum -- "$baseline_snapshot" | awk '{print $1}')
		if [ -n "$source_baseline_sha256" ] &&
				[ "$actual_baseline_sha256" != "$source_baseline_sha256" ]; then
			echo "source export baseline digest differs from the external expectation" >&2
			exit 2
		fi
		identity_baseline_sha256=$(awk -v path="$source_baseline" '
			$2 == path { print $1 }
		' "$source_manifest")
		if [ "$identity_baseline_sha256" != "$actual_baseline_sha256" ]; then
			echo "source export baseline changed during identity generation" >&2
			exit 2
		fi

		baseline_paths="$scratch/baseline-paths.txt"
		if ! awk '
			length($1) != 64 || $1 ~ /[^0-9a-f]/ ||
				substr($0, 65, 2) != "  " {
				exit 1
			}
			{
				path = substr($0, 67)
				if (path == "" || path ~ /^\// || path ~ /\.\./ ||
						path ~ /[^A-Za-z0-9_.\/+\-]/)
					exit 1
				print path
			}
		' "$baseline_snapshot" > "$baseline_paths"; then
			echo "source export baseline is malformed" >&2
			exit 2
		fi
		if ! cmp -s "$baseline_paths" "$state_paths"; then
			echo "source export baseline denominator differs from production inputs" >&2
			exit 2
		fi

		if [ ! -f "$source_export_metadata" ] || [ -L "$source_export_metadata" ]; then
			echo "source export metadata is not a regular file" >&2
			exit 2
		fi
		metadata_snapshot="$scratch/source-export.mk"
		cp -- "$source_export_metadata" "$metadata_snapshot"
		actual_metadata_sha256=$(sha256sum -- "$metadata_snapshot" | awk '{print $1}')
		baseline_metadata_sha256=$(awk -v path="$source_export_metadata" '
			$2 == path { print $1 }
		' "$baseline_snapshot")
		identity_metadata_sha256=$(awk -v path="$source_export_metadata" '
			$2 == path { print $1 }
		' "$source_manifest")
		if [ "$baseline_metadata_sha256" != "$actual_metadata_sha256" ] ||
				[ "$identity_metadata_sha256" != "$actual_metadata_sha256" ]; then
			echo "source export metadata differs from the authenticated identity" >&2
			exit 2
		fi
		metadata_values="$scratch/source-export-metadata-values.txt"
		if ! awk -v baseline="$source_baseline" '
			BEGIN { valid = 1 }
			NR == 1 {
				if (NF != 3 || $1 != "VERSION" || $2 != "?=") valid = 0
				else print $3
				next
			}
			NR == 2 {
				if (NF != 3 || $1 != "SOURCE_COMMIT" || $2 != "?=") valid = 0
				else print $3
				next
			}
			NR == 3 {
				if (NF != 3 || $1 != "SOURCE_STATE" || $2 != "?=" ||
						$3 != "unknown") valid = 0
				next
			}
			NR == 4 {
				if (NF != 3 || $1 != "SOURCE_BASELINE" || $2 != "?=" ||
						$3 != baseline) valid = 0
				next
			}
			{ valid = 0 }
			END { if (!valid || NR != 4) exit 1 }
		' "$metadata_snapshot" > "$metadata_values"; then
			echo "source export metadata schema differs" >&2
			exit 2
		fi
		exported_version=$(sed -n '1p' "$metadata_values")
		exported_commit=$(sed -n '2p' "$metadata_values")
		if [ "$version" != "$exported_version" ]; then
			echo "VERSION does not match the authenticated source export metadata" >&2
			exit 2
		fi
		if [ "$source_commit" != "$exported_commit" ]; then
			echo "SOURCE_COMMIT does not match the authenticated source export metadata" >&2
			exit 2
		fi

		if [ -n "$source_baseline_sha256" ]; then
			admitted_baseline_sha256=$source_baseline_sha256
			if sha256sum --status -c -- "$baseline_snapshot"; then
				baseline_state=clean
			else
				baseline_state=dirty
			fi
			if [ -n "$source_state" ] && [ "$source_state" != unknown ] &&
					[ "$source_state" != "$baseline_state" ]; then
				echo "SOURCE_STATE does not match the externally authenticated baseline" >&2
				exit 2
			fi
			source_state=$baseline_state
		else
			if [ "$source_state" = clean ] || [ "$source_state" = dirty ]; then
				echo "SOURCE_STATE clean or dirty requires SOURCE_BASELINE_SHA256" >&2
				exit 2
			fi
			source_state=unknown
		fi
	else
		if [ -n "$source_baseline_sha256" ]; then
			echo "SOURCE_BASELINE_SHA256 requires a source export baseline" >&2
			exit 2
		fi
		if [ "$source_state" = clean ] || [ "$source_state" = dirty ]; then
			echo "SOURCE_STATE clean or dirty requires an authenticated source export baseline" >&2
			exit 2
		fi
		source_state=unknown
	fi
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

hex_field() {
	field_name=$1
	field_value=$2
	field_hex=$(printf '%s' "$field_value" | od -An -v -tx1 | tr -d ' \n')
	printf '%s_hex=%s\n' "$field_name" "$field_hex"
}

build_manifest="$scratch/build-manifest.txt"
{
	printf '%s\n' 'schema=radeontop_build_manifest_v2'
	printf 'source_manifest_sha256=%s\n' "$source_sha256"
	printf 'source_commit=%s\n' "$source_commit"
	printf 'source_state=%s\n' "$source_state"
	printf 'source_baseline_sha256=%s\n' "$admitted_baseline_sha256"
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

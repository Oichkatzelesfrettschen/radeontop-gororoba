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

# Produces a bounded source denominator, lexical call graphs, cross-reference
# databases, include edges, callback bindings, complexity reports, and hashes.
# The output directory starts empty so one invocation maps one exact source tree.

set -eu

usage() {
	echo "usage: $0 OUTPUT_DIRECTORY" >&2
	exit 2
}

[ "$#" -eq 1 ] || usage

repo_root=$(git rev-parse --show-toplevel)
output_input=$1

case "$output_input" in
	/*) output_dir=$output_input ;;
	*) output_dir=$PWD/$output_input ;;
esac

if [ -d "$output_dir" ] &&
	[ -n "$(find "$output_dir" -mindepth 1 -maxdepth 1 -print -quit)" ]; then
	echo "output directory is not empty: $output_dir" >&2
	exit 2
fi

mkdir -p "$output_dir"
output_dir=$(CDPATH='' cd -- "$output_dir" && pwd)

for tool in cflow cscope ctags readtags gtags global lizard scc dot \
	git sha256sum pkg-config cc awk sed find sort wc grep rg basename cmp cp \
	dd chmod; do
	command -v "$tool" >/dev/null 2>&1 || {
		echo "required tool is unavailable: $tool" >&2
		exit 1
	}
done

cd "$repo_root"
export LC_ALL=C
source_root=$repo_root

source_denominator_valid() {
	candidate=$1
	canonical=$2

	[ -s "$candidate" ] && sort -C -u "$candidate" &&
		cmp -s "$candidate" "$canonical" || return 1

	while IFS= read -r source_file; do
		case "$source_file" in
			./*.c|./*.h) ;;
			*) return 1 ;;
		esac
		case "$source_file" in
			*[!A-Za-z0-9_./-]*) return 1 ;;
		esac
		source_path=$source_root/${source_file#./}
		[ -f "$source_path" ] && [ ! -L "$source_path" ] &&
			git -C "$repo_root" ls-files --error-unmatch -- \
				"${source_file#./}" \
					>/dev/null 2>&1 || return 1
	done < "$candidate"
}

write_source_denominator() {
	denominator_output=$1
	denominator_files=$2
	denominator_root=$3

	{
		printf 'path\tbytes\tlines\tsha256\n'
		while IFS= read -r source_file; do
			source_path=$denominator_root/${source_file#./}
			bytes=$(wc -c < "$source_path")
			lines=$(wc -l < "$source_path")
			digest=$(sha256sum "$source_path" | awk '{print $1}')
			printf '%s\t%s\t%s\t%s\n' \
				"$source_file" "$bytes" "$lines" "$digest"
		done < "$denominator_files"
	} > "$denominator_output"
}

source_state_stable() {
	candidate_start_commit=$1
	candidate_end_commit=$2
	candidate_start_branch=$3
	candidate_end_branch=$4
	candidate_start_status=$5
	candidate_end_status=$6
	candidate_start_denominator=$7
	candidate_end_denominator=$8

	[ "$candidate_start_commit" = "$candidate_end_commit" ] &&
		[ "$candidate_start_branch" = "$candidate_end_branch" ] &&
		cmp -s "$candidate_start_status" "$candidate_end_status" &&
		cmp -s "$candidate_start_denominator" "$candidate_end_denominator"
}

preprocessor_output=
checksum_tmp=
checkout_path_bad=
output_path_bad=
snapshot_path_bad=
cscope_normalized_tmp=
compiler_dependencies_raw=
analysis_snapshot_root=
cleanup() {
	for temporary_path in "$preprocessor_output" "$checksum_tmp" \
			"$checkout_path_bad" "$output_path_bad" \
			"$snapshot_path_bad" "$cscope_normalized_tmp" \
			"$compiler_dependencies_raw"; do
		if [ -n "$temporary_path" ]; then
			rm -f "$temporary_path"
		fi
	done
	if [ -n "$analysis_snapshot_root" ]; then
		find "$analysis_snapshot_root" -type d -exec chmod u+w {} +
		find "$analysis_snapshot_root" -depth -delete
	fi
}
trap cleanup 0 1 2 15

repository_commit_start=$(git -C "$repo_root" rev-parse HEAD)
repository_branch_start=$(
	git -C "$repo_root" symbolic-ref --quiet --short HEAD || printf detached
)
repository_status_start=$output_dir/.repository-status-start.tmp
git -C "$repo_root" status --short > "$repository_status_start"

canonical_sources=$output_dir/calibration-source-denominator-known-good.txt
git -C "$repo_root" ls-files -- '*.c' '*.h' | sed 's#^#./#' | sort \
	> "$canonical_sources"
source_denominator_valid "$canonical_sources" "$canonical_sources"
cp "$canonical_sources" "$output_dir/source-files.txt"
source_denominator_valid "$output_dir/source-files.txt" "$canonical_sources"

# include/version.h is a build product.  A clean Git status hides ignored files,
# so explicit tracked-file membership keeps stale build identity outside the
# primary source denominator and its lexical consumers.
git -C "$repo_root" check-ignore -q -- include/version.h
if git -C "$repo_root" ls-files --error-unmatch -- include/version.h \
		>/dev/null 2>&1; then
	echo "generated build header entered the tracked denominator" >&2
	exit 1
fi
[ -f include/version.h ] || {
	echo "generated build header is unavailable: run the Makefile target" >&2
	exit 1
}
source_untracked_bad=$output_dir/calibration-source-denominator-untracked-known-bad.txt
{
	awk '1' "$canonical_sources"
	printf '%s\n' './include/version.h'
} | sort > "$source_untracked_bad"
if source_denominator_valid "$source_untracked_bad" "$canonical_sources"; then
	echo "source denominator accepted an ignored generated header" >&2
	exit 1
fi

source_duplicate_bad=$output_dir/calibration-source-denominator-duplicate-known-bad.txt
grep -Fxq './collector.c' "$canonical_sources"
{
	awk '1' "$canonical_sources"
	printf '%s\n' './collector.c'
} | sort > "$source_duplicate_bad"
if source_denominator_valid "$source_duplicate_bad" "$canonical_sources"; then
	echo "source denominator accepted a duplicate path" >&2
	exit 1
fi

source_missing_bad=$output_dir/calibration-source-denominator-missing-known-bad.txt
grep -Fvx './collector.c' "$canonical_sources" > "$source_missing_bad"
if source_denominator_valid "$source_missing_bad" "$canonical_sources"; then
	echo "source denominator accepted a missing tracked source" >&2
	exit 1
fi

[ -s "$output_dir/source-files.txt" ] || {
	echo "source denominator is empty" >&2
	exit 1
}

awk '/\.c$/ { print }' "$output_dir/source-files.txt" \
	> "$output_dir/c-source-files.txt"
awk '/\.h$/ { print }' "$output_dir/source-files.txt" \
	> "$output_dir/header-source-files.txt"
awk '1' "$output_dir/c-source-files.txt" "$output_dir/header-source-files.txt" |
	sort > "$output_dir/source-partition-union.txt"
sort -C -u "$output_dir/source-partition-union.txt"
cmp -s "$output_dir/source-partition-union.txt" "$output_dir/source-files.txt"

awk '/^\.\/[^/]+\.c$/ { print }' "$output_dir/c-source-files.txt" \
	> "$output_dir/runtime-source-files.txt"
[ -s "$output_dir/runtime-source-files.txt" ] || {
	echo "runtime source denominator is empty" >&2
	exit 1
}

write_source_denominator "$output_dir/source-denominator.tsv" \
	"$output_dir/source-files.txt" "$repo_root"
generated_input_files=$output_dir/generated-input-files.txt
printf '%s\n' './include/version.h' > "$generated_input_files"
write_source_denominator "$output_dir/generated-input-denominator.tsv" \
	"$generated_input_files" "$repo_root"

# Every analyzer reads the same byte snapshot.  The ignored build header stays
# in a separate generated-input overlay so lexical indexes retain the exact
# tracked C and header denominator.
analysis_snapshot_root=$(mktemp -d \
	"${TMPDIR:-/tmp}/radeontop-analysis-snapshot.XXXXXX")
tracked_snapshot_root=$analysis_snapshot_root/tracked
generated_snapshot_root=$analysis_snapshot_root/generated
mkdir -p "$tracked_snapshot_root" "$generated_snapshot_root/include"
while IFS= read -r source_file; do
	snapshot_source=$tracked_snapshot_root/${source_file#./}
	mkdir -p "${snapshot_source%/*}"
	cp "$repo_root/${source_file#./}" "$snapshot_source"
done < "$output_dir/source-files.txt"
cp "$repo_root/include/version.h" \
	"$generated_snapshot_root/include/version.h"

snapshot_source_files=$output_dir/.source-snapshot-files.tmp
snapshot_source_denominator=$output_dir/.source-snapshot-denominator.tmp
snapshot_generated_denominator=$output_dir/.generated-snapshot-denominator.tmp
(
	cd "$tracked_snapshot_root"
	find . -type f -print | sort > "$snapshot_source_files"
)
cmp -s "$snapshot_source_files" "$output_dir/source-files.txt"
write_source_denominator "$snapshot_source_denominator" \
	"$output_dir/source-files.txt" "$tracked_snapshot_root"
cmp -s "$snapshot_source_denominator" \
	"$output_dir/source-denominator.tsv"
write_source_denominator "$snapshot_generated_denominator" \
	"$generated_input_files" "$generated_snapshot_root"
cmp -s "$snapshot_generated_denominator" \
	"$output_dir/generated-input-denominator.tsv"
find "$analysis_snapshot_root" -type f -exec chmod 0444 {} +
find "$analysis_snapshot_root" -type d -exec chmod 0555 {} +
if find "$analysis_snapshot_root" -perm /222 -print -quit | grep -q .; then
	echo "analysis snapshot contains a writable path" >&2
	exit 1
fi
rm -f "$snapshot_source_files" "$snapshot_source_denominator" \
	"$snapshot_generated_denominator"
printf '%s\n' \
	'schema=radeontop_source_intelligence_snapshot_v1' \
	'tracked_denominator=exact' \
	'generated_input_denominator=exact' \
	'analyzer_source=read_only_snapshot' \
	> "$output_dir/source-snapshot-validation.txt"

source_root=$tracked_snapshot_root
cd "$source_root"

{
	cflow --version | sed -n '1p'
	cscope -V 2>&1 | sed -n '1p'
	ctags --version | sed -n '1p'
	readtags --version | sed -n '1p'
	gtags --version | sed -n '1p'
	lizard --version
	scc --version
	dot -V 2>&1
	cc --version | sed -n '1p'
} > "$output_dir/tool-versions.txt"

write_call_graph() {
	graph_name=$1
	shift

	cflow --all --include=_s --number --format=gnu "$@" \
		> "$output_dir/cflow-$graph_name-forward.txt" \
		2> "$output_dir/cflow-$graph_name-forward.stderr"
	cflow --all --include=_s --number --reverse --format=gnu "$@" \
		> "$output_dir/cflow-$graph_name-reverse.txt" \
		2> "$output_dir/cflow-$graph_name-reverse.stderr"
	cflow --all --include=_s --format=dot "$@" \
		> "$output_dir/cflow-$graph_name-callgraph.dot" \
		2> "$output_dir/cflow-$graph_name-callgraph.stderr"
	dot -Tsvg "$output_dir/cflow-$graph_name-callgraph.dot" \
		-o "$output_dir/cflow-$graph_name-callgraph.svg"
	[ ! -s "$output_dir/cflow-$graph_name-forward.stderr" ]
	[ ! -s "$output_dir/cflow-$graph_name-reverse.stderr" ]
	[ ! -s "$output_dir/cflow-$graph_name-callgraph.stderr" ]
	grep -Eq '(^|[[:space:]])main\(\)' \
		"$output_dir/cflow-$graph_name-forward.txt"
	! grep -q '__radeontop_missing_symbol__' \
		"$output_dir/cflow-$graph_name-forward.txt"
}

ctags_diagnostics_valid() {
	diagnostic_file=$1

	[ -s "$diagnostic_file" ] || return 0
	awk '
		$0 == "ctags: Notice: No options will be read from files or environment" {
			notice++
			next
		}
		$0 == "ctags: Warning: Enabling Cargo subparser may enable TOML parser." {
			cargo_warning++
			next
		}
		$0 == "ctags: Warning: The current implementation of the TOML parser is broken." {
			toml_warning++
			next
		}
		{ unexpected++ }
		END {
			exit unexpected || notice > 1 || cargo_warning > 1 ||
				toml_warning > 1
		}
	' "$diagnostic_file"
}

artifact_tree_excludes_path() {
	artifact_root=$1
	forbidden_path=$2
	[ -n "$forbidden_path" ] || return 2

	if rg --hidden --no-ignore -a -F -q -- "$forbidden_path" \
			"$artifact_root"; then
		return 1
	else
		search_status=$?
	fi
	[ "$search_status" -eq 1 ]
}

normalize_cscope_database_root() {
	database_file=$1
	database_header=$(sed -n '1p' "$database_file")
	case "$database_header" in
		'cscope '*) ;;
		*) return 1 ;;
	esac

	header_after_magic=${database_header#cscope }
	database_version=${header_after_magic%% *}
	case "$database_version" in
		''|*[!0-9]*) return 1 ;;
	esac
	[ "$database_version" -ge 8 ] || return 1
	case "$database_header" in
		*' -q '*|*' -c '*|*' -T '*) return 1 ;;
	esac

	old_trailer_text=${database_header##* }
	case "$old_trailer_text" in
		''|*[!0-9]*) return 1 ;;
	esac
	old_trailer_offset=$(printf '%s\n' "$old_trailer_text" | sed 's/^0*//')
	[ -n "$old_trailer_offset" ] || old_trailer_offset=0
	old_header_bytes=$((${#database_header} + 1))
	canonical_header=$(printf 'cscope %s .               %010d' \
		"$database_version" 0)
	canonical_header_bytes=$((${#canonical_header} + 1))
	header_size_delta=$((canonical_header_bytes - old_header_bytes))
	new_trailer_offset=$((old_trailer_offset + header_size_delta))
	[ "$new_trailer_offset" -gt "$canonical_header_bytes" ] || return 1

	database_size_before=$(wc -c < "$database_file")
	cscope_normalized_tmp=$(mktemp \
		"${database_file%/*}/.cscope-normalized.XXXXXX")
	printf 'cscope %s .               %010d\n' \
		"$database_version" "$new_trailer_offset" \
		> "$cscope_normalized_tmp"
	dd if="$database_file" bs=1 skip="$old_header_bytes" status=none \
		>> "$cscope_normalized_tmp"
	expected_database_size=$((database_size_before + header_size_delta))
	[ "$(wc -c < "$cscope_normalized_tmp")" -eq "$expected_database_size" ]
	chmod 0644 "$cscope_normalized_tmp"
	mv "$cscope_normalized_tmp" "$database_file"
	cscope_normalized_tmp=

	expected_header=$(printf 'cscope %s .               %010d' \
		"$database_version" "$new_trailer_offset")
	[ "$(sed -n '1p' "$database_file")" = "$expected_header" ]
}

set --
while IFS= read -r source_file; do
	set -- "$@" "$source_file"
done < "$output_dir/runtime-source-files.txt"
write_call_graph runtime "$@"

for graph_source in ./tests/capture_test.c ./capture.c ./collector.c \
	./device_model.c ./tests/collector_test.c ./tests/detect_path_test.c ./detect.c \
	./rs480_observation.c ./tests/device_model_test.c \
	./tests/privileges_test.c ./privileges.c \
	./tests/rs480_observation_test.c; do
	grep -Fxq "$graph_source" "$output_dir/source-files.txt"
done

configured_detect_source_valid() {
	configured_source=$1
	required_symbol=$2
	excluded_symbol=$3

	[ "$(grep -c '^int main(void)' "$configured_source")" -eq 1 ] &&
		grep -Fq "$required_symbol" "$configured_source" &&
		! grep -Fq "$excluded_symbol" "$configured_source" &&
		grep -Fq 'test_open(' "$configured_source" &&
		! grep -Eq '(^|[^A-Za-z0-9_])open[[:space:]]*\(' "$configured_source"
}

configured_detect_origin_valid() {
	configured_source=$1

	awk '
		/^# [0-9]+ "/ {
			path = $3
			gsub(/^"|"$/, "", path)
			if (path != "tests/detect_path_test.c" &&
				path != "tests/../detect.c")
				invalid++
			markers++
		}
		END { exit invalid || !markers }
	' "$configured_source"
}

project_origin_preprocess() {
	lane_flag=$1
	configured_source=$2
	diagnostics=$3

	# The Makefile compiles this test with the same project include path and
	# libdrm/libpciaccess flags.  Compiler preprocessing resolves the macros set
	# inside the test before its textual detect.c inclusion.
	# shellcheck disable=SC2086
	cc -E -std=gnu11 $lane_flag -Iinclude -I../generated/include \
		$detect_compiler_flags \
		tests/detect_path_test.c > "$preprocessor_output" 2> "$diagnostics"
	[ ! -s "$diagnostics" ]

	# System declarations obscure the project call map and produce cflow parser
	# noise.  GCC line markers retain only the two project files that form this
	# textual translation unit while preserving their source line identities.
	awk '
		/^# [0-9]+ "/ {
			path = $3
			gsub(/^"|"$/, "", path)
			keep = path == "tests/detect_path_test.c" ||
				path == "tests/../detect.c"
			if (keep)
				print
			next
		}
		keep { print }
	' "$preprocessor_output" > "$configured_source"
	configured_detect_origin_valid "$configured_source"
}

# detect_path_test.c compiles into two executables under opposite values of
# TEST_DRM_BUS_DISCOVERY.  The compiler produces one configured project-origin
# translation unit per executable, so each graph carries the branches and
# interposed calls its binary contains.
legacy_detect_source=$output_dir/configured-detect-path-test.c
modern_detect_source=$output_dir/configured-detect-drm-discovery-test.c
preprocessor_output=$(mktemp "${TMPDIR:-/tmp}/radeontop-detect-preprocessor.XXXXXX")
detect_compiler_flags=
for package in libdrm pciaccess; do
	package_flags=$(pkg-config --cflags "$package") || {
		echo "required pkg-config module is unavailable: $package" >&2
		exit 1
	}
	detect_compiler_flags="$detect_compiler_flags $package_flags"
done
{
	printf '%s\n' 'legacy=cc -E -std=gnu11 -UTEST_DRM_BUS_DISCOVERY -Iinclude -I../generated/include [pkg-config libdrm pciaccess] tests/detect_path_test.c'
	printf '%s\n' 'modern=cc -E -std=gnu11 -DTEST_DRM_BUS_DISCOVERY=1 -Iinclude -I../generated/include [pkg-config libdrm pciaccess] tests/detect_path_test.c'
	printf 'resolved_pkg_config_flags=%s\n' "$detect_compiler_flags"
} > "$output_dir/configured-detect-preprocessor-command.txt"
project_origin_preprocess -UTEST_DRM_BUS_DISCOVERY "$legacy_detect_source" \
	"$output_dir/configured-detect-path-test-preprocessor.stderr"
project_origin_preprocess -DTEST_DRM_BUS_DISCOVERY=1 "$modern_detect_source" \
	"$output_dir/configured-detect-drm-discovery-test-preprocessor.stderr"
configured_detect_source_valid "$legacy_detect_source" \
	'initialize_pci_device' 'configure_drm_devices'
configured_detect_source_valid "$modern_detect_source" \
	'configure_drm_devices' 'initialize_pci_device'

configured_detect_bad=$output_dir/calibration-configured-detect-known-bad.c
awk '1' "$legacy_detect_source" "$modern_detect_source" \
	> "$configured_detect_bad"
if configured_detect_source_valid "$configured_detect_bad" \
	'initialize_pci_device' '__radeontop_missing_symbol__'; then
	echo "configured detect-source validator accepted a two-main union" >&2
	exit 1
fi

# Each test graph includes the production translation units compiled separately
# or textually included behind an interposition boundary.  The configured detect
# sources already contain their exact detect.c inclusion.  Adding raw detect.c
# reintroduces the branch union that preprocessing removed.
write_call_graph capture-test ./tests/capture_test.c ./capture.c \
	./collector.c ./device_model.c
write_call_graph collector-test ./tests/collector_test.c ./collector.c
write_call_graph detect-path-test "$legacy_detect_source" \
	./device_model.c ./rs480_observation.c
write_call_graph detect-drm-discovery-test "$modern_detect_source" \
	./device_model.c ./rs480_observation.c
write_call_graph device-model-test ./tests/device_model_test.c ./device_model.c
write_call_graph privileges-test ./tests/privileges_test.c ./privileges.c
write_call_graph rs480-observation-test ./tests/rs480_observation_test.c \
	./rs480_observation.c

configured_detect_graph_valid() {
	graph=$1
	lane=$2

	grep -Eq '(^|[[:space:]])main\(\)' "$graph" &&
		grep -Eq '(^|[[:space:]])test_open\(\)' "$graph" &&
		! grep -Eq '(^|[[:space:]])open\(\)' "$graph" || return 1

	case "$lane" in
		legacy)
			! grep -Eq '(^|[[:space:]])(find_drm|device_info_drm)\(\)' "$graph"
			;;
		modern)
			grep -Eq '(^|[[:space:]])find_drm\(\)' "$graph" &&
				grep -Eq '(^|[[:space:]])device_info_drm\(\)' "$graph"
			;;
		*) return 1 ;;
	esac
}

legacy_detect_graph=$output_dir/cflow-detect-path-test-forward.txt
modern_detect_graph=$output_dir/cflow-detect-drm-discovery-test-forward.txt
configured_detect_graph_valid "$legacy_detect_graph" legacy
configured_detect_graph_valid "$modern_detect_graph" modern

# Adding raw detect.c recreates the former unconfigured union.  The retained
# negative control exposes modern discovery in the legacy graph and fails the
# configured-graph predicate.
cflow --all --include=_s --number --format=gnu "$legacy_detect_source" \
	./detect.c ./device_model.c ./rs480_observation.c \
	> "$output_dir/calibration-configured-detect-graph-known-bad.txt" \
	2> "$output_dir/calibration-configured-detect-graph-known-bad.stderr"
[ ! -s "$output_dir/calibration-configured-detect-graph-known-bad.stderr" ]
grep -Eq '(^|[[:space:]])find_drm\(\)' \
	"$output_dir/calibration-configured-detect-graph-known-bad.txt"
if configured_detect_graph_valid \
		"$output_dir/calibration-configured-detect-graph-known-bad.txt" legacy; then
	echo "configured detect graph accepted a raw production union" >&2
	exit 1
fi

cp "$output_dir/source-files.txt" "$output_dir/cscope.files"
find . -type f -print | sort \
	> "$output_dir/cscope-staged-source-files.txt"
cmp -s "$output_dir/cscope-staged-source-files.txt" \
	"$output_dir/source-files.txt"
cscope -b -k -I include -i "$output_dir/cscope.files" \
	-f "$output_dir/cscope.out"
normalize_cscope_database_root "$output_dir/cscope.out"
sed -n '1p' "$output_dir/cscope.out" \
	> "$output_dir/cscope-database-header.txt"

ctags --options=NONE --languages=C --fields=+nKz --extras=+q \
	--pseudo-tags=-TAG_PROC_CWD \
	-f "$output_dir/tags" -L "$output_dir/source-files.txt" \
	2> "$output_dir/ctags.stderr"

printf '%s\n' \
	'ctags: Notice: No options will be read from files or environment' \
	'ctags: Warning: Enabling Cargo subparser may enable TOML parser.' \
	'ctags: Warning: The current implementation of the TOML parser is broken.' \
	> "$output_dir/calibration-ctags-diagnostics-known-good.txt"
ctags_diagnostics_valid \
	"$output_dir/calibration-ctags-diagnostics-known-good.txt"
printf '%s\n' 'ctags: Warning: unexpected diagnostic' \
	> "$output_dir/calibration-ctags-diagnostics-known-bad.txt"
if ctags_diagnostics_valid \
	"$output_dir/calibration-ctags-diagnostics-known-bad.txt"; then
	echo "ctags diagnostic validator accepted its negative control" >&2
	exit 1
fi
if ! ctags_diagnostics_valid "$output_dir/ctags.stderr"; then
	echo "ctags emitted an unexpected diagnostic" >&2
	sed -n '1,40p' "$output_dir/ctags.stderr" >&2
	exit 1
fi

readtags -t "$output_dir/tags" - main \
	> "$output_dir/calibration-ctags-known-good.txt"
readtags -t "$output_dir/tags" - __radeontop_missing_symbol__ \
	> "$output_dir/calibration-ctags-known-bad.txt"
grep -q 'radeontop.c' "$output_dir/calibration-ctags-known-good.txt"
[ ! -s "$output_dir/calibration-ctags-known-bad.txt" ]

(
	cd "$output_dir"
	cscope -d -L -P "$source_root" -f cscope.out -0 main \
		> calibration-cscope-known-good.txt
	cscope -d -L -P "$source_root" -f cscope.out \
		-0 __radeontop_missing_symbol__ \
		> calibration-cscope-known-bad.txt
	cscope -d -L -P "$source_root" -f cscope.out -7 '.*' \
		> cscope-all-files-query.txt
)
grep -q 'radeontop.c' "$output_dir/calibration-cscope-known-good.txt"
[ ! -s "$output_dir/calibration-cscope-known-bad.txt" ]
awk '{ print "./" $1 }' "$output_dir/cscope-all-files-query.txt" | sort -u \
	> "$output_dir/cscope-indexed-files.txt"
source_denominator_valid "$output_dir/cscope-indexed-files.txt" \
	"$canonical_sources"
printf '%s\n' \
	'schema=radeontop_source_intelligence_cscope_v1' \
	'database_root=.' \
	'indexed_denominator=exact' \
	'quick_index=false' \
	'relocation_query=true' \
	> "$output_dir/cscope-database-validation.txt"

mkdir -p "$output_dir/gtags"
gtags --sqlite3 -f "$output_dir/source-files.txt" "$output_dir/gtags"
GTAGSROOT=$source_root GTAGSDBPATH=$output_dir/gtags global -x main \
	> "$output_dir/calibration-global-known-good.txt"
GTAGSROOT=$source_root GTAGSDBPATH=$output_dir/gtags \
	global -x __radeontop_missing_symbol__ \
	> "$output_dir/calibration-global-known-bad.txt" || true
GTAGSROOT=$source_root GTAGSDBPATH=$output_dir/gtags global -P \
	> "$output_dir/global-all-files-query.txt"
grep -q 'radeontop.c' "$output_dir/calibration-global-known-good.txt"
[ ! -s "$output_dir/calibration-global-known-bad.txt" ]
sed 's#^#./#' "$output_dir/global-all-files-query.txt" | sort -u \
	> "$output_dir/global-indexed-files.txt"
source_denominator_valid "$output_dir/global-indexed-files.txt" \
	"$canonical_sources"
printf '%s\n' \
	'schema=radeontop_source_intelligence_global_v1' \
	'backend=sqlite3' \
	'indexed_denominator=exact' \
	> "$output_dir/global-database-validation.txt"

lizard -l cpp -f "$output_dir/source-files.txt" --csv \
	> "$output_dir/lizard-complexity.csv"
set --
while IFS= read -r source_file; do
	set -- "$@" "$source_file"
done < "$output_dir/source-files.txt"
scc --ci --by-file --sort name \
	--directory-walker-job-workers 1 \
	--file-process-job-workers 1 \
	--file-summary-job-queue-size 1 \
	--file-list-queue-size 1 \
	--format json "$@" \
	> "$output_dir/scc-by-file.json"

# Compiler dependencies capture preprocessing edges that lexical call tools do
# not see.  pkg-config emits trusted compiler flags for available dependencies.
set --
while IFS= read -r source_file; do
	set -- "$@" "$source_file"
done < "$output_dir/c-source-files.txt"
compiler_flags='-Iinclude -I../generated/include'
for package in pciaccess libdrm; do
	package_flags=$(pkg-config --cflags "$package") || {
		echo "required pkg-config module is unavailable: $package" >&2
		exit 1
	}
	compiler_flags="$compiler_flags $package_flags"
done
for package in libdrm_amdgpu xcb xcb-dri2 ncursesw; do
	if package_flags=$(pkg-config --cflags "$package" 2>/dev/null); then
		compiler_flags="$compiler_flags $package_flags"
	fi
done
# shellcheck disable=SC2086
compiler_dependencies_raw=$output_dir/.compiler-dependencies.raw.tmp
cc -MM $compiler_flags "$@" > "$compiler_dependencies_raw"
sed 's#\.\./generated/include/version\.h#include/version.h#g' \
	"$compiler_dependencies_raw" > "$output_dir/compiler-dependencies.mk"
rm -f "$compiler_dependencies_raw"
compiler_dependencies_raw=
grep -q 'include/version\.h' "$output_dir/compiler-dependencies.mk"
! grep -q '\.\./generated/' "$output_dir/compiler-dependencies.mk"

sed ':join
/\\$/ {
	N
	s/\\\n/ /
	b join
}' "$output_dir/compiler-dependencies.mk" |
awk '
BEGIN { print "digraph include_dependencies {" }
{
	target = $1
	sub(/:$/, "", target)
	for (field = 2; field <= NF; field++)
		printf "  \"%s\" -> \"%s\";\n", target, $field
}
END { print "}" }
' > "$output_dir/compiler-include-graph.dot"
dot -Tsvg "$output_dir/compiler-include-graph.dot" \
	-o "$output_dir/compiler-include-graph.svg"

callback_binding_pattern='(getgrbm|getsrbm2|getsrbm|getvram|getgtt|getsclk|getmclk|read_status|read_uvd_status|read_vce_status|read_sclk|read_mclk|read_vram|read_gtt|wait_until|wake)[[:space:]]*='
set --
while IFS= read -r source_file; do
	set -- "$@" "$source_file"
done < "$output_dir/c-source-files.txt"
rg --threads 1 -n --no-heading "$callback_binding_pattern" \
	-- "$@" > "$output_dir/callback-bindings.txt"
for required_binding in \
	'getsrbm2 = getsrbm2_pci;' \
	'getsrbm2 = getsrbm2_radeon;' \
	'getsrbm2 = getsrbm2_amdgpu;'; do
	grep -Fq "$required_binding" "$output_dir/callback-bindings.txt"
done

printf '%s\n' 'getsrbm2 = known_reader;' \
	> "$output_dir/calibration-callback-binding-known-good-input.txt"
printf '%s\n' 'getsrbm2(known_reader);' \
	> "$output_dir/calibration-callback-binding-known-bad-input.txt"
(
	cd "$output_dir"
	rg -n --no-heading "$callback_binding_pattern" \
		calibration-callback-binding-known-good-input.txt \
		> calibration-callback-binding-known-good.txt
	set +e
	rg -n --no-heading "$callback_binding_pattern" \
		calibration-callback-binding-known-bad-input.txt \
		> calibration-callback-binding-known-bad.txt
	missing_binding_status=$?
	set -e
	if [ "$missing_binding_status" -eq 0 ]; then
		echo "missing callback-binding mutation entered the inventory" >&2
		exit 1
	fi
	if [ "$missing_binding_status" -ne 1 ]; then
		echo "callback-binding mutation calibration failed" >&2
		exit 1
	fi
	[ ! -s calibration-callback-binding-known-bad.txt ]
)

snapshot_source_end=$output_dir/.source-snapshot-end.tmp
snapshot_generated_end=$output_dir/.generated-snapshot-end.tmp
write_source_denominator "$snapshot_source_end" \
	"$output_dir/source-files.txt" "$source_root"
write_source_denominator "$snapshot_generated_end" \
	"$generated_input_files" "$generated_snapshot_root"
cmp -s "$output_dir/source-denominator.tsv" "$snapshot_source_end"
cmp -s "$output_dir/generated-input-denominator.tsv" \
	"$snapshot_generated_end"

repository_commit_end=$(git -C "$repo_root" rev-parse HEAD)
repository_branch_end=$(
	git -C "$repo_root" symbolic-ref --quiet --short HEAD || printf detached
)
repository_status_end=$output_dir/.repository-status-end.tmp
source_denominator_end=$output_dir/.source-denominator-end.tmp
generated_denominator_end=$output_dir/.generated-denominator-end.tmp
source_state_bad=$output_dir/.source-state-known-bad.tmp
git -C "$repo_root" status --short > "$repository_status_end"
write_source_denominator "$source_denominator_end" \
	"$output_dir/source-files.txt" "$repo_root"
write_source_denominator "$generated_denominator_end" \
	"$generated_input_files" "$repo_root"
source_state_stable \
	"$repository_commit_start" "$repository_commit_end" \
	"$repository_branch_start" "$repository_branch_end" \
	"$repository_status_start" "$repository_status_end" \
	"$output_dir/source-denominator.tsv" "$source_denominator_end"
cmp -s "$output_dir/generated-input-denominator.tsv" \
	"$generated_denominator_end"

if source_state_stable \
		"$repository_commit_start" \
		'0000000000000000000000000000000000000000' \
		"$repository_branch_start" "$repository_branch_end" \
		"$repository_status_start" "$repository_status_end" \
		"$output_dir/source-denominator.tsv" "$source_denominator_end"; then
	echo "source-stability validator accepted a commit mutation" >&2
	exit 1
fi
cp "$output_dir/generated-input-denominator.tsv" "$source_state_bad"
printf '%s\n' './__radeontop_generated_input_mutation__' \
	>> "$source_state_bad"
if cmp -s "$output_dir/generated-input-denominator.tsv" \
		"$source_state_bad"; then
	echo "generated-input validator accepted a denominator mutation" >&2
	exit 1
fi
if source_state_stable \
		"$repository_commit_start" "$repository_commit_end" \
		"$repository_branch_start" \
		"${repository_branch_end}__mutation__" \
		"$repository_status_start" "$repository_status_end" \
		"$output_dir/source-denominator.tsv" "$source_denominator_end"; then
	echo "source-stability validator accepted a branch mutation" >&2
	exit 1
fi
cp "$repository_status_start" "$source_state_bad"
printf '%s\n' ' M __radeontop_status_mutation__' >> "$source_state_bad"
if source_state_stable \
		"$repository_commit_start" "$repository_commit_end" \
		"$repository_branch_start" "$repository_branch_end" \
		"$repository_status_start" "$source_state_bad" \
		"$output_dir/source-denominator.tsv" "$source_denominator_end"; then
	echo "source-stability validator accepted a status mutation" >&2
	exit 1
fi
cp "$output_dir/source-denominator.tsv" "$source_state_bad"
printf '%s\n' './__radeontop_denominator_mutation__' >> "$source_state_bad"
if source_state_stable \
		"$repository_commit_start" "$repository_commit_end" \
		"$repository_branch_start" "$repository_branch_end" \
		"$repository_status_start" "$repository_status_end" \
		"$output_dir/source-denominator.tsv" "$source_state_bad"; then
	echo "source-stability validator accepted a denominator mutation" >&2
	exit 1
fi

{
	printf 'repository=radeontop-gororoba\n'
	printf 'repository_root=.\n'
	printf 'commit=%s\n' "$repository_commit_start"
	printf 'branch=%s\n' "$repository_branch_start"
	printf 'source_file_count=%s\n' \
		"$(wc -l < "$output_dir/source-files.txt")"
	printf 'source_byte_count=%s\n' \
		"$(awk 'NR > 1 { total += $2 } END { print total + 0 }' \
		"$output_dir/source-denominator.tsv")"
	printf 'source_line_count=%s\n' \
		"$(awk 'NR > 1 { total += $3 } END { print total + 0 }' \
		"$output_dir/source-denominator.tsv")"
	printf 'source_stability=pass\n'
	printf 'analysis_snapshot=hash-verified-read-only\n'
	printf 'generated_input_snapshot=hash-verified-read-only\n'
	printf 'worktree_status_begin\n'
	awk '1' "$repository_status_start"
	printf 'worktree_status_end\n'
} > "$output_dir/repository-state.txt"
printf '%s\n' \
	'schema=radeontop_source_intelligence_source_stability_v1' \
	'commit_mutation=rejected' \
	'branch_mutation=rejected' \
	'worktree_status_mutation=rejected' \
	'source_denominator_mutation=rejected' \
	'generated_input_mutation=rejected' \
	'repository_end_state_stable=true' \
	'analysis_snapshot_stable=true' \
	'generated_input_snapshot_stable=true' \
	> "$output_dir/source-stability-validation.txt"
rm -f "$repository_status_start" "$repository_status_end" \
	"$source_denominator_end" "$generated_denominator_end" \
	"$snapshot_source_end" "$snapshot_generated_end" "$source_state_bad"

printf '%s\n' \
	'repository_root=.' \
	'output_root=.' \
	> "$output_dir/calibration-path-residue-known-good.txt"
artifact_tree_excludes_path \
	"$output_dir/calibration-path-residue-known-good.txt" "$repo_root"
artifact_tree_excludes_path \
	"$output_dir/calibration-path-residue-known-good.txt" "$output_dir"
artifact_tree_excludes_path \
	"$output_dir/calibration-path-residue-known-good.txt" \
	"$analysis_snapshot_root"

checkout_path_bad=$(mktemp \
	"${TMPDIR:-/tmp}/radeontop-checkout-path-known-bad.XXXXXX")
printf '%s\n' "$repo_root" > "$checkout_path_bad"
if artifact_tree_excludes_path "$checkout_path_bad" "$repo_root"; then
	echo "path-residue validator accepted the checkout-root mutation" >&2
	exit 1
fi
rm -f "$checkout_path_bad"
checkout_path_bad=

output_path_bad=$(mktemp \
	"${TMPDIR:-/tmp}/radeontop-output-path-known-bad.XXXXXX")
printf '%s\n' "$output_dir" > "$output_path_bad"
if artifact_tree_excludes_path "$output_path_bad" "$output_dir"; then
	echo "path-residue validator accepted the output-root mutation" >&2
	exit 1
fi
rm -f "$output_path_bad"
output_path_bad=

snapshot_path_bad=$(mktemp \
	"${TMPDIR:-/tmp}/radeontop-snapshot-path-known-bad.XXXXXX")
printf '%s\n' "$analysis_snapshot_root" > "$snapshot_path_bad"
if artifact_tree_excludes_path \
		"$snapshot_path_bad" "$analysis_snapshot_root"; then
	echo "path-residue validator accepted the snapshot-root mutation" >&2
	exit 1
fi
rm -f "$snapshot_path_bad"
snapshot_path_bad=

printf '%s\n' \
	'schema=radeontop_source_intelligence_path_residue_v1' \
	'checkout_path_mutation=rejected' \
	'output_path_mutation=rejected' \
	'snapshot_path_mutation=rejected' \
	'checkout_root_absent=true' \
	'output_root_absent=true' \
	'snapshot_root_absent=true' \
	'binary_artifacts_scanned=true' \
	> "$output_dir/path-residue-validation.txt"
if ! artifact_tree_excludes_path "$output_dir" "$repo_root" ||
	! artifact_tree_excludes_path "$output_dir" "$output_dir" ||
	! artifact_tree_excludes_path \
		"$output_dir" "$analysis_snapshot_root"; then
	echo "source-intelligence artifact contains a generating path" >&2
	rg --hidden --no-ignore -a -F -l -- "$repo_root" "$output_dir" >&2 || true
	rg --hidden --no-ignore -a -F -l -- "$output_dir" "$output_dir" >&2 || true
	rg --hidden --no-ignore -a -F -l -- \
		"$analysis_snapshot_root" "$output_dir" >&2 || true
	exit 1
fi

checksum_tmp=$(mktemp "${TMPDIR:-/tmp}/radeontop-source-map-sha256.XXXXXX")
(
	cd "$output_dir"
	find . -type f ! -name SHA256SUMS -print | sort |
	while IFS= read -r artifact; do
		sha256sum "${artifact#./}"
	done
) > "$checksum_tmp"
mv "$checksum_tmp" "$output_dir/SHA256SUMS"
(
	cd "$output_dir"
	sha256sum -c SHA256SUMS >/dev/null
)

printf 'source intelligence written to %s\n' "$output_dir"

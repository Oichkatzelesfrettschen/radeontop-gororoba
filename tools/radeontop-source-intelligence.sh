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

for tool in cflow cscope ctags readtags gtags global lizard scc dot unifdef \
	git sha256sum pkg-config cc awk sed find sort wc grep rg basename; do
	command -v "$tool" >/dev/null 2>&1 || {
		echo "required tool is unavailable: $tool" >&2
		exit 1
	}
done

cd "$repo_root"
export LC_ALL=C

find . -type f \( -name '*.c' -o -name '*.h' \) \
	! -path './.git/*' -print | sort > "$output_dir/source-files.txt"

[ -s "$output_dir/source-files.txt" ] || {
	echo "source denominator is empty" >&2
	exit 1
}

find . -maxdepth 1 -type f -name '*.c' -print | sort \
	> "$output_dir/runtime-source-files.txt"
[ -s "$output_dir/runtime-source-files.txt" ] || {
	echo "runtime source denominator is empty" >&2
	exit 1
}

{
	printf 'path\tbytes\tlines\tsha256\n'
	while IFS= read -r source_file; do
		bytes=$(wc -c < "$source_file")
		lines=$(wc -l < "$source_file")
		digest=$(sha256sum "$source_file" | awk '{print $1}')
		printf '%s\t%s\t%s\t%s\n' "$source_file" "$bytes" "$lines" "$digest"
	done < "$output_dir/source-files.txt"
} > "$output_dir/source-denominator.tsv"

{
	printf 'repository=%s\n' "$repo_root"
	printf 'commit=%s\n' "$(git rev-parse HEAD)"
	printf 'branch=%s\n' "$(git symbolic-ref --quiet --short HEAD || printf detached)"
	printf 'source_file_count=%s\n' "$(wc -l < "$output_dir/source-files.txt")"
	printf 'source_byte_count=%s\n' "$(awk 'NR > 1 { total += $2 } END { print total + 0 }' "$output_dir/source-denominator.tsv")"
	printf 'source_line_count=%s\n' "$(awk 'NR > 1 { total += $3 } END { print total + 0 }' "$output_dir/source-denominator.tsv")"
	printf 'worktree_status_begin\n'
	git status --short
	printf 'worktree_status_end\n'
} > "$output_dir/repository-state.txt"

{
	cflow --version | sed -n '1p'
	cscope -V 2>&1 | sed -n '1p'
	ctags --version | sed -n '1p'
	readtags --version | sed -n '1p'
	gtags --version | sed -n '1p'
	lizard --version
	scc --version
	dot -V 2>&1
	unifdef -V 2>&1 | sed -n '1p'
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

set --
while IFS= read -r source_file; do
	set -- "$@" "$source_file"
done < "$output_dir/runtime-source-files.txt"
write_call_graph runtime "$@"

configured_detect_source_valid() {
	configured_source=$1
	required_symbol=$2
	excluded_symbol=$3

	[ "$(grep -c '^int main(void)' "$configured_source")" -eq 1 ] &&
		grep -Fq "$required_symbol" "$configured_source" &&
		! grep -Fq "$excluded_symbol" "$configured_source"
}

# detect_path_test.c compiles into two executables under opposite values of
# TEST_DRM_BUS_DISCOVERY.  A raw lexical union has two main definitions and
# cannot represent either executable.  Line-preserving projections keep each
# call graph bound to the same conditional lane that the Makefile compiles.
legacy_detect_source=$output_dir/configured-detect-path-test.c
modern_detect_source=$output_dir/configured-detect-drm-discovery-test.c
unifdef -b -x2 -UTEST_DRM_BUS_DISCOVERY ./tests/detect_path_test.c \
	> "$legacy_detect_source"
unifdef -b -x2 -DTEST_DRM_BUS_DISCOVERY ./tests/detect_path_test.c \
	> "$modern_detect_source"
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
# or textually included behind an interposition boundary.  A test-only graph
# hides the path from the harness into the contract under test and therefore is
# not a complete executable call map.
write_call_graph capture-test ./tests/capture_test.c ./capture.c \
	./collector.c ./device_model.c
write_call_graph collector-test ./tests/collector_test.c ./collector.c
write_call_graph detect-path-test "$legacy_detect_source" ./detect.c \
	./device_model.c ./rs480_observation.c
write_call_graph detect-drm-discovery-test "$modern_detect_source" ./detect.c \
	./device_model.c ./rs480_observation.c
write_call_graph device-model-test ./tests/device_model_test.c ./device_model.c
write_call_graph privileges-test ./tests/privileges_test.c ./privileges.c
write_call_graph rs480-observation-test ./tests/rs480_observation_test.c \
	./rs480_observation.c

sed "s#^\./#$repo_root/#" "$output_dir/source-files.txt" \
	> "$output_dir/cscope.files"
cscope -b -q -k -I "$repo_root/include" \
	-i "$output_dir/cscope.files" -f "$output_dir/cscope.out"

ctags --options=NONE --languages=C --fields=+nKz --extras=+q \
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

cscope -d -L -f "$output_dir/cscope.out" -0 main \
	> "$output_dir/calibration-cscope-known-good.txt"
cscope -d -L -f "$output_dir/cscope.out" -0 __radeontop_missing_symbol__ \
	> "$output_dir/calibration-cscope-known-bad.txt"
grep -q 'radeontop.c' "$output_dir/calibration-cscope-known-good.txt"
[ ! -s "$output_dir/calibration-cscope-known-bad.txt" ]

mkdir -p "$output_dir/gtags"
gtags -C "$repo_root" -f "$output_dir/source-files.txt" "$output_dir/gtags"
GTAGSROOT=$repo_root GTAGSDBPATH=$output_dir/gtags global -x main \
	> "$output_dir/calibration-global-known-good.txt"
GTAGSROOT=$repo_root GTAGSDBPATH=$output_dir/gtags \
	global -x __radeontop_missing_symbol__ \
	> "$output_dir/calibration-global-known-bad.txt" || true
grep -q 'radeontop.c' "$output_dir/calibration-global-known-good.txt"
[ ! -s "$output_dir/calibration-global-known-bad.txt" ]

lizard -l cpp -f "$output_dir/source-files.txt" --csv \
	> "$output_dir/lizard-complexity.csv"
set -- ./*.c ./include/*.h ./tests/*.c
scc --ci --by-file --format json "$@" \
	> "$output_dir/scc-by-file.json"

# Compiler dependencies capture preprocessing edges that lexical call tools do
# not see.  pkg-config emits trusted compiler flags for available dependencies.
set -- ./*.c ./tests/*.c
compiler_flags=-Iinclude
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
cc -MM $compiler_flags "$@" \
	> "$output_dir/compiler-dependencies.mk"

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
rg -n --no-heading "$callback_binding_pattern" \
	-- ./*.c ./tests/*.c > "$output_dir/callback-bindings.txt"
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

checksum_tmp=$(mktemp "${TMPDIR:-/tmp}/radeontop-source-map-sha256.XXXXXX")
trap 'rm -f "$checksum_tmp"' 0 1 2 15
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

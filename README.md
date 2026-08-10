# RadeonTop

RadeonTop samples Radeon engine-status registers and reports total GPU activity
plus the hardware blocks whose status bits have an evidence-backed mapping.
This fork adds the RS400, RS480, RS482, and RS485 read path and a versioned dump
format for replayable measurements.

![RadeonTop terminal display](https://user-images.githubusercontent.com/11575/67134324-fdec5300-f211-11e9-8597-394d9c062fe7.png)

## Supported hardware and read paths

- R300-class integrated graphics (`RS400`, `RS480`, `RS482`, and `RS485`) read
  `RBBM_STATUS` at BAR-relative offset `0x00000e40` through PCI `resource2`.
  The radeon read-register ioctl rejects RBBM_STATUS on pre-R600 parts, so detection
  selects the direct path automatically.
- R600 and later use `GRBM_STATUS` through the radeon or amdgpu DRM interface
  when that interface supplies the status word.
- `-m` forces the family-classified PCI `resourceN` path. An unknown PCI ID has
  no admitted BAR layout and fails before a PCI resource open or `mmap`.

The RS4xx total activity gauge follows `RBBM_STATUS.GUI_ACTIVE`. Per-block
gauges appear only for fields supported by the register decode and bounded
target observations. The [RS4xx read-path analysis](docs/rs4xx-engine-busy-read-path.md)
states the evidence and the remaining hardware uncertainty.

DRM access follows the selected device node's permissions. Direct MMIO needs
permission to open the exact PCI resource. In a setuid-root deployment invoked
by an ordinary user, RadeonTop drops effective privilege during option parsing
and device discovery, raises it only for the validated resource open and map,
and removes the saved root identity before sampling or output. `make install`
installs mode `0755`; deployment policy owns any setuid mode.

## Running

Automatic discovery skips unknown AMD PCI IDs and selects the first supported
GPU. An explicit DRM path remains bound to its exact PCI device, and an unknown
explicit ID fails instead of selecting another GPU:

```sh
./radeontop
```

The bus selector takes one hexadecimal PCI bus number:

```sh
./radeontop -b 0f
```

An explicit DRM path selects one exact device. `-m` can accompany it and forces
the validated direct-MMIO status path for that same PCI BDF:

```sh
./radeontop -p /dev/dri/renderD128 -m
```

Dump mode writes one window per line. `-` means stdout exactly; a name such as
`-capture.log` remains a regular file path.

```sh
./radeontop -d - -t 120 -i 1
```

Every dump invocation starts with a `radeontop_capture_v1` JSON header. It binds
the readable version to one clean Git object, exact production source-input
manifest and digest, exact build manifest and digest, boot, device, and command
bytes. A Git checkout derives `clean` from Git. An exported tree derives
`clean` only when the caller supplies the independently retained SHA-256 of its
complete source baseline. The build manifest records that admitted digest.
The authenticated baseline also binds the generated export metadata, so a
caller cannot substitute another valid-looking version or Git object.
Dump mode rejects dirty and unknown source identities because no immutable
object reconstructs their bytes. The same canonical manifests install under
`/usr/share/radeontop/`, and their byte encoding in the header is `byte-u00xx`.
Each data line retains its legacy human-readable fields and ends with an
`evidence_v2` JSON object containing the run UUID, exact timing, attempted and
missed slots, capabilities, per-signal validity, clock means, endpoint states,
lane denominators, and unconditional missing-data bounds. A
`radeontop_run_end_v2` footer records the logical exit reason, last observed
committed generation, and typed device, clock, or schedule terminal cause. The
process holds an exclusive lock for a regular output file,
including redirected stdout. A nonempty regular destination receives a newline
record boundary before the next header, so a truncated prior record cannot
absorb the new run identity. Runtime diagnostics use stderr, and a generation
gap makes the capture fail.

`--dither-seed N` selects a reproducible within-slot phase schedule. The
generator uses rejection sampling, so every nanosecond in each exact rational
slot has equal probability. Omitting the option preserves the exact deadline
grid.

The complete option contract is available from the binary and the man page:

```sh
./radeontop --help
man ./radeontop.1
```

## Building and validation

The build requires a C11 compiler, GNU make, pkgconf, libdrm, libpciaccess,
ncurses, gettext when translations are enabled, and libxcb when XCB support is
enabled. The JSON schema gate under `make check` uses Python 3 from the standard
library only; `PYTHON` selects that interpreter.

The CLI and generated-man parity gate requires AsciiDoc 10.2.1 or newer, a
DocBook XML 4.5 catalog, `xmllint`, and `xsltproc`. AsciiDoc 10.2.0 emits Python
`SyntaxWarning` diagnostics on current interpreters before rendering the same
source, so the gate rejects that version instead of masking its diagnostics.

```sh
make
make check
make check-build-identity
make check-cli-docs
make check-dist
make check-test-dependencies
make install PREFIX=/usr DESTDIR=./staging
```

`make dist` exports committed `HEAD` without changing the worktree. The archive
carries its source object and version in a generated Makefile fragment, retains
the identity recipe for the downstream compiler and flags, and writes a
deterministic gzip stream, its SHA-256 sidecar, and a separate source-baseline
SHA-256 sidecar. The archive-internal baseline has no authority by itself, so an
unanchored exported build reports `SOURCE_STATE=unknown`. `DIST_OUTPUT_DIR`
selects the explicit destination.

```sh
make dist DIST_OUTPUT_DIR=./dist-output
```

A clean exported build receives `SOURCE_BASELINE_SHA256` from a digest retained
outside the extracted tree. The source-baseline sidecar supplies that value only
after its digest is retained through an independent channel. The `.tgz.sha256`
manifest hashes both the archive and the source-baseline sidecar and publishes
last, so an independently retained copy authenticates the complete pair. A
separately retained archive digest also authenticates the archive-internal
baseline, whose SHA-256 the caller then passes explicitly.

```sh
(cd ../dist-output && sha256sum -c radeontop-VERSION.tgz.sha256)
baseline_sha256=$(awk '{print $1}' \
	../radeontop-VERSION.source-baseline.sha256)
make SOURCE_BASELINE_SHA256="$baseline_sha256"
```

The pair-manifest check runs before the source-baseline sidecar is read. The
caller retains the pair manifest through an independent release channel.

Build options take `1` to enable a lane:

```text
nls     translations, default on
debug   debug symbols, default off
nostrip disable Makefile stripping, default off
plain   apply neither the Makefile's -g nor -s
xcb     unprivileged Xorg authentication, default on
amdgpu  amdgpu reporting, auto-detected from libdrm_amdgpu
```

For example:

```sh
make amdgpu=1 xcb=1 plain=1
```

The source-intelligence target derives one exact tracked C/H denominator and
creates bounded forward and reverse cflow maps for the runtime and every linked
test path, including compiler-preprocessed project-origin translation units for
both `TEST_DRM_BUS_DISCOVERY` configurations. It also creates cscope, Ctags, and
GNU Global indexes, compiler dependency graphs, complexity reports, tool
identities, calibration results, and a verified SHA-256 manifest. The ignored
generated `include/version.h` stays outside the tracked denominator and enters
only the compiler include graph as a regenerated build dependency. The
destination must be empty. Retained artifacts use repository-relative source
paths and reject both the generating checkout path and destination path across
text and binary products. The cscope database carries a fixed `.` root and uses
`cscope -d -P SOURCE_ROOT` when queried from a retained bundle. Every analyzer
reads one hash-verified, read-only snapshot of the tracked source denominator;
the compiler dependency graph also reads a separately hashed generated-header
overlay.

```sh
source_intelligence_dir=$(mktemp -d)
make source-intelligence SOURCE_INTELLIGENCE_DIR="$source_intelligence_dir"
```

The [architecture and evidence model](docs/architecture-and-evidence.md)
separates source, build, runtime, and silicon claims. The [open-work ledger](docs/open-work.md)
names each unresolved gate and its falsifier.

## Translations

Translation work uses the upstream Launchpad project:

https://translations.launchpad.net/radeontop

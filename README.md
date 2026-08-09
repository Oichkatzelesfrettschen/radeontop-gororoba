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
  no admitted BAR layout and fails before `open` or `mmap`.

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

RadeonTop selects the first supported GPU:

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
bytes. Dump mode rejects dirty and unknown source identities because no
immutable object can reconstruct their bytes. The same canonical manifests
install under `/usr/share/radeontop/`, and their byte encoding in the header is
`byte-u00xx`.
Each data line retains its legacy human-readable fields and ends with an
`evidence_v1` JSON object containing the run UUID, exact timing, attempted and
missed slots, capabilities, per-signal validity, clock means, endpoint states,
lane denominators, and unconditional missing-data bounds. A
`radeontop_run_end_v1` footer records the logical exit reason and the final
collector state. The process holds an exclusive lock for a regular output file,
including redirected stdout, and a generation gap makes the capture fail.

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

```sh
make
make check
make check-build-identity
make check-cli-docs
make install PREFIX=/usr DESTDIR=/path/to/staging
```

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

The source-intelligence target creates bounded forward and reverse cflow maps
for the runtime and every linked test path, cscope, Ctags, and GNU Global
indexes, compiler dependency graphs, complexity reports, tool identities,
calibration results, and a verified SHA-256 manifest. The destination must be
empty.

```sh
make source-intelligence SOURCE_INTELLIGENCE_DIR=/path/to/empty/output
```

The [architecture and evidence model](docs/architecture-and-evidence.md)
separates source, build, runtime, and silicon claims. The [open-work ledger](docs/open-work.md)
names each unresolved gate and its falsifier.

## Translations

Translation work uses the upstream Launchpad project:

https://translations.launchpad.net/radeontop

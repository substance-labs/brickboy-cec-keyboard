# Reproducible BrickBoy build

This directory contains the helper source, the exact statically linked libCEC
and p8-platform sources, and the Raspberry Pi development headers needed by
the RPi CEC adapter backend. `third_party/SOURCES.json` records the upstream
commits, Git tree identifiers, archive hashes, and license locations.

## Prerequisites

Build natively on an ARM64 Raspberry Pi running Debian 12 Bookworm (Raspberry
Pi OS or the BrickBoy image). Install the toolchain and the libraries libCEC
links against:

```sh
sudo apt-get install cmake g++ make pkg-config libudev-dev libx11-dev libxrandr-dev
```

The build also links against three Raspberry Pi firmware libraries:
`libbcm_host.so.0`, `libvchiq_arm.so.0`, and `libvcos.so.0`. They come from the
`libraspberrypi0` package in the Raspberry Pi OS archive; the reference build
used `libraspberrypi0:arm64 1:2+git20231018~131943+3c97f76-1`, and the SHA-256
of each library is listed under `runtime_inputs` in `third_party/SOURCES.json`.
Point the build at the directory that holds them with `RPI_RUNTIME_LIB_DIR`:

```sh
sudo apt-get install libraspberrypi0
dpkg -L libraspberrypi0 | grep libbcm_host.so.0   # shows the library directory
```

When the helper is built inside the BrickBoy repository, `RPI_RUNTIME_LIB_DIR`
defaults to the copies BrickBoy ships in
`brickboy-home-folder/brickboy/program/brickboy-cec-keyboard/lib/rpi`. Outside
that repository it must be set.

## Build

```sh
RPI_RUNTIME_LIB_DIR=/path/to/rpi/libs ./build-brickboy.sh
```

The script copies p8-platform, libCEC, the Raspberry Pi headers, and the three
runtime libraries into a fresh temporary workspace, builds p8-platform and
static libCEC there, and then builds the helper. It sets a fixed source epoch,
remaps temporary source paths, and fails if the output has an ELF
RPATH/RUNPATH. Nothing outside `OUTPUT_DIR` is written in the source tree.

Optional environment variables:

| Variable | Default | Purpose |
| --- | --- | --- |
| `RPI_RUNTIME_LIB_DIR` | BrickBoy repository copy (see above) | Directory with the three Raspberry Pi libraries |
| `OUTPUT_DIR` | `./build` | Where the finished binary is installed |
| `JOBS` | `2` | Parallel build jobs |
| `SOURCE_DATE_EPOCH` | `1782562569` | Timestamp embedded in the build |
| `KEEP_BUILD_WORK_DIR` | `0` | Set to `1` to keep the temporary workspace for inspection |

The output is `build/brickboy-cec-keyboard`, and the script prints its SHA-256.

## Reproducibility

`SOURCE_DATE_EPOCH` defaults to `1782562569`, the commit timestamp of the
pinned libCEC revision. libCEC also embeds the kernel release and architecture
in its build information, so exact byte-for-byte reproduction requires the
reference environment below:

| Component | Reference version |
| --- | --- |
| Kernel | `Linux 6.1.0-rpi7-rpi-v8 aarch64` |
| `binutils` | `2.40-2` |
| `cmake` | `3.25.1-1` |
| `g++` | `4:12.2.0-3` |
| `make` | `4.3-4.1` |
| `pkg-config` | `1.8.1-1` |
| `libudev-dev` | `252.39-1~deb12u2` |
| `libx11-dev` | `2:1.8.4-2+deb12u2` |
| `libxrandr-dev` | `2:1.5.2-2+b1` |
| `libraspberrypi0` | `1:2+git20231018~131943+3c97f76-1` |

## Third-party sources

`third_party/libcec` and `third_party/p8-platform` are unmodified copies of the
upstream commits recorded in `SOURCES.json`. Upstream libCEC declares three Git
submodules (`src/platform`, `src/dotnet`, and `support`) and p8-platform
declares one (`support`). They are not used by this build and are not included,
so the vendored directories contain every upstream file but hash to a
different Git tree than the `tree` value in `SOURCES.json`. To compare against
upstream, check out the recorded `commit` without submodules and diff the files.

`third_party/raspberrypi-userland/include` holds the development headers from
the `libraspberrypi-dev` package recorded in `SOURCES.json`.

## Validate

Run the offline mapping test. It needs no CEC adapter, uinput, or sockets;
`LD_LIBRARY_PATH` is only needed when the Raspberry Pi libraries are not in the
system library path:

```sh
LD_LIBRARY_PATH="$RPI_RUNTIME_LIB_DIR" ./build/brickboy-cec-keyboard --self-test --map up=KEY_UP
```

It prints `self-test up -> KEY_UP` followed by `self-test ok`. See `README.md`
for the production mapping and hardware-run instructions.

## Verified reference artifact

Two independent clean builds on a BrickBoy in the reference environment
produced identical files on 2026-08-20. The result is byte-identical to the
helper binary installed at
`/home/brickboy/program/brickboy-cec-keyboard/bin/brickboy-cec-keyboard` by
BrickBoy releases v0.5.8-4 through v0.6.0-1:

- SHA-256: `56355ff0ecabef665478dcd154fb9cc3aa0127e4770ed95e5da0e26b0cbb9ad2`
- GNU Build ID: `69c8ef8078aacb194a19170e374476859753eb4b`
- ELF RPATH/RUNPATH: absent
- Temporary build paths in the output: absent

The verified artifact passed the complete production key-mapping self-test and
opened the real `RPI` CEC adapter. It registered the `Brickboy CEC Keyboard`
uinput device, served both Unix sockets, detected the connected TV and its
power state, and selected CEC physical address `1.0.0.0`.

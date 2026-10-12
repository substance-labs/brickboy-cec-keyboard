# brickboy-cec-keyboard

`brickboy-cec-keyboard` is a GPL-2.0-or-later helper process for turning HDMI-CEC remote-control events into a Linux virtual keyboard/controller device.

It is intentionally separate from the proprietary Brickboy application. It links libCEC, owns the CEC adapter, creates a `/dev/uinput` virtual input device named `Brickboy CEC Keyboard`, and talks to other processes over two Unix sockets:

- an event socket (`/run/brickboy-cec.sock`) that streams key, power, device, and audio observations as JSON lines;
- a control socket (`/run/brickboy-cec-control.sock`) that accepts a small set of TV commands (standby, power on, active source).

## Runtime Mapping

The helper has no built-in keymap. At least one `--map CEC_KEY=LINUX_KEY` argument is required, and the option can be repeated. Later mappings for the same CEC key replace earlier ones.

CEC keys can be given by name (`up`, `select`, `red`, …) or as a CEC user-control code in hex (`0x01`, or a bare one- or two-digit hex byte such as `2b`). Linux inputs can be given by name (`KEY_UP`, `BTN_START`, a single letter such as `x`) or as a numeric evdev code.

The BrickBoy production mapping is:

| CEC key argument | Linux input |
| --- | --- |
| `up` | `KEY_UP` |
| `down` | `KEY_DOWN` |
| `left` | `KEY_LEFT` |
| `right` | `KEY_RIGHT` |
| `select`, `enter` | `KEY_X` |
| `exit`, `samsung_return` | `KEY_Z` |
| `play`, `pause`, `play_function`, `pause_play_function` | `KEY_MENU` |
| `rewind`, `backward` | `BTN_SELECT` |
| `fast_forward`, `forward` | `BTN_START` |
| `red` | `KEY_MENU` |
| `green` | `BTN_START` |
| `yellow` | `BTN_SELECT` |
| `blue` | `BTN_MODE` |

Consumers should read normal Linux input events from the virtual device for controls and use the event socket only for device/power/audio metadata.

## Build

The complete source for the helper and its statically linked dependencies
(libCEC and p8-platform) is in this directory. Build it natively on an ARM64
Raspberry Pi running Debian 12 Bookworm:

```sh
./build-brickboy.sh
```

See `BUILD.md` for system prerequisites, the Raspberry Pi runtime libraries
the build links against, dependency provenance, reproducibility controls, and
validation commands.

## Run

The helper links dynamically against the Raspberry Pi firmware libraries
`libbcm_host.so.0`, `libvchiq_arm.so.0`, and `libvcos.so.0`. If they are not in
the system library path, point `LD_LIBRARY_PATH` at the directory that holds
them (the same directory passed to the build as `RPI_RUNTIME_LIB_DIR`) for
every command below.

For an offline mapping test that needs no CEC adapter, uinput, or sockets:

```sh
./build/brickboy-cec-keyboard --self-test \
  --map up=KEY_UP \
  --map down=KEY_DOWN \
  --map left=KEY_LEFT \
  --map right=KEY_RIGHT \
  --map select=KEY_X \
  --map enter=KEY_X \
  --map exit=KEY_Z \
  --map samsung_return=KEY_Z \
  --map play=KEY_MENU \
  --map pause=KEY_MENU \
  --map play_function=KEY_MENU \
  --map pause_play_function=KEY_MENU \
  --map rewind=BTN_SELECT \
  --map backward=BTN_SELECT \
  --map fast_forward=BTN_START \
  --map forward=BTN_START \
  --map red=KEY_MENU \
  --map green=BTN_START \
  --map yellow=BTN_SELECT \
  --map blue=BTN_MODE
```

It prints one `self-test <cec> -> <linux>` line per mapping and ends with
`self-test ok`.

For a standalone daemon test, libCEC can autodetect the adapter and attempt to
choose an HDMI physical address. Root (or equivalent access to `/dev/uinput`,
the CEC device, and `/run`) is required:

```sh
sudo ./build/brickboy-cec-keyboard --activate-source \
  --map up=KEY_UP \
  --map down=KEY_DOWN \
  --map left=KEY_LEFT \
  --map right=KEY_RIGHT \
  --map select=KEY_X \
  --map enter=KEY_X \
  --map exit=KEY_Z \
  --map samsung_return=KEY_Z \
  --map play=KEY_MENU \
  --map pause=KEY_MENU \
  --map play_function=KEY_MENU \
  --map pause_play_function=KEY_MENU \
  --map rewind=BTN_SELECT \
  --map backward=BTN_SELECT \
  --map fast_forward=BTN_START \
  --map forward=BTN_START \
  --map red=KEY_MENU \
  --map green=BTN_START \
  --map yellow=BTN_SELECT \
  --map blue=BTN_MODE
```

To override the port or address explicitly:

```sh
sudo ./build/brickboy-cec-keyboard --activate-source --hdmi-port 2 --map up=KEY_UP
sudo ./build/brickboy-cec-keyboard --activate-source --physical-address 2.0.0.0 --map up=KEY_UP
```

Those address examples use a minimal one-key map only to show the option
syntax. Use the full repeated `--map` list above for normal operation.
Prefer `--hdmi-port` for top-level inputs: the Raspberry Pi libCEC adapter can
reset an explicit `--physical-address` to `1.0.0.0`. A nested physical address
may still require `--physical-address`, but verify the address libCEC actually
registers rather than trusting the argument alone.

Use `--dry-run` to log input events instead of opening `/dev/uinput`, and
`--verbose` to print every event and libCEC log line to stdout.

### How BrickBoy runs it

On BrickBoy devices the helper runs as a systemd service through a wrapper
script that supplies the production mapping above, selects the `RPI` adapter
with `--port RPI`, and sets `--release-timeout 1000`. Before each start, a
BrickBoy script reads the HDMI physical address from the firmware EDID and
passes the top-level input to the helper as `--hdmi-port`. The service then
restricts both sockets to mode `0660`. The wrapper, service unit, and EDID
script are part of the BrickBoy system, not this helper, and are not needed to
build or run it.

## HDMI Address Detection

When no explicit `--physical-address` or `--hdmi-port` is supplied, the helper
chooses a CEC physical address in this order:

1. EDID HDMI vendor block physical address from `/sys/class/drm/*HDMI*/edid`
2. libCEC/RPi-reported physical address after opening the adapter
3. top-level probe candidates `1.0.0.0` through `4.0.0.0`

The selected address is retained on the event socket:

```json
{"type":"cec_address","action":"selected","physical_address":"2000","source":"edid:card0-HDMI-A-1"}
```

If EDID is unavailable and only libCEC's address is known, the helper can probe the common top-level HDMI paths by briefly sending Active Source for each candidate. It stops probing as soon as any CEC keypress reaches the helper.

DRM EDID can be empty when the video mode is forced, even though the firmware
still knows the EDID; that is why BrickBoy passes `--hdmi-port` explicitly
instead of relying on this fallback. Address probing and `--activate-source`
can switch or wake the TV, so do not use them as passive status checks.

## Options

```text
--map CEC=LINUX              Map CEC remote key to Linux input key; repeatable; required
--port PORT                  CEC adapter port, default autodetect (BrickBoy uses RPI)
--hdmi-port PORT             Override autodetected HDMI port, 1-15
--physical-address ADDR      Override autodetected CEC physical address, e.g. 2000 or 2.0.0.0
--activate-source            Ask libCEC to become the active playback source on startup
--activate-source-delay MS   Delay before the startup active-source request, default 2000
--probe-addresses            Probe CEC physical addresses even when EDID is present
--no-address-probe           Disable physical-address probing
--address-probe-interval MS  Delay between probe candidates, default 8000
--release-timeout MS         Release stuck CEC keys after this timeout, -1 disables, default -1
--socket PATH                Event socket path, default /run/brickboy-cec.sock
--no-socket                  Disable the event socket
--control-socket PATH        Control socket path, default /run/brickboy-cec-control.sock
--no-control-socket          Disable the control socket
--dry-run                    Log virtual input instead of opening /dev/uinput
--self-test                  Run a synthetic mapping test and exit; implies --dry-run and no sockets
--verbose                    Print JSON events and libCEC logs to stdout
-h, --help                   Show usage
```

## Event Socket

`/run/brickboy-cec.sock` is a read-only stream of JSON lines. Anything a client
writes to it is ignored. New clients immediately receive the retained `ready`,
`display`, and `cec_address` events when available, then future events as they
happen.

| `type` | Meaning |
| --- | --- |
| `ready` | The helper finished starting (retained) |
| `key` | Mapped key `down`, `repeat`, or `up` (with a `reason` on release), or `unmapped` with the raw `cec_code` |
| `raw_keypress` | libCEC key callback with `cec_code` and `duration_ms` |
| `power` | `<Standby>` or `<Report Power Status>` from a device |
| `display` | Latest TV snapshot: `connected`, `power_state`, `vendor_id`, and the `reason` it changed (retained) |
| `device` | Vendor ID or physical address reported by a device |
| `audio` | System audio mode reported by a device |
| `active_source` | An `<Active Source>` seen on the bus (`reason: bus`) or returned by `get_active_source` (`reason: query`) |
| `cec_address` | Physical address `selected`, `probing`, or `restored` after probing, and where it came from (retained) |
| `menu_state` | libCEC menu state `activated` or `deactivated` |
| `alert` | libCEC adapter alert |
| `control` | Result of a control-socket command (see below) |
| `raw` | Every received CEC frame with opcode and parameters |
| `libcec_log` | libCEC log line, only with `--verbose` |

Examples:

```json
{"type":"key","action":"down","cec_key":"up","cec_code":"01","linux_key":"KEY_UP"}
{"type":"power","source":"tv","state":"standby"}
{"type":"device","logical":4,"role":"playback1","vendor_id":"080046"}
{"type":"display","source":"tv","connected":true,"power_state":"on","vendor_id":"0000f0","reason":"report_power_status"}
{"type":"audio","source":"audio","system_audio_mode":"off"}
{"type":"raw","from":5,"to":15,"from_name":"audio","to_name":"broadcast","opcode":"72","opcode_name":"set_system_audio_mode","params":["00"]}
```

An external process can use the latest `display.connected` and
`display.power_state` values to decide whether HDMI audio should be enabled.

## Control Socket

`/run/brickboy-cec-control.sock` accepts one command per line, either as a bare
name or as JSON `{"cmd":"..."}`, and answers each line with one JSON line. A
client may send several commands on one connection.

```sh
printf '{"cmd":"standby_tv"}\n' | nc -N -U /run/brickboy-cec-control.sock
{"type":"control_response","cmd":"standby_tv","ok":true,"message":"sent standby to tv"}
```

| Command | Aliases | Effect |
| --- | --- | --- |
| `standby_tv` | `tv_standby`, `standby`, `power_off_tv`, `power_off` | Send `<Standby>` to the TV |
| `power_on_tv` | `tv_power_on`, `power_on`, `on` | Wake the TV through libCEC |
| `active_source` | `activate_source`, `as` | Broadcast `<Active Source>` for this device (One Touch Play: can wake the TV and switch its input) |
| `get_active_source` | `query_active_source`, `request_active_source` | Broadcast `<Request Active Source>` and wait up to 500 ms for a reply |

`get_active_source` returns `ok:true` with the message `reported active source`
when a device answered (the answer is published on the event socket as an
`active_source` event), or `requested active source; no response` when nothing
answered in time. It never reports libCEC's cached active source, which can be
stale after the TV is switched off.

libCEC skips `standby_tv` and `power_on_tv` when its cached power state for the
TV already matches the request, but still reports success. A TV that never
sends `<Report Power Status>` keeps that cache stale, for example after it is
woken by its own remote. Sending `active_source` first refreshes the cache, but
it also wakes a TV that is off, so only do that when the TV is not already
known to be off.

Every command result is also published on the event socket as a `control`
event.

The helper creates both sockets with mode `0666`. Anyone who can reach the
control socket can switch the TV off or on, so restrict it (BrickBoy changes
both sockets to `0660` with a dedicated group) or disable it with
`--no-control-socket` if nothing needs it.

## License

This helper is GPL-2.0-or-later; see `LICENSE`. libCEC and p8-platform are
GPL-2.0-or-later and the Raspberry Pi userland headers are BSD-3-Clause; their
license files are kept in their directories under `third_party/`, and
`third_party/SOURCES.json` records where each one came from.

Product distributions must include this helper's complete corresponding source, build instructions, license text, and the corresponding source for linked GPL components such as libCEC.

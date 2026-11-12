# Host tools for the Govee H6006 app

This directory holds two things that run on a computer, not on the Flipper:

- `run_tests.sh` builds and runs the tests of the app's portable core (`../core`) with the
  system C compiler.
- `govee_remote.py` controls the app on a connected Flipper over the USB serial console and
  waits for the result of each command.

## Host tests

Requirements: a POSIX shell and a C compiler that accepts `-std=gnu17` (`cc` by default; set `CC`
to use another one). No Flipper, toolchain or library is needed.

From the repository root, change to the app directory and run the script:

```sh
cd applications_user/govee_h6006
host/run_tests.sh
```

The script runs the compiler and the tests at the lowest CPU priority (`nice -n 19`) by itself.

The script compiles `core/govee_h6006.c`, `core/govee_registry.c`, `core/govee_engine.c`,
`core/govee_status.c` and the tests in `host/tests/` with `-Wall -Wextra -Werror` and more warnings,
writes the binary to `host/build/govee_core_tests`, and runs it. It prints one line per test
(`ok   <name>` or `FAIL <name>`, with the failed checks on stderr) and a summary:

```text
SUMMARY: <tests> tests, <failed> failed, <checks> checks, <failures> check failures
```

Exit code: 0 when every test passed, 1 when a test failed, and the compiler's exit code when the
build failed.

The tests also write two status file samples to `host/tests/fixtures/`
(`remote_status_sample.txt` and `remote_status_partial.txt`). `govee_remote.py selftest` reads
them, so the Python parser is checked against files produced by the same C code the app uses.
Run `host/run_tests.sh` before the first `selftest`; without these files `selftest` stops with
`FileNotFoundError`.

### How the core stays testable

`core/` contains everything that decides what the app does, and none of it depends on the
firmware:

- Portable C (gnu17): no Furi or firmware headers, no heap, no floating point, no threads.
- `govee_engine` is a state machine. It never calls the radio, a timer or the clock directly. It
  gets events through `govee_engine_on_*()` and acts through a table of functions
  (`GoveeEngineOps`: connect, cancel, discover, write, disconnect, timer start and stop, current
  time) that the caller supplies. On the Flipper, `govee_link.c` fills that table with calls to
  the BLE central HAL. In the tests, `tests/sim.c` fills it with a fake clock and a fake link that
  can be told to be unreachable, slow, silent, to refuse writes or to drop the link, and delivers
  the resulting events in time order.
- `govee_registry` reads and writes the lamp list as text; the app does the file steps.
- `govee_status` formats the status file and validates remote commands; the app writes the text
  to the SD card.
- `govee_h6006` builds the 20-byte H6006 command packets.

## govee_remote.py

Requirements: Python 3 and, for every command except `selftest`, the `pyserial` package. The
Python of the firmware toolchain has it. To put it on the `PATH`, run in the repository root
`source scripts/toolchain/fbtenv.sh` (macOS, Linux) or `scripts\toolchain\fbtenv.cmd` (Windows),
then change to the app directory, where the examples below run:

```sh
source scripts/toolchain/fbtenv.sh
cd applications_user/govee_h6006
```

Any other Python 3 works too after `python3 -m pip install pyserial`. The tool uses only
portable Python and pyserial, so it also runs on Windows; pass the port there, for example
`--port COM5`.

The app must be installed as `/ext/apps/Bluetooth/govee_h6006.fap` (or pass `--fap`), and no
other program may hold the Flipper's serial port.

```text
govee_remote.py [--port PORT] [--timeout S] [--fap PATH] [--clear] [--fixtures DIR] COMMAND [ARGS]
```

- `--port`: serial port. Without it the tool uses the only `/dev/cu.usbmodemflip_*` device it
  finds (macOS naming); with none or several it stops with exit code 4. On other systems pass the
  port explicitly.
- `--timeout`: how long, in seconds, the tool waits for the app to confirm a change, a scan or a
  status, default 30. A status the tool asked for before this time may still be read up to 3 s
  later. `open` and `close` do not use it: `open` checks once that the app runs, and `close`
  waits up to 8 s for the app to end. Each serial command has its own limit of 5 s for the
  Flipper's CLI prompt, and 3 s when the port is opened.
- `--fap`: path of the app for `open`, default `/ext/apps/Bluetooth/govee_h6006.fap`.
- `--clear`: with `events`, clear the event list after printing it.
- `--fixtures`: directory with the status file samples for `selftest`, default `tests/fixtures`
  next to the script.

The tool talks to the Flipper CLI at 230400 baud. Before every signal it checks with
`loader info` that the Govee app is the running app and refuses otherwise.

Commands:

| Command | What it does |
|---|---|
| `open` | start the app (refused if another app is running) |
| `close` | close the app; succeeds once the loader reports no running app |
| `status` | write and print the app status |
| `on`, `off` | power the targeted lamps on or off |
| `bri N` | brightness 1 to 100 |
| `ct KELVIN` | white temperature 2700 to 6500 |
| `rgb RRGGBB` | colour, hex (a leading `#` is allowed) |
| `scan` | scan for new lamps and wait until the scan has ended; fails if the scan was refused |
| `apply` | send the full state again to the targeted lamps |
| `forget N` | remove lamp N (1-based, as listed by `status`) |
| `target N` | 0 for all lamps, N for only lamp N |
| `lightshow on`, `lightshow off` | start or stop the lightshow (exactly one lamp targeted) |
| `events` | print the app's event list |
| `raw SIGNAL [HEXARG]` | send one signal from 100 to 110 |
| `selftest` | offline test of the status parser and the wait logic; needs no Flipper |

`on`, `off`, `bri`, `ct`, `rgb` and `apply` send the signal, then ask the app for its status
again and again until every targeted lamp has a final result for the new revision, or the
`--timeout` time passes. While a scan holds the command back, they keep waiting. They then print
a line `rev <n> engine=<state> fault=<fault> pass=<n>` and one line per targeted lamp with its
number, address, outcome, the phase timings (`conn`, `disc`, `write`, `dis`, `total`),
`writes`, `reused`, `rev`, `targeted` and `name`. The outcome is `pending` while the lamp has no
final result for the new revision, even if the app still reports an outcome of an earlier
revision for it.

`scan` sends the scan signal. The app starts the scan once its current lamp work has finished,
and ignores the signal while a scan waits or runs. The tool asks for the status until it shows
that no scan waits or runs, then prints the status. If the Bluetooth stack refused to start the
scan, it says so and ends with exit code 2.

Each status request carries a request number; the tool accepts only the status file that echoes
it, so a file written for an earlier request is never taken for the answer. The tool learns that
the app echoes request numbers only from an answer to its own request, not from a status file
that was already on the SD card. If no status file was on the card before the request, only a
file that echoes a number the tool sent counts as such an answer. If the app does not accept a
request with a number before it has echoed one in this run (an older app build that does not
echo them, or an app whose inbox is full), the tool sends requests without a number and matches
the answers by session and sequence number, also answers that echo the number 0. It returns to
request numbers once an answer shows that the app echoes them. While it sends requests without a
number and no status file was on the card before the request, the answer to an earlier status
request sent by hand that is still pending can be taken for the tool's own answer.

### Exit codes

- 0: done; after a change, every targeted lamp was submitted or had nothing to change.
- 1: a targeted lamp was not submitted, no lamp is targeted, nothing was sent in this app run, or
  the engine is faulted or stopped. For `selftest`: a check failed.
- 2: usage error, the Govee app is not the running app, a signal was not accepted, the app
  rejected the request (`target`, `forget`, `lightshow`, `close`), or the Bluetooth stack
  refused the scan (`scan`).
- 3: the `--timeout` time passed before the app confirmed the result.
- 4: serial port or CLI failure.

### Examples

Run these from the app directory (`applications_user/govee_h6006`).

```sh
# Offline check of the tool itself (run host/run_tests.sh once before)
python3 host/govee_remote.py selftest

# Start the app on the only connected Flipper
python3 host/govee_remote.py open

# Name the port explicitly
python3 host/govee_remote.py --port /dev/cu.usbmodemflip_XXXX status

# All lamps on, then 40 percent (waiting up to 60 s for the result), then warm white
python3 host/govee_remote.py on
python3 host/govee_remote.py --timeout 60 bri 40
python3 host/govee_remote.py ct 2700

# Only lamp 2, red
python3 host/govee_remote.py target 2
python3 host/govee_remote.py rgb FF0000

# Event list, then clear it
python3 host/govee_remote.py --clear events

# Close the app
python3 host/govee_remote.py close
```

`selftest` prints one indented line starting with `FAIL:` for each failed check, and then the
summary line `selftest: <n> checks, <failed> failed`.

The signal numbers, their arguments and the status file format are described in the app's
[README](../README.md#remote-control-over-the-usb-serial-console).

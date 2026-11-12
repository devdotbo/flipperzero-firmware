# Govee H6006 for Flipper Zero

An external app that switches and dims Govee H6006 Bluetooth LED bulbs from a Flipper Zero. It
finds the bulbs nearby, saves them on the SD card and sends power, brightness, white temperature
and colour to all saved bulbs or to one of them. It can also be driven from a computer over the
USB serial console.

The app is for people who own H6006 bulbs and are willing to run this firmware fork. It uses the
BLE central HAL of this fork (see [BLE central HAL](../../documentation/BleCentralHal.md)); it
does not run on official firmware.

## Requirements

- A Flipper Zero running the main firmware of this fork. On official firmware the loader refuses
  to start the app (official 1.5.1 reports "Missing Imports"), because the app calls functions
  that only this fork exports.
- The BLE Full radio stack on the radio coprocessor. The central functions work only with the
  Full stack. With the Light stack the app starts, but scans are refused and no lamp can be
  reached.
- One or more Govee H6006 bulbs within Bluetooth range. The scan lists every device whose
  advertised name contains `H6006`.

The fork is the branch `feat/ble-central-1.5.1` of
<https://github.com/devdotbo/flipperzero-firmware>. Clone it with its submodules:

```sh
git clone --recursive --branch feat/ble-central-1.5.1 https://github.com/devdotbo/flipperzero-firmware.git
```

The bulbs need no preparation. The app does not pair or bond with a bulb and does not use a
Govee account, so nothing has to be set up, removed or reset in the Govee Home phone app. A bulb
that is powered and advertises a name containing `H6006` is found and saved.

### The radio stack

The Flipper Zero has two cores: the main core runs the firmware, the radio coprocessor runs a
separate Bluetooth stack. Official update packages install the Light stack (the official
`fbt_options.py` selects it). On the branch `feat/ble-central-1.5.1`, `fbt_options.py` selects the
Full stack (`stm32wb5x_BLE_Stack_full_fw.bin`, type `ble_full`) instead. The firmware accepts both
the Light and the Full stack when it starts the radio.

Check which stack your device runs:

- On the Flipper: Settings, About, page "FW Version Info". The radio stack version ends in `:F`
  for Full and `:L` for Light.
- In the CLI: `info device` prints `radio.stack.type`, which is `1` for Full and `3` for Light.

Flashing the radio coprocessor is the risky step, and it is separate from flashing the main
firmware. During development of this fork, one full update that changed the radio stack left a
device unable to start until its main firmware was written again with an SWD programmer. Do it
once, and only if your device still runs the Light stack. The update package is copied over USB;
the Flipper then installs it from the SD card by itself and restarts several times while it
removes the old radio stack and writes the new one. Charge the battery first and keep the USB
cable connected until the update has finished. The key combinations for reset and recovery are
in [Key Combos](../../documentation/KeyCombo.md).

What the `fbt` targets of this repository write:

- `./fbt flash` writes the main firmware only, over an SWD probe.
- `./fbt jflash` writes the main firmware only, over a J-Link probe.
- `./fbt flash_usb` writes the main firmware only. It builds the minimal update package (no radio
  stack, no SD card resources), uploads it over USB and installs it.
- `./fbt flash_usb_full` writes the main firmware, the radio stack and the SD card resources. It
  builds the full update package with the stack named in `fbt_options.py` (Full on this fork)
  and installs it over USB.
- `./fbt updater_package` writes nothing; it builds the full update package without installing it.
- `./fbt copro_dist` writes nothing; it bundles the coprocessor binaries for qFlipper.

The updater installs the radio stack from a full package only when its version or type differs
from the stack on the device.

### Firmware size

The updater on the Flipper writes the main firmware image, `firmware.bin`, only if the image ends
below the start of the radio stack that is installed at that moment. It reads that boundary from
the Flipper during the update.

With the BLE Full stack v1.20.0 on the 1 MB part, the stack starts at `0x080CE000`, so the
largest `firmware.bin` is 843,775 bytes. With a different radio stack installed, the boundary is
different.

The full update (`flash_usb_full`, `fbt` target `updater_package`) runs an extra check while it
builds the package. It compares `firmware.dfu`, which is 309 bytes larger than `firmware.bin`,
with 843,776 bytes and refuses to build the package when `firmware.dfu` is larger. The largest
`firmware.bin` that passes is 843,467 bytes. The minimal update (`flash_usb`) does not run this
check.

Keep `firmware.bin` at or below 843,467 bytes. To build the firmware without flashing it and read
its size in bytes:

```sh
./fbt COMPACT=1 DEBUG=0
ls -l build/f7-firmware-C/firmware.bin
```

With the code of `feat/ble-central-1.5.1`, a `COMPACT=1 DEBUG=0` build is 782,608 bytes and a
build with the default options (`DEBUG=1`, `COMPACT=0`) is 812,360 bytes. The commands in this
README use `COMPACT=1 DEBUG=0`.

If `firmware.bin` is larger than 843,467 bytes, make it smaller before you flash it. A build with
`COMPACT=1 DEBUG=0` is smaller than a build with the default options.

### Going back to official firmware

Install an official release with qFlipper or the Flipper mobile app. The official update package
contains the Light stack, and the updater installs it because its type differs from the Full
stack. That writes the radio coprocessor again, with the same risk as above. After that the
Govee app no longer starts; its lamp list stays on the SD card.

Other apps keep working on this fork. The built-in apps of official 1.5.1 are unchanged. The
fork's API version is 88.4, and its API table contains every entry of the official 1.5.1 table
(88.2). The loader accepts an external app when its major API version is the same (88) and every
function it imports exists, so every app built for official 1.5.1 runs here.

## Build and install

Before you start:

- Clone the fork as described under Requirements, and run every command from the repository
  root.
- Insert an SD card in the Flipper. The USB update copies the update package to
  `/ext/update/` on the card, and the app and its lamp list are stored there too.
- The Flipper must be on and running working firmware (not in DFU or recovery mode), connected
  over USB. The tools talk to its command line over the USB serial port.
- Close qFlipper, serial terminals and anything else that holds the Flipper's serial port.
- For `flash_usb_full`, which writes the radio stack: a charged battery and a stable USB
  connection.

First time on a device that still runs the Light stack (main firmware, radio stack and SD card
resources in one update):

```sh
./fbt COMPACT=1 DEBUG=0 flash_usb_full
```

Device that already runs the Full stack (main firmware only):

```sh
./fbt COMPACT=1 DEBUG=0 flash_usb
```

Both commands build the firmware first; to build without flashing, see Firmware size.

Build the app and start it on a connected Flipper:

```sh
./fbt COMPACT=1 DEBUG=0 launch APPSRC=govee_h6006
```

Use the same `COMPACT=1 DEBUG=0` options for the app as for the firmware. `fbt` keeps one build
directory per option set (`build/f7-firmware-C` for these options, `build/f7-firmware-D` for the
defaults) and builds the app against the firmware in that directory. With the same options it
reuses the firmware you just built; with other options it first builds a second firmware
configuration.

`launch` uploads the app to `/ext/apps/Bluetooth/govee_h6006.fap` and starts it. If more than one
Flipper is connected, name the port, for example
`./fbt COMPACT=1 DEBUG=0 FLIP_PORT=/dev/cu.usbmodemflip_XXXX launch APPSRC=govee_h6006`.

To build only, use `./fbt COMPACT=1 DEBUG=0 fap_govee_h6006`; the file is then
`build/f7-firmware-C/.extapps/govee_h6006.fap`. Copy it to `apps/Bluetooth/` on the SD card. On
the Flipper the app is listed under Apps, Bluetooth, as "Govee H6006".

Each time you flash firmware from a newer checkout, build and install the app again with the
same command. The loader refuses an app whose major API version differs from the firmware's
("Outdated App" or "Outdated Firmware") and an app that imports a function the firmware does not
export ("Missing Imports").

## Use

### First start

With no saved lamp, the app starts a scan by itself. Every H6006 it sees is saved at once. The
scan ends 1.5 s after the last new lamp appeared, and after 8 s at the latest. Then the control
screen opens.

If nothing was found, the screen says "No lamps found." OK retries the scan, Back exits. Back
during a running scan stops it; with no saved lamp it also exits the app.

When lamps are saved, the app opens the control screen directly and does not scan.

The Flipper connects to a bulb only while it sends a change, and keeps the link for up to 12 s
when exactly one lamp is targeted (see Behaviour worth knowing). At other times it holds no link
to the bulb. The app cannot read a bulb's state, so it does not see changes made with the phone
app: it sends only values that differ from what it sent itself. After using the phone, press OK
on the Power row to send the Flipper's full state again.

### Control screen

| Row | Left and Right | OK |
|---|---|---|
| Power | Off or On | send the full state again |
| Brightness | 5 to 100 % in steps of 5 | send the full state again |
| Mode | White or Colour | send the full state again |
| White temp | 2700 to 6500 K in steps of 100 K; switches to White | send the full state again |
| Colour | shows the colour as `RRGGBB` | open the colour picker |
| Lightshow | Off or On; On starts the lightshow and opens its screen | nothing |
| Lamps | shows "All n" or "Only k/n" | open the lamp list |
| Find lamps | nothing | scan for new lamps |
| Result | nothing | nothing |

A change with Left or Right is sent right away. "Send the full state again" sends power,
brightness and colour to every targeted lamp, even if the app thinks the lamp already has them.

The colour picker offers Red, Green, Blue, Yellow, Cyan and Magenta. The arrows move the cursor,
OK sends the colour (and switches the lamps on, in Colour mode), Back returns.

The Result row shows `-` before anything was sent, `sending` while lamps are being updated,
`held` while a scan holds back the changes, `fault` while the Bluetooth layer recovers, and
`k/n ok` when a pass is over: k of n targeted lamps got the current state.

Back on the control screen exits the app.

### Lamp list

The first entry, "All lamps", targets every saved lamp. Below it each lamp is listed with its
number, the result of its last attempt and its name. A `*` marks the current target. Result codes:
`ok` submitted or nothing to send, `tmo` connect timed out, `conn` connect failed, `cncl` cancel
failed, `disc` the write characteristic was not found, `wr` a write was refused, `lost` the link
dropped, `abrt` aborted because the app was closing, `-` not tried yet.

OK on a lamp opens its details: address, result, and the time of the last attempt in
milliseconds (total, then connect `c`, discovery `d`, writes `w`, disconnect `x`). The buttons are
Left "Remove" (forget the lamp), OK "All" (target all lamps) and Right "Only" (target only this
lamp).

At most 16 lamps can be saved. When the list is full, a scan shows "16 lamps saved. Remove one to
add more." and does not save new lamps.

### Lightshow

Setting the Lightshow row to On opens the lightshow screen. It shows the colour just sent, the
lamp's name and "Back: stop". The lightshow switches the lamp on and cycles through eight
colours (red, orange, yellow, green, cyan, blue, violet, pink), one every 600 ms. It runs only
when exactly one lamp is targeted; otherwise the app shows "Lightshow needs exactly one lamp.
Pick one in Lamps." and the row stays Off.

Back on the lightshow screen stops the lightshow and returns to the control screen; it does not
exit the app. The lightshow also stops when the mode, white temperature or colour changes, when
more than one lamp becomes targeted, when a scan starts, or on signal 101 with argument 0
(power off). Started by remote control (signal 109), it shows the same screen.

## Behaviour worth knowing

- Opening the app sends nothing. The first change after opening sends the full state.
- Every setting goes to all saved lamps unless one lamp is targeted in the lamp list.
- The radio holds one link at a time, so lamps are updated one after another. For each lamp the
  app connects, looks up the write characteristic, sends only the values that differ from what
  it sent before, waits 150 ms and disconnects.
- With exactly one lamp targeted, the link stays open after a change and the next change reuses
  it. The app closes a kept link 12 s after it was opened, and before a scan.
- A lamp that does not answer within 3 s is skipped, and the next lamp is served. A change during
  a pass reaches lamps not yet visited; lamps already visited get it in the next pass, which
  starts right after.
- The app remembers in the lamp file whether a lamp's last attempt succeeded. Lamps that failed
  last time are visited after the others and get 1.5 s instead of 3 s to connect, and a new change
  cancels such an attempt at once.
- With power off, only the power command is sent. Brightness and colour changes made while the
  lamps are off are sent with the next power on.
- The bulb does not acknowledge writes. A result of `ok` (`submitted` in the status file) means
  the Flipper's radio accepted every write for that lamp, not that the bulb changed. If a lamp
  did not react, press OK on the Power row to send the full state again.
- If the Bluetooth layer stops responding, the app shuts the central down and starts it again by
  itself.
- While the app runs, the Flipper's own Bluetooth connection (for example to the phone app) is
  disconnected and advertising stops. When the app exits it restores the default Bluetooth
  profile.

## Files on the SD card

All files are in `/ext/apps_data/govee_h6006/`.

| File | Content |
|---|---|
| `bulbs.json` | the saved lamps |
| `bulbs.json.new` | the new lamp list, written before a valid `bulbs.json` is replaced |
| `bulbs.json.bak` | the lamp list before that save |
| `bulbs.json.corrupt`, `.corrupt.1` to `.corrupt.9` | an invalid `bulbs.json`, kept |
| `remote_status.txt` | the last status written for remote control |
| `remote_status.txt.tmp` | a status file being written |

`bulbs.json` is a JSON array with one object per lamp:

```json
[
  {
    "name": "ihoment_H6006_XXXX",
    "address": "AA:BB:CC:DD:EE:FF",
    "addr_type": 0,
    "last_seen_ts": 0,
    "ok": 1
  }
]
```

`address` is written in the byte order the radio reports, which is the reverse of the usual
notation. `ok` is 1 when the last attempt succeeded and 0 when it failed; it is missing until the
first attempt ends. The app rejects the whole file if any entry is invalid.

On start the app loads the first valid file in this order: `bulbs.json`, `bulbs.json.new`,
`bulbs.json.bak`. An invalid `bulbs.json` is copied to `bulbs.json.corrupt` (the first free name)
and never overwritten. After recovering from `.new` or `.bak`, the app writes `bulbs.json` again.

A save first reads `bulbs.json` and then takes one of two paths:

- `bulbs.json` is valid: the app writes and verifies `bulbs.json.new`, copies `bulbs.json` to
  `bulbs.json.bak` and verifies the copy, then writes and verifies `bulbs.json`. If the last step
  fails, the previous list is in `.bak` and the new one in `.new`.
- `bulbs.json` is missing or invalid: the app only writes and verifies `bulbs.json`. It leaves
  `.new` and `.bak` untouched, so a file the list was recovered from is never overwritten.

If a lamp file exists at start but cannot be read, the app turns saving off for that run so that
it never replaces a list it could not read. A save that cannot read `bulbs.json` fails without
writing any file.

## Remote control over the USB serial console

The app accepts commands as loader signals from the Flipper CLI and answers through a status
file. Open the CLI with `./fbt cli` or any serial terminal on the Flipper's USB port, then:

```text
loader open /ext/apps/Bluetooth/govee_h6006.fap
loader signal 102 32
loader signal 100 1
storage read /ext/apps_data/govee_h6006/remote_status.txt
```

The signal number is decimal, the argument is hexadecimal (`32` is brightness 50, `10` is lamp 16).
Always pass an argument. The CLI answers "was handled" when the app accepted the command into its
queue, and "was ignored" when the signal or the argument was out of range or the queue was full.

"Was handled" means only that the command is queued. The app handles its queue in order, so the
status file answers a signal 100 after every command sent before it, but the lamps are updated
after that, one after another. To read the result of a command:

1. Send `loader signal 100 <n>` with a number `n` you have not used in this app run.
2. Read `remote_status.txt`. Use it only if `req=` is that number and the last line is `end=`
   with the same number as `seq=`; otherwise wait a moment and read it again.
3. If the file shows `scanning=1`, repeat from step 1 with a new number. While a scan waits or
   runs, the app can hold a change back until the scan ends, and `desired.rev` does not show it
   yet. Evaluate the lamps only once the file shows `scanning=0`: a change is done for a lamp
   when its `lamp.<n>` line shows `rev=` at least `desired.rev` (reached), or `try=` at least
   `desired.rev` with a failure outcome in `out=` (for example `connect_timeout`). Until every
   targeted lamp is done, repeat from step 1 with a new number.

| Signal | Argument | Effect |
|---|---|---|
| 100 | any | write the status file; the argument is echoed as `req=` |
| 101 | 0 or 1 | power off or on |
| 102 | 1 to 100 (hex 1 to 64) | brightness in percent |
| 103 | 2700 to 6500 (hex A8C to 1964) | white temperature in kelvin, switches to White |
| 104 | 0 to FFFFFF | colour as RRGGBB, switches to Colour |
| 105 | 0 | scan for new lamps once the current lamp work has finished (ignored while a scan waits or runs) |
| 106 | 0 | send the full state again to all targeted lamps |
| 107 | 1 to 16 (hex 1 to 10) | forget lamp n (1-based) |
| 108 | 0 to 16 (hex 0 to 10) | target all lamps (0) or only lamp n |
| 109 | 0 or 1 | stop or start the lightshow (exactly one lamp targeted) |
| 110 | 0 | clear the event list in the status file |

A lamp number that does not exist is accepted by the CLI but ignored by the app. `loader close`
ends the app cleanly. The file `host/govee_remote.py` wraps all of this; see
[host/README.md](host/README.md).

### Status file

The app writes `remote_status.txt` only on signal 100. It writes a temporary file and moves it into
place. The format is one `key=value` per line, ASCII, `\n` line ends:

- `seq`: dump sequence number.
- `session`: marker of this app run, never 0, differs between runs.
- `req`: argument of the signal 100 this file answers.
- `uptime_ms`: Flipper tick in milliseconds.
- `app`: the screen, one of `start`, `scan`, `scan_none`, `home`, `lamps`, `lamp`, `colour`,
  `lightshow`, `notice`, `exiting`.
- `inbox_overflow`: messages lost because the app's queue was full.
- `target`: 0 for all lamps, n for only lamp n.
- `scanning`: 1 while a scan waits for the current lamp work to finish or runs, else 0.
- `scan.refused`: 1 if the Bluetooth stack refused to start the last scan, else 0. A new scan
  request sets it back to 0.
- `lightshow`: 0 or 1.
- `registry.count`, `registry.max`: saved lamps, and the limit 16.
- `registry.full`: 1 if a new lamp was refused because 16 are saved.
- `registry.load`: where the lamp list came from, one of `main`, `new`, `bak`, `empty`,
  `none_valid`, `read_error`, `probe_error`.
- `registry.save`: last save result, for example `ok` or `main write fails=1`.
- `desired.rev`: revision of the desired state; 0 means nothing was sent in this run.
- `desired.power`, `desired.brightness`, `desired.mode` (`white` or `colour`), `desired.kelvin`,
  `desired.rgb` (hex): the desired state.
- `engine.state`: one of `idle`, `connecting`, `cancelling`, `discovering`, `write_gap`, `tail`,
  `disconnecting`, `kept`, `faulted`, `stopped`.
- `engine.fault`: one of `none`, `cancel`, `disconnect`, `internal`.
- `engine.pass`, `engine.in_pass`: pass number, and 1 while a pass runs.
- `engine.pending`: 1 while a lamp list change waits for the current lamp to finish.
- `engine.targeted`: number of targeted lamps.
- `lamps`: number of `lamp.<n>` lines that follow.
- `lamp.<n>`: one line per lamp, see below.
- `events`, `events.total`: event lines that follow (at most 64), and events since start or since
  signal 110.
- `ev.<seq>`: `<t_ms> <event name> lamp=<n or -> arg=<number>`.
- `end`: the same number as `seq`.

A lamp line has these fields: `rev` (revision reached), `try` (revision last attempted), `out`
(outcome: `none`, `submitted`, `unchanged`, `connect_failed`, `connect_timeout`, `cancel_failed`,
`discovery_failed`, `write_rejected`, `link_lost`, `aborted`), `targeted`, `conn_ms`, `disc_ms`,
`write_ms`, `dis_ms`, `total_ms`, `writes`, `reused` (1 if a kept link was used), `type` (address
type), `addr`, and `name`, which is the rest of the line and may contain spaces.

The file is complete only if its last line is `end=` with the same number as `seq=`.

## Typical timings

Measured on one setup: one Flipper Zero with this firmware and two H6006 bulbs, one of them
switched off. Your numbers will differ with distance and surroundings.

- First command after opening (power on, full state, one reachable lamp): writes submitted
  685 ms after the command.
- Full state to one lamp: connect 286 ms, discovery 240 ms, three writes 251 ms, disconnect
  28 ms, 955 ms in total.
- One lamp targeted, first change: connect, discovery and one write in 437 ms; the link stays
  open. The next change on the kept link is written without a new connect.
- Lamp switched off and not marked as failed yet: given up at the 3 s connect deadline; the
  cancel took 14 ms.
- Lamp switched off and marked as failed: the reachable lamp was submitted 460 ms after the
  change, the dead lamp given up after 1.5 s.
- A change while the dead lamp was being tried: its attempt was cancelled within 13 ms, and the
  reachable lamp was submitted 337 ms after the change.

## Known limits

- One Bluetooth link at a time. Each lamp costs about one second per full-state change, plus 3 s
  (or 1.5 s) for each lamp that does not answer.
- There is no feedback from the bulb. The app cannot read the bulb's state.
- At most 16 saved lamps.
- The colour picker has six fixed colours; any colour is possible through signal 104.
- Brightness on the Flipper moves in 5 % steps; signal 102 accepts every value from 1 to 100.
- The lightshow works with exactly one targeted lamp.
- Changes made during a scan are held and sent when the scan ends.
- The Flipper's own Bluetooth connection is off while the app runs.
- If another program holds one of the lamp files open (for example a file transfer from a
  computer), the app waits until it is closed.

## Troubleshooting

**"Missing Imports", "Outdated App" or "Outdated Firmware" when the app starts.** The app and the
firmware on the device do not match. Flash this fork's main firmware, then build and install the
app again with `launch` (see Build and install).

**"Bluetooth scan refused."** The radio did not accept the scan. If it runs the Light stack (check
About as described under The radio stack), this repeats every time; install the Full stack. On
the Full stack the Bluetooth layer refused the command or was in a fault state. The status file
shows `scan.refused=1`. After a refused scan the app checks the Bluetooth layer and restarts it
if it is faulted, so press OK to retry. If it keeps repeating, close and reopen the app.

**"No lamps found."** The bulb is not powered or out of range, or its name does not contain
`H6006`. Power the bulb near the Flipper and press OK.

**Result shows `held`.** A scan is running. The change is sent when the scan ends.

**Result shows `fault`.** The Bluetooth layer did not confirm an operation in time. The app
restarts the central by itself; if the state stays, close and reopen the app.

**A lamp shows `tmo` or `conn`.** The lamp did not answer. It is tried again with the next
change. Remove it in the lamp list if it is gone for good.

**A lamp shows `ok` but did not change.** The writes were submitted, but the bulb does not
acknowledge them. Press OK on the Power row to send the full state again.

**"Saving lamps failed (...)" or "... Saving is off."** The SD card could not be written or read.
Check the card; see Files on the SD card for the recovery files.

**"16 lamps saved. Remove one to add more."** Remove a lamp in the lamp list, then scan again.

**"Lightshow needs exactly one lamp. Pick one in Lamps."** Open the lamp list, select a lamp and
press Right ("Only").

**The phone cannot connect to the Flipper.** The app turns the Flipper's own Bluetooth
connection off while it runs. Exit the app.

**`flash_usb_full` stops with "Firmware image overlaps C2 region and is not programmable!"**
`firmware.bin` is larger than 843,467 bytes, and the package was not built. Make the image
smaller (see Firmware size).

**`flash_usb` finishes, but the Flipper still runs the previous firmware.** If `firmware.bin` does
not end below the start of the installed radio stack, the updater on the Flipper does not write
it. Make the image smaller (see Firmware size).

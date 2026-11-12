# BLE central HAL

This fork adds a Bluetooth Low Energy central role to the Flipper Zero firmware. An app can scan
for advertising devices, connect to one of them, find a characteristic and write to it. The
public header is `targets/furi_hal_include/furi_hal_bt_central.h`; its 12 functions are exported
in the API table (`targets/f7/api_symbols.csv`, version 88.4, which contains every entry of the
official 88.2 table). Apps that use it run only on this fork.

The fork is the branch `feat/ble-central-1.5.1` of
<https://github.com/devdotbo/flipperzero-firmware>. The Govee H6006 app in
`applications_user/govee_h6006` is a complete user of this API.

## Requirements

The central works only with the BLE Full stack on the radio coprocessor
(`furi_hal_bt_get_radio_stack() == FuriHalBtStackFull`). The `fbt_options.py` of this fork selects
that stack for update packages; official firmware installs the Light stack. With any other stack
`furi_hal_bt_central_init()` does nothing, and every function that returns a value returns false,
NULL or 0.
See the Govee app's [README](../applications_user/govee_h6006/README.md#the-radio-stack) for how
to check the stack and which `fbt` targets write it.

## Building an app that uses the HAL

The functions are exported only by this fork's API table, so the app must be built against this
fork's SDK. The official SDK has neither the header nor the functions, so the app does not build
against it, and an app built here fails on official firmware with "Missing Imports". Two ways work:

- With `fbt` in a checkout of this fork: put the app's directory under `applications_user/` and
  build it like any external app, for example `./fbt COMPACT=1 DEBUG=0 fap_<appid>` or
  `./fbt COMPACT=1 DEBUG=0 launch APPSRC=<appid>`. Use the same options as for the firmware on
  the device.
- With [ufbt](https://pypi.org/project/ufbt/): build this fork's SDK with
  `./fbt COMPACT=1 DEBUG=0 updater_package`, which writes
  `dist/f7-C/flipper-z-f7-sdk-local.zip`, then point ufbt at it with
  `ufbt update --local=<path to that zip>` and build the app with `ufbt` as usual.
  `ufbt update --channel=release` switches back to the official SDK.

The app's `application.fam` needs nothing special for the HAL. A manifest for the example in
Minimal usage:

```python
App(
    appid="central_demo",
    name="Central demo",
    apptype=FlipperAppType.EXTERNAL,
    entry_point="central_demo_app",
    requires=["bt"],
    stack_size=4 * 1024,
    fap_category="Bluetooth",
)
```

The headers come from the SDK: `#include <furi_hal_bt_central.h>`.

## What it offers and what it does not

Offered:

- Active scanning (scan responses are requested) with one callback per distinct report.
- One connection at a time to a public or random address.
- Full discovery of primary services, characteristics and their CCCD descriptors, or a targeted
  discovery of one characteristic by its 128-bit UUID.
- Lookup of a characteristic's value handle by 128-bit UUID.
- Write without response (GATT write command).
- Subscription to notifications, and delivery of notifications.

Not offered:

- More than one link. A second `furi_hal_bt_central_connect()` is refused while a connection
  attempt or link exists.
- More than one owner. There is one central per device, with one scan callback and one connection
  callback at a time. `furi_hal_bt_central_init()` on an initialised central only logs a warning,
  and one `furi_hal_bt_central_deinit()` ends it for every user. Only one app should use it.
- Reading characteristics, write with response, indications, pairing, bonding, encryption.
- Starting an MTU exchange. The write limit is ATT_MTU - 3, which is 20 bytes unless the peer
  changed the MTU.
- Scan or connection parameters. They are fixed (see Timing), and a peer's request to change the
  connection parameters is rejected.
- Scan filters. The app filters the reports itself.

## Threading contract

- Scan and connection callbacks run on the BLE event thread or on the central's deadline worker
  thread, never on the thread that made the call.
- Connection callbacks never run concurrently with each other.
- A callback must not block. Copy what you need and post it to a queue with timeout 0, then
  handle it on your own thread.
- From a callback every function may be called except:
  - `furi_hal_bt_central_deinit()`: crashes the firmware.
  - `furi_hal_bt_central_connect()` while a scan is running or stopping: returns NULL instead of
    waiting for the scan to end.
- Two calls can block the thread that calls them, each for a bounded time. Call them from your
  own thread:
  - `furi_hal_bt_central_connect()` during a scan stops the scan and blocks up to 500 ms until it
    has ended; it returns NULL if the scan did not end. Called from a callback during a scan, it
    does not block and returns NULL at once (see above).
  - `furi_hal_bt_central_deinit()` blocks up to 1 s each for the scan, a connection attempt and an
    open link to end, then until every running callback has returned. Never call it from a
    callback.
- No other function waits for a scan, a connection or a GATT procedure to finish; their results
  arrive through the callbacks.
- `furi_hal_bt_central_init()` and `furi_hal_bt_central_deinit()` must not run at the same time as
  any other call of this API.
- Payload pointers passed to a callback (`data` of a notification) refer to the event buffer.
  Copy the bytes inside the callback.

## Scan

`furi_hal_bt_central_start_scan(callback, context)` starts a general discovery. The controller
ends it by itself after 10.24 s; `furi_hal_bt_central_stop_scan()` or
`furi_hal_bt_central_connect()` end it earlier. A new scan or connect is accepted only after the
scan has ended.

The callback gets a `FuriHalBtCentralScanResult`:

- `addr_type`: `FuriHalBtCentralAddrTypePublic` (0) or `FuriHalBtCentralAddrTypeRandom` (1).
- `addr`: 6 bytes in the order the controller reports them, least significant byte first. Pass
  them unchanged to `furi_hal_bt_central_connect()`.
- `rssi`: signal strength in dBm.
- `data_len`, `data`: the raw advertising or scan response data, up to 31 bytes. `data` is an
  array inside the struct (`uint8_t data[31]`), not a pointer, so copying the whole struct, as
  the example under Minimal usage does, copies the bytes.
- `report_type`: 0x00 `ADV_IND`, 0x01 `ADV_DIRECT_IND`, 0x02 `ADV_SCAN_IND`, 0x03
  `ADV_NONCONN_IND`, 0x04 `SCAN_RSP`, or `FURI_HAL_BT_CENTRAL_SCAN_REPORT_END` (0xFF).

The callback fires once per (address type, address, report type) in one scan, so a device's scan
response arrives separately from its advertising report; merge them by address. The table that
suppresses repeats holds 64 entries; when it is full, the oldest entry is dropped and that report
may arrive again.

Every accepted scan ends with exactly one call whose `report_type` is
`FURI_HAL_BT_CENTRAL_SCAN_REPORT_END`, with a zero address and no data. A new scan is accepted
only after it. A connect from your own thread stops a running scan itself; a connect from a
callback is refused until the scan has ended, which the end report signals.

## Connection events

The connection callback receives `FuriHalBtCentralEvent` values:

- `FuriHalBtCentralEventConnected`: the link is up. `char_handle` is 0.
- `FuriHalBtCentralEventDiscoveryComplete`: a discovery ended. `char_handle` is 0.
- `FuriHalBtCentralEventNotification`: the peer sent a notification. `char_handle` is the handle
  of the source characteristic, `data` and `len` carry the value.
- `FuriHalBtCentralEventDisconnected`: the link closed, or the attempt was cancelled. Terminal.
- `FuriHalBtCentralEventError`: `char_handle` is `FURI_HAL_BT_CENTRAL_ERROR_LINK` (terminal) or
  `FURI_HAL_BT_CENTRAL_ERROR_GATT`; see Errors.
- `FuriHalBtCentralEventWriteComplete`: never delivered, because a write command has no
  completion. Ignore it.

Every accepted `furi_hal_bt_central_connect()` gets exactly one terminal event:
`FuriHalBtCentralEventDisconnected`, or `FuriHalBtCentralEventError` with `char_handle`
`FURI_HAL_BT_CENTRAL_ERROR_LINK`. No event of that call follows it, and the connection handle is
invalid afterwards. A connection that completes while its attempt is being cancelled is closed at
once and never reported as Connected.

The one exception is `furi_hal_bt_central_deinit()`. It may end a connect call without its
terminal event. While deinit runs, the terminal event can still arrive; once deinit has returned,
no event is delivered at all. Your app then owns the cleanup of its per-connection state.

## Disconnect

`furi_hal_bt_central_disconnect(conn)` closes the link of a connect call, or cancels the attempt
if the link is not up yet. It is the way to give up a connect attempt early. It returns nothing,
does not wait, and may be called from callbacks.

The result arrives as the terminal event of that connect call:

- `FuriHalBtCentralEventDisconnected` once the link is down or the attempt was cancelled.
- `FuriHalBtCentralEventError` with `FURI_HAL_BT_CENTRAL_ERROR_LINK` if the controller does not
  confirm the cancel within 1 s or the disconnect within 6 s. The central is then faulted (see
  Faulted state and recovery).

A connection that completes while its attempt is being cancelled is closed at once and never
reported as Connected.

## Errors

The functions report failure by their return value; controller status codes go to the log only
(tags `FuriHalBtCentral`, `BleGapCentral`, `BleGattClient`).

- `start_scan`: not usable (see below), callback NULL, a scan, connection attempt or link exists,
  or the controller refused.
- `stop_scan`: not usable. It returns true when no scan was running.
- `connect`: not usable, `addr` or callback NULL, the scan did not stop within 500 ms, called from
  a callback during a scan, a connection attempt or link exists, or the controller refused.
- `gatt_discover`, `gatt_discover_char`: no link, UUID NULL, another GATT procedure runs or an
  abandoned one has not completed yet, or the controller refused.
- `gatt_write_nr`: no link, `len` above the limit, `data` NULL with `len` above 0, or the
  controller refused, also when its transmit pool is full (retry later).
- `gatt_subscribe`: no link, another GATT procedure runs, the handle is unknown or has no CCCD (a
  full discovery is needed), or the controller refused.
- `find_char`: the UUID was not found by the last discovery, or the central is not usable.
- `disconnect` has no return value. It does nothing when `conn` is not a handle returned by
  `connect`, when the central is not usable, when no connection attempt or link exists, or when
  one is already being closed. In the last case the terminal event is already on its way.

"Not usable" means: not the Full stack, not initialised, or faulted.
`furi_hal_bt_central_available()` returns true only when none of these applies.

The error event has two kinds:

- `FURI_HAL_BT_CENTRAL_ERROR_LINK` (0): the connect call is over. The attempt failed, the HAL
  cancelled it after 10 s, or the central became faulted. It is the terminal event of that call.
- `FURI_HAL_BT_CENTRAL_ERROR_GATT` (1): a discovery or subscription failed or hit its deadline.
  The link stays up.

## GATT

- `furi_hal_bt_central_gatt_discover()` runs a full discovery of primary services,
  characteristics and CCCD descriptors. It ends with DiscoveryComplete, or with Error (GATT) on
  failure or after 5 s. It keeps up to 16 services and 32 characteristics; 16-bit UUIDs are
  stored as 128-bit UUIDs on the Bluetooth base UUID.
- `furi_hal_bt_central_gatt_discover_char(conn, uuid128)` runs one discover-characteristics-by-UUID
  over the whole handle range. It ends with DiscoveryComplete (then `find_char` returns the value
  handle, or 0 if the peer has no such characteristic), or with Error (GATT) on failure or after
  3 s. Starting it clears what earlier discoveries found: afterwards only this UUID resolves, and
  `gatt_subscribe` needs a new full discovery.
- `furi_hal_bt_central_find_char(conn, uuid128)` returns the value handle after a discovery. UUIDs
  are given in the order they are written, most significant byte first: the UUID
  `00010203-0405-0607-0809-0a0b0c0d2b11` is the array `{0x00, 0x01, ..., 0x2b, 0x11}`. The HAL
  reverses the bytes for the radio.
- `furi_hal_bt_central_gatt_write_nr(conn, handle, data, len)` queues a write command. There is
  no completion event and no acknowledgement from the peer. It also works with a handle known
  from earlier, without discovery. `len` is at most ATT_MTU - 3 and at most 250.
- `furi_hal_bt_central_gatt_subscribe(conn, handle)` writes 0x0001 to the characteristic's CCCD,
  which a full discovery must have found. Success has no event; a failure arrives as Error (GATT).
  Notifications then arrive as Notification events.

One GATT procedure (discovery or subscription) runs at a time. A procedure that hit its deadline
still has a completion pending in the controller; no new procedure starts until that completion
has arrived or the link has ended.

## Timing

All values are fixed in `targets/f7/ble_glue/gap_central.h`.

- Scan interval and window: 500 ms each, so the radio listens all the time.
- Scan duration: ended by the controller after 10.24 s; the HAL stops it itself at 12 s.
- Connection interval 15 to 30 ms, latency 0, supervision timeout 2 s. The link keeps these
  values: the HAL rejects a peer's request to change them (L2CAP connection parameter update
  request).
- Connect attempt: cancelled by the HAL after 10 s, which ends in a terminal Error.
- Full discovery 5 s, targeted discovery 3 s, subscribe 3 s, then Error (GATT).
- Scan stop and connect cancel must be confirmed within 1 s, a disconnect within 6 s; otherwise
  the central becomes faulted.

## Faulted state and recovery

Every state of the central that waits for the controller has a deadline. If a scan stop, a
connect cancel or a disconnect is not confirmed in time, the controller's state is unknown and the
central becomes faulted:

- A running connect call gets its terminal Error (`FURI_HAL_BT_CENTRAL_ERROR_LINK`); a running scan
  gets its end report.
- Every call fails, and `furi_hal_bt_central_available()` returns false.
- The only way out is `furi_hal_bt_central_deinit()` followed by `furi_hal_bt_central_init()`,
  called from your own thread, not from a callback.

A link the controller reports later for an attempt that is already resolved is terminated by the
HAL and never reported to the app. A GATT deadline does not fault the central.

## Coexistence with the peripheral role

The firmware's normal Bluetooth (the phone connection, Bluetooth remote apps) uses the same radio
in the peripheral role. `furi_hal_bt_central_init()` registers an event handler with the
firmware's BLE event dispatcher and splits the controller's events by owner:

- The central consumes every successful connection-complete in the central role. It keeps the
  link only while it is connecting and the peer is the address passed to `connect`; then the
  app gets Connected. It terminates every other central-role link: one whose peer does not
  match (the connect call then ends with Error, `FURI_HAL_BT_CENTRAL_ERROR_LINK`), one that
  completes while the attempt is being cancelled, and one that arrives when no attempt is
  outstanding. A successful connection-complete in the peripheral role is left to the
  peripheral code.
- A failed connection-complete belongs to the central while a connect attempt is outstanding or
  being cancelled; it then ends that connect call. A failed one that reports the cancel of an
  attempt the central has already resolved is consumed as well. Any other failed one is left to
  the peripheral code.
- Every later event that carries the central's connection handle (disconnection, connection and
  PHY updates, data length, GATT events) is consumed by the central and does not reach the
  peripheral code. A peer's request to change the connection parameters (L2CAP connection
  parameter update request) on such a link is rejected, so the link keeps the parameters the
  HAL set (see Timing).
- The peripheral code accepts only a successful connection-complete in the peripheral role and a
  disconnection of its own link.

The HAL does not stop advertising and does not drop a connected phone. Your app has to do that
before it scans: while the firmware advertises, the radio refuses the scan and
`furi_hal_bt_central_start_scan()` returns false. Suspend the peripheral before you initialise
the central, and restore the default profile after deinit (the BT service then advertises again
if Bluetooth is on in the settings). The Govee app and the example below do it like this:

```c
Bt* bt = furi_record_open(RECORD_BT);
bt_disconnect(bt);
furi_delay_ms(200);
furi_hal_bt_stop_advertising();
furi_record_close(RECORD_BT);
furi_hal_bt_central_init();

/* ... use the central ... */

furi_hal_bt_central_deinit();
bt = furi_record_open(RECORD_BT);
bt_profile_restore_default(bt);
furi_record_close(RECORD_BT);
```

`bt_disconnect()` returns once advertising has stopped. A connected phone is dropped by the
radio after that, in the background. The 200 ms pause gives that time to finish before the
central starts; `furi_hal_bt_stop_advertising()` then returns at once if advertising is already
off.

## Deinitialise before the Bluetooth profile changes

`bt_profile_start()` and `bt_profile_restore_default()` restart the Bluetooth stack. During that
restart the firmware checks that no event handler is registered with the BLE event dispatcher,
and stops with a crash if one is. An initialised central has a handler registered. Call
`furi_hal_bt_central_deinit()` before any profile change, including the restore of the default
profile when your app exits. Deinit removes the handler.

## Minimal usage

The sequence below scans for a device by name, connects, finds one characteristic, writes one
value and disconnects. Callbacks only copy and post; everything else runs on the app thread.
Every wait ends, because each step of the HAL has a deadline and the terminal event ends every wait
after the connect. A real app should still add its own timeouts and handle a full queue.

A callback posts with timeout 0, so when the queue is full the event is lost. Losing a terminal
event is the dangerous case: the app keeps waiting for a connect call that is already over. The
Govee app guards against that. Its connection callback first stores every terminal event in an
atomic variable, together with a number that identifies the connect call, and then posts the
message. The app thread reads and clears that variable each time it wakes up and on a periodic
timer, so it sees the terminal event even when the message was dropped. It handles a terminal
event only once when both the message and the variable carry it, and ignores one that belongs
to an older connect call. The scan end report is kept the same way. The app also counts
dropped messages, so a too-small queue shows up.

```c
#include <furi.h>
#include <furi_hal_bt.h>
#include <furi_hal_bt_central.h>
#include <bt/bt_service/bt.h>
#include <string.h>

typedef enum {
    EvScanReport,
    EvScanEnd,
    EvConnected,
    EvDiscovered,
    EvClosed, /* terminal: Disconnected or Error (LINK) */
    EvGattError,
} EvType;

typedef struct {
    EvType type;
    FuriHalBtCentralScanResult report;
} Ev;

/* 128-bit UUID of the characteristic, most significant byte first. */
static const uint8_t char_uuid[FURI_HAL_BT_CENTRAL_UUID128_LEN] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x2b, 0x11,
};

/* True when the advertising data carries a local name (AD type 0x08 or 0x09) containing want. */
static bool name_contains(const FuriHalBtCentralScanResult* r, const char* want) {
    size_t i = 0;
    while(i < r->data_len) {
        uint8_t field = r->data[i];
        if(field == 0 || i + field >= r->data_len) break;
        uint8_t type = r->data[i + 1];
        if(type == 0x08 || type == 0x09) {
            char name[32] = {0};
            size_t n = (size_t)field - 1;
            if(n >= sizeof(name)) n = sizeof(name) - 1;
            memcpy(name, &r->data[i + 2], n);
            return strstr(name, want) != NULL;
        }
        i += (size_t)field + 1;
    }
    return false;
}

/* BLE event thread: filter, copy, post with timeout 0. */
static void scan_cb(const FuriHalBtCentralScanResult* result, void* context) {
    FuriMessageQueue* queue = context;
    Ev ev = {0};
    if(result->report_type == FURI_HAL_BT_CENTRAL_SCAN_REPORT_END) {
        ev.type = EvScanEnd;
    } else if(name_contains(result, "MyDevice")) {
        ev.type = EvScanReport;
        ev.report = *result;
    } else {
        return;
    }
    furi_message_queue_put(queue, &ev, 0);
}

/* BLE event thread or deadline worker: map, post with timeout 0. */
static void conn_cb(
    FuriHalBtCentralConnection* conn,
    FuriHalBtCentralEvent event,
    uint16_t char_handle,
    const uint8_t* data,
    size_t len,
    void* context) {
    UNUSED(conn);
    UNUSED(data);
    UNUSED(len);
    FuriMessageQueue* queue = context;
    Ev ev = {0};
    switch(event) {
    case FuriHalBtCentralEventConnected:
        ev.type = EvConnected;
        break;
    case FuriHalBtCentralEventDiscoveryComplete:
        ev.type = EvDiscovered;
        break;
    case FuriHalBtCentralEventDisconnected:
        ev.type = EvClosed;
        break;
    case FuriHalBtCentralEventError:
        ev.type = (char_handle == FURI_HAL_BT_CENTRAL_ERROR_LINK) ? EvClosed : EvGattError;
        break;
    default:
        return;
    }
    furi_message_queue_put(queue, &ev, 0);
}

#define EV(t) (1UL << (t))

/* App thread: next event whose type is in mask. EvClosed always ends the wait, because no event
 * of a connect call follows its terminal event. */
static EvType wait_for(FuriMessageQueue* queue, Ev* ev, uint32_t mask) {
    mask |= EV(EvClosed);
    while(furi_message_queue_get(queue, ev, FuriWaitForever) == FuriStatusOk) {
        if(mask & EV(ev->type)) return ev->type;
    }
    return EvClosed;
}

int32_t central_demo_app(void* p) {
    UNUSED(p);
    FuriMessageQueue* queue = furi_message_queue_alloc(16, sizeof(Ev));
    Ev ev;

    /* Suspend the peripheral role, then start the central. */
    Bt* bt = furi_record_open(RECORD_BT);
    bt_disconnect(bt);
    furi_delay_ms(200);
    furi_hal_bt_stop_advertising();
    furi_record_close(RECORD_BT);
    furi_hal_bt_central_init();

    do {
        if(!furi_hal_bt_central_available()) break; /* not the Full stack */

        /* 1. Scan until the device shows up, then stop and wait for the end report. */
        if(!furi_hal_bt_central_start_scan(scan_cb, queue)) break;
        bool found = false;
        FuriHalBtCentralScanResult peer = {0};
        while(wait_for(queue, &ev, EV(EvScanReport) | EV(EvScanEnd)) == EvScanReport) {
            if(!found) {
                peer = ev.report;
                found = true;
                furi_hal_bt_central_stop_scan();
            }
        }
        if(!found) break;

        /* 2. Connect. Exactly one EvClosed follows every accepted connect. */
        FuriHalBtCentralConnection* conn =
            furi_hal_bt_central_connect(peer.addr, peer.addr_type, conn_cb, queue);
        if(!conn) break;
        if(wait_for(queue, &ev, EV(EvConnected)) == EvClosed) break;

        /* 3. Find the characteristic and write one value. */
        if(furi_hal_bt_central_gatt_discover_char(conn, char_uuid)) {
            EvType t = wait_for(queue, &ev, EV(EvDiscovered) | EV(EvGattError));
            if(t == EvClosed) break;
            uint16_t handle = 0;
            if(t == EvDiscovered) handle = furi_hal_bt_central_find_char(conn, char_uuid);
            if(handle != 0) {
                const uint8_t value[2] = {0x01, 0x00};
                furi_hal_bt_central_gatt_write_nr(conn, handle, value, sizeof(value));
            }
        }

        /* 4. Disconnect and wait for the terminal event. */
        furi_hal_bt_central_disconnect(conn);
        wait_for(queue, &ev, 0);
    } while(false);

    /* Deinit before the profile changes, then restore the peripheral role. */
    furi_hal_bt_central_deinit();
    bt = furi_record_open(RECORD_BT);
    bt_profile_restore_default(bt);
    furi_record_close(RECORD_BT);

    furi_message_queue_free(queue);
    return 0;
}
```

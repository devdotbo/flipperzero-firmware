/**
 * @file govee_status.h
 * Remote interface: signal numbers, argument validation, and the
 * remote status file /ext/apps_data/govee_h6006/remote_status.txt.
 *
 * Commands arrive as `loader signal <signal:decimal> <arg:hex>` (the loader
 * CLI needs the arg; the host tool always sends one). The signal callback runs
 * on the loader thread: it calls govee_remote_validate() (pure, no shared
 * state), posts the typed command to the inbox with timeout 0, and returns
 * true only if the command was valid and the post succeeded.
 *
 * Status file format: one `key=value` per line, '\n' line ends, ASCII only.
 *   seq=<dump sequence number>
 *   session=<run marker>         (fixed for one run of the app, differs between runs, never 0;
 *                                a reader treats a file without this line as session 0)
 *   req=<arg of the status request>  (signal 100 takes any arg and the dump echoes it, so a
 *                                reader can tell its own answer apart; 0 = no request id)
 *   uptime_ms=<now_ms>
 *   app=<app state>
 *   inbox_overflow=<count>
 *   target=<0 all, n lamp n>
 *   scanning=<0|1>
 *   scan.refused=<0|1>           (the last scan start was refused by the Bluetooth stack;
 *                                a reader treats a file without this line as 0)
 *   lightshow=<0|1>
 *   registry.count=<n>
 *   registry.max=16
 *   registry.full=<0|1>          (an upsert was refused because 16 lamps are saved)
 *   registry.load=<result>       (how the registry was loaded, glue string)
 *   registry.save=<result>       (last save result, glue string)
 *   desired.rev=<revision>
 *   desired.power=<0|1>
 *   desired.brightness=<1..100>
 *   desired.mode=<white|colour>
 *   desired.kelvin=<2700..6500>
 *   desired.rgb=<RRGGBB hex>
 *   engine.state=<govee_engine_state_name>
 *   engine.fault=<govee_engine_fault_name>
 *   engine.pass=<pass number>
 *   engine.in_pass=<0|1>
 *   engine.pending=<0|1>         (a lamp list change waits for the lamp boundary)
 *   engine.targeted=<n>
 *   lamps=<registry count>
 *   lamp.<n>=rev=<reached> try=<attempted> out=<outcome> targeted=<0|1> conn_ms=<> disc_ms=<>
 *            write_ms=<> dis_ms=<> total_ms=<> writes=<> reused=<0|1> type=<addr type>
 *            addr=<XX:XX:XX:XX:XX:XX> name=<rest of line, may contain spaces>
 *            (one line; n is the 1-based registry index; values come from the engine
 *            record with the same identity; a lamp the engine has not seen yet shows
 *            rev=0 try=0 out=none)
 *   events=<number of ring entries that follow>
 *   events.total=<events recorded since start or clear>
 *   ev.<seq>=<t_ms> <event name> lamp=<n or -> arg=<decimal>
 *            (lamp is the 1-based engine index, equal to the registry index unless
 *            engine.pending=1)
 *   end=<dump sequence number>
 * A reader treats the file as complete only if its last line is `end=` with
 * the same number as `seq=`.
 */
#pragma once

#include "govee_engine.h"
#include "govee_registry.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Status file path on the Flipper SD card. */
#define GOVEE_STATUS_PATH "/ext/apps_data/govee_h6006/remote_status.txt"

/** Buffer size that always holds a full status text (16 lamps, 64 events). */
#define GOVEE_STATUS_TEXT_MAX (8192u)

/** Buffer size that always holds one status line including '\n' and NUL. */
#define GOVEE_STATUS_LINE_MAX (256u)

/** Custom signals (furi/core/base.h: custom signals start at 100). */
typedef enum {
    GoveeSignalStatus = 100, /**< any arg: write the status file; the arg is echoed as req= */
    GoveeSignalPower = 101, /**< arg 0 or 1 */
    GoveeSignalBrightness = 102, /**< arg 1..100 (hex on the CLI: 1..64) */
    GoveeSignalKelvin = 103, /**< arg 2700..6500 kelvin */
    GoveeSignalRgb = 104, /**< arg 0xRRGGBB */
    GoveeSignalScan = 105, /**< arg 0: rescan after the current lamp work; ignored while a scan runs */
    GoveeSignalApply = 106, /**< arg 0: send everything again to all targeted lamps */
    GoveeSignalForget = 107, /**< arg n: remove lamp n (1-based registry index) */
    GoveeSignalTarget = 108, /**< arg 0: all lamps, n: only lamp n (1-based) */
    GoveeSignalLightshow = 109, /**< arg 0 stop, 1 start (exactly one lamp targeted) */
    GoveeSignalClearEvents = 110, /**< arg 0: clear the engine event ring */
} GoveeSignal;

#define GOVEE_SIGNAL_FIRST (100u)
#define GOVEE_SIGNAL_LAST  (110u)

/** A validated remote command. value is the argument, already range-checked. */
typedef struct {
    GoveeSignal signal;
    uint32_t value;
} GoveeRemoteCmd;

/**
 * Validate (signal, arg). Range checks only; whether lamp n exists is checked
 * by the app thread when it executes the command.
 * Pure function, safe on any thread.
 * @return true and fills *out if valid; false for any other signal or argument
 */
bool govee_remote_validate(uint32_t signal, uint32_t arg, GoveeRemoteCmd* out);

/** Everything the status file reports. Pointers must be valid during formatting. */
typedef struct {
    uint32_t seq; /**< dump sequence number, incremented by the glue per dump */
    uint32_t session; /**< run marker: set once at app start, differs between runs, not 0 */
    uint32_t req; /**< arg of the status request (signal 100) this dump answers; 0 = none */
    uint32_t now_ms;
    const char* app_state; /**< short token, e.g. "home", "scan"; NULL gives "unknown" */
    uint32_t inbox_overflow;
    uint8_t target; /**< 0 all, n lamp n */
    bool scanning;
    bool scan_refused; /**< the last scan start was refused by the Bluetooth stack */
    bool lightshow;
    bool registry_full;
    const char* load_result; /**< e.g. "main", "new", "bak", "empty"; NULL gives "unknown" */
    const char* save_result; /**< e.g. "ok", "write_failed"; NULL gives "unknown" */
    const GoveeRegistry* registry;
    const GoveeEngine* engine;
} GoveeStatusInput;

/**
 * Format line number *cursor into line (with '\n', NUL-terminated) and advance
 * *cursor. Start with *cursor = 0. Lets the glue stream the file with a
 * GOVEE_STATUS_LINE_MAX buffer instead of holding the whole text.
 * A line longer than size - 1 is cut, ends with '\n' and the call returns
 * GoveeStatusLineTruncated.
 */
typedef enum {
    GoveeStatusLineOk,
    GoveeStatusLineTruncated,
    GoveeStatusLineDone, /**< no more lines; line is set to "" */
} GoveeStatusLineResult;

GoveeStatusLineResult govee_status_format_line(
    const GoveeStatusInput* in,
    uint32_t* cursor,
    char* line,
    size_t size);

/**
 * Format the whole status text into buf. If it does not fit, the text is cut
 * at a line boundary before the `end=` line, so a reader sees it as partial.
 * @param out_len optional; bytes written without the NUL
 * @return true if everything including `end=` fit
 */
bool govee_status_format(const GoveeStatusInput* in, char* buf, size_t size, size_t* out_len);

#ifdef __cplusplus
}
#endif

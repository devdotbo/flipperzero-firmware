/**
 * @file govee_engine.h
 * Apply engine as a pure state machine.
 *
 * The engine owns no thread, timer, radio or heap. Inputs are the event and
 * command functions below; outputs go through GoveeEngineOps supplied by the
 * caller (the glue). Portable C (gnu17), no floating point, no recursion,
 * every loop bounded.
 *
 * ----------------------------------------------------------------------------
 * Threading and re-entrancy (the glue must guarantee all of this)
 * ----------------------------------------------------------------------------
 * - Every govee_engine_* function is called from one thread only (the app
 *   thread that consumes the inbox). HAL callbacks, timer callbacks
 *   and the signal callback never call the engine; they post to the inbox.
 * - No govee_engine_* function may be called from inside a GoveeEngineOps
 *   function. A call made while an op runs is ignored and recorded as event
 *   GoveeEvReentry, so the bug shows up in the status file.
 * - Ops never block. Their results either come back as the bool return value
 *   (synchronous acceptance) or later as an event function call.
 *
 * ----------------------------------------------------------------------------
 * Generations
 * ----------------------------------------------------------------------------
 * - Link generation: ops.connect() receives a fresh link_gen. The glue tags
 *   every later HAL event of that connection (Connected, DiscoveryComplete,
 *   Disconnected, Error) with this link_gen when it posts the event, and
 *   passes it back to the engine. Because the HAL accepts no new connect
 *   before the previous one produced its terminal event, an atomic
 *   "current link_gen" read in the HAL callback is sufficient.
 * - Timer generation: ops.timer_start() receives a timer_gen; the glue passes
 *   it back in govee_engine_on_timer(). A new timer_start or timer_stop makes
 *   every older timer_gen stale.
 * - Events with an old generation are ignored and logged as GoveeEvStale.
 *
 * ----------------------------------------------------------------------------
 * Timer semantics
 * ----------------------------------------------------------------------------
 * One one-shot timer. timer_start(ms, gen) (re)arms it, replacing any armed
 * one; timer_stop() disarms it. A fire that was already posted to the inbox
 * may still arrive after a stop or restart: the generation check drops it.
 * Firing late is fine; firing early is not (the engine does not recheck now_ms
 * against its deadlines).
 *
 * ----------------------------------------------------------------------------
 * "Write accepted"
 * ----------------------------------------------------------------------------
 * ops.write() returns true when the HAL queued the write-without-response
 * (furi_hal_bt_central_gatt_write_nr returned true). There is no completion
 * event; the engine paces writes with its own waits. "Submitted" in
 * outcomes means exactly this, never a confirmed lamp state.
 *
 * ----------------------------------------------------------------------------
 * HAL event to engine call mapping
 * ----------------------------------------------------------------------------
 *   HAL Connected               -> govee_engine_on_connected(link_gen)
 *   HAL DiscoveryComplete       -> govee_engine_on_discovery(link_gen, handle != 0)
 *   HAL Disconnected            -> govee_engine_on_disconnected(link_gen)
 *   HAL Error                   -> govee_engine_on_link_error(link_gen)
 *   HAL WriteComplete, Notification: not passed to the engine.
 * The engine interprets them by its state:
 *   state          on_connected   on_discovery   on_disconnected  on_link_error  timer fires
 *   Connecting     connected      ignored        connect failed   connect failed connect timeout -> cancel
 *   Cancelling     ignored        ignored        cancel done      cancel failed  cancel failed
 *   Discovering    ignored        found/missing  link lost        discovery fail discovery failed
 *   WriteGap       ignored        ignored        link lost        link lost      next write
 *   Tail           ignored        ignored        link gone (ok)   disconnect     close or keep
 *   Disconnecting  ignored        ignored        disconnect done  logged, waits  fault (disconnect timeout)
 *   Kept           ignored        ignored        link gone        disconnect     disconnect (keep limit)
 *   Idle/Faulted/Stopped: link events logged and ignored.
 *
 * ----------------------------------------------------------------------------
 * States and their deadlines
 * ----------------------------------------------------------------------------
 *   Idle          no link, no operation outstanding. Waits for input only;
 *                 nothing can be lost because nothing was requested.
 *   Connecting    GOVEE_ENGINE_CONNECT_DEADLINE_MS, then cancel; for a lamp
 *                 whose last attempt failed GOVEE_ENGINE_CONNECT_RETRY_DEADLINE_MS,
 *                 and a change of the desired state (set_desired with a
 *                 difference, apply_again) cancels such an attempt at once;
 *                 the next pass starts when that cancel has resolved.
 *   Cancelling    GOVEE_ENGINE_CANCEL_DEADLINE_MS, then Faulted.
 *   Discovering   GOVEE_ENGINE_DISCOVER_DEADLINE_MS, then disconnect.
 *   WriteGap      GOVEE_ENGINE_GAP_AFTER_POWER_ON_MS or GOVEE_ENGINE_GAP_MS.
 *   Tail          GOVEE_ENGINE_TAIL_MS.
 *   Disconnecting GOVEE_ENGINE_DISCONNECT_DEADLINE_MS, then Faulted; after
 *                 govee_engine_stop() GOVEE_ENGINE_STOP_DISCONNECT_DEADLINE_MS
 *                 (also for a disconnect already running when stop came).
 *   Kept          GOVEE_ENGINE_KEEP_MAX_MS after connect, then disconnect.
 *   Faulted       the link layer did not resolve a cancel or disconnect in
 *                 time; the engine issues nothing until the glue has
 *                 deinitialised and reinitialised the central HAL (the HAL bounds
 *                 that) and calls govee_engine_link_reset(). No operation of
 *                 the engine is outstanding, so it cannot wait on a lost event.
 *   Stopped       terminal; nothing outstanding.
 * Early link loss: when the visit's link ends after Connected and before
 * the first write of the visit was accepted (Disconnected while discovering,
 * or the disconnect op finds the link already gone after a link error or a
 * rejected first write), the engine records GoveeEvLinkRetry and connects the
 * same lamp once more within the same visit (same deadline as the first
 * connect, total_ms covers both attempts). At most one such retry per visit; a
 * second early loss gives the lamp its failure outcome. A link lost after an
 * accepted write is never retried.
 * Worst case for one lamp visit: GOVEE_ENGINE_VISIT_MAX_MS.
 * Worst case for govee_engine_stop() to reach Stopped (or Faulted, which the
 * glue then treats as stopped): GOVEE_ENGINE_STOP_MAX_MS.
 *
 * ----------------------------------------------------------------------------
 * Legal call order
 * ----------------------------------------------------------------------------
 *   govee_engine_init()                       once, before anything else
 *   govee_engine_set_lamps() / set_desired()  any time before stop
 *   govee_engine_on_*()                       any time after init (stale or
 *                                             unexpected ones are logged and ignored)
 *   govee_engine_apply_again()                any time before stop
 *   govee_engine_link_reset()                 only in Faulted (ignored otherwise)
 *   govee_engine_stop()                       any time; then keep delivering
 *                                             events until govee_engine_is_stopped()
 *                                             or GOVEE_ENGINE_STOP_MAX_MS passed
 *   getters, clear_events                     any time
 */
#pragma once

#include "govee_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- Deadlines and waits (milliseconds) ---------------------------------- */

/** Connect must complete within this, else the attempt is cancelled. */
#define GOVEE_ENGINE_CONNECT_DEADLINE_MS (3000u)
/** Connect deadline for a lamp whose last attempt failed. */
#define GOVEE_ENGINE_CONNECT_RETRY_DEADLINE_MS (1500u)
/** Cancel must resolve within this, else Faulted. */
#define GOVEE_ENGINE_CANCEL_DEADLINE_MS (1500u)
/** Targeted discovery (the HAL has an internal 3 s deadline that ends in Error); engine backstop. */
#define GOVEE_ENGINE_DISCOVER_DEADLINE_MS (3500u)
/** Wait after a power-on command before the next command of the same visit. */
#define GOVEE_ENGINE_GAP_AFTER_POWER_ON_MS (200u)
/** Wait after any other command before the next command of the same visit. */
#define GOVEE_ENGINE_GAP_MS (50u)
/** Wait after the last command before disconnecting or keeping the link. */
#define GOVEE_ENGINE_TAIL_MS (150u)
/** Disconnect must complete within this, else Faulted (above the 2 s link supervision timeout). */
#define GOVEE_ENGINE_DISCONNECT_DEADLINE_MS (5000u)
/** Disconnect deadline once govee_engine_stop() was called (keeps the stop bound short). */
#define GOVEE_ENGINE_STOP_DISCONNECT_DEADLINE_MS (1000u)
/** A kept link is closed this long after it connected; the bulb drops links after about 15 s. */
#define GOVEE_ENGINE_KEEP_MAX_MS (12000u)

/** Upper bound for one lamp visit (an early-lost connect and discovery, then
 *  connect, discover, three writes, tail, disconnect). */
#define GOVEE_ENGINE_VISIT_MAX_MS                                                      \
    (GOVEE_ENGINE_CONNECT_DEADLINE_MS + GOVEE_ENGINE_DISCOVER_DEADLINE_MS +            \
     GOVEE_ENGINE_CONNECT_DEADLINE_MS + GOVEE_ENGINE_CANCEL_DEADLINE_MS +              \
     GOVEE_ENGINE_DISCOVER_DEADLINE_MS + GOVEE_ENGINE_GAP_AFTER_POWER_ON_MS +          \
     GOVEE_ENGINE_GAP_MS + GOVEE_ENGINE_TAIL_MS + GOVEE_ENGINE_DISCONNECT_DEADLINE_MS)
/** Upper bound from govee_engine_stop() to Stopped or Faulted. */
#define GOVEE_ENGINE_STOP_MAX_MS \
    (GOVEE_ENGINE_CANCEL_DEADLINE_MS + GOVEE_ENGINE_STOP_DISCONNECT_DEADLINE_MS)

/** Number of engine events kept in the ring. */
#define GOVEE_ENGINE_EVENT_RING (64u)

/* --- Public enums --------------------------------------------------------- */

typedef enum {
    GoveeEngineIdle = 0,
    GoveeEngineConnecting,
    GoveeEngineCancelling,
    GoveeEngineDiscovering,
    GoveeEngineWriteGap,
    GoveeEngineTail,
    GoveeEngineDisconnecting,
    GoveeEngineKept,
    GoveeEngineFaulted,
    GoveeEngineStopped,
    GoveeEngineStateCount,
} GoveeEngineState;

/** Outcome of the last attempt on a lamp. Only Submitted and Unchanged advance its revision. */
typedef enum {
    GoveeOutcomeNone = 0, /**< never attempted */
    GoveeOutcomeSubmitted, /**< every differing property was accepted by the HAL */
    GoveeOutcomeUnchanged, /**< nothing differed; revision advanced without a visit */
    GoveeOutcomeConnectFailed, /**< connect refused or the link went down before Connected */
    GoveeOutcomeConnectTimeout, /**< no Connected within the deadline; cancel succeeded */
    GoveeOutcomeCancelFailed, /**< connect timed out and the cancel failed or timed out */
    GoveeOutcomeDiscoveryFailed, /**< discovery refused, failed, timed out or no write characteristic */
    GoveeOutcomeWriteRejected, /**< the HAL refused a write */
    GoveeOutcomeLinkLost, /**< link dropped before every property was submitted */
    GoveeOutcomeAborted, /**< visit ended by govee_engine_stop() */
    GoveeOutcomeCount,
} GoveeOutcome;

/** Event types recorded in the ring. */
typedef enum {
    GoveeEvNone = 0,
    GoveeEvDesired, /**< arg = new revision */
    GoveeEvApplyAgain, /**< arg = new revision */
    GoveeEvLamps, /**< arg = lamp count (applied) */
    GoveeEvLampsPending, /**< arg = lamp count (stored until the lamp boundary) */
    GoveeEvPassStart, /**< arg = pass number */
    GoveeEvPassEnd, /**< arg = pass duration ms */
    GoveeEvVisit, /**< arg = revision visited */
    GoveeEvConnect, /**< arg = link gen */
    GoveeEvConnected, /**< arg = connect ms */
    GoveeEvConnectFailed, /**< arg = connect ms */
    GoveeEvConnectTimeout, /**< arg = link gen */
    GoveeEvCancelDone, /**< arg = cancel ms */
    GoveeEvCancelFailed, /**< arg = cancel ms */
    GoveeEvDiscover, /**< arg = link gen */
    GoveeEvDiscovered, /**< arg = discovery ms */
    GoveeEvDiscoveryFailed, /**< arg = discovery ms */
    GoveeEvWrite, /**< arg = property (0 power, 1 brightness, 2 colour) */
    GoveeEvWriteRejected, /**< arg = property */
    GoveeEvSubmitted, /**< arg = revision reached */
    GoveeEvUnchanged, /**< arg = revision reached */
    GoveeEvReuse, /**< arg = link age ms */
    GoveeEvKeep, /**< arg = link age ms */
    GoveeEvDisconnect, /**< arg = link gen */
    GoveeEvDisconnected, /**< arg = disconnect ms */
    GoveeEvDisconnectTimeout, /**< arg = link gen */
    GoveeEvLinkLost, /**< arg = engine state when it happened */
    GoveeEvFault, /**< arg = GoveeEngineFault */
    GoveeEvLinkReset, /**< arg = 0 */
    GoveeEvStop, /**< arg = engine state when requested */
    GoveeEvStopped, /**< arg = 0 */
    GoveeEvStale, /**< arg = stale generation */
    GoveeEvUnexpected, /**< arg = engine state; event did not fit the state */
    GoveeEvReentry, /**< arg = 0; engine called from inside an op */
    GoveeEvConnectSuperseded, /**< arg = link gen; a change cancelled a retry connect */
    GoveeEvLinkRetry, /**< arg = lost link gen; early link loss, same lamp connected again */
    GoveeEvCount,
} GoveeEventType;

/** Why the engine is Faulted. */
typedef enum {
    GoveeFaultNone = 0,
    GoveeFaultCancel, /**< cancel refused, failed or timed out */
    GoveeFaultDisconnect, /**< disconnect timed out or failed */
    GoveeFaultInternal, /**< internal step bound exceeded (bug) */
} GoveeEngineFault;

/** Property indices for writes (order of submission). */
typedef enum {
    GoveePropPower = 0,
    GoveePropBrightness = 1,
    GoveePropColour = 2,
    GoveePropCount = 3,
} GoveeProp;

/* --- Output function table ------------------------------------------------ */

typedef struct {
    /** Start connecting to id. Return false if the HAL refused (connect returned NULL). */
    bool (*connect)(void* ctx, const GoveeLampId* id, uint32_t link_gen);
    /** Cancel the pending connect. Return false if the cancel could not be issued. */
    bool (*cancel_connect)(void* ctx, uint32_t link_gen);
    /** Start the targeted discovery of the write characteristic. Return false if refused. */
    bool (*discover)(void* ctx, uint32_t link_gen);
    /** Queue one write without response to the write characteristic. Return true if accepted. */
    bool (*write)(void* ctx, uint32_t link_gen, const uint8_t* data, size_t len);
    /** Start disconnecting. Return false if there is no link to disconnect (treated as done). */
    bool (*disconnect)(void* ctx, uint32_t link_gen);
    /** (Re)arm the one-shot engine timer. */
    void (*timer_start)(void* ctx, uint32_t ms, uint32_t timer_gen);
    /** Disarm the engine timer. */
    void (*timer_stop)(void* ctx);
    /** Monotonic milliseconds (wraps at 2^32). */
    uint32_t (*now_ms)(void* ctx);
    /** Optional (may be NULL): lamp outcome, pass or state changed; refresh the UI. Must not call the engine. */
    void (*changed)(void* ctx);
} GoveeEngineOps;

/* --- Engine data (caller allocates; treat as read-only outside the engine) - */

typedef struct {
    uint32_t connect_ms;
    uint32_t discover_ms;
    uint32_t write_ms; /**< first write to last accepted write, including gaps */
    uint32_t disconnect_ms;
    uint32_t total_ms; /**< visit start to visit end (link closed or kept), cancel and tail included */
    uint8_t writes; /**< writes accepted in the visit */
    bool reused; /**< the visit reused a kept link */
} GoveeLampTimes;

typedef struct {
    GoveeLampId id;
    bool targeted;
    bool visited; /**< visited in the current pass */
    /* last submitted values */
    bool known[GoveePropCount];
    bool power;
    uint8_t brightness;
    uint8_t mode;
    uint16_t kelvin;
    uint32_t rgb;
    /* progress */
    uint32_t reached_rev; /**< revision whose differing properties were all submitted */
    uint32_t attempt_rev; /**< revision of the last attempt (one attempt per lamp and revision) */
    uint8_t outcome; /**< GoveeOutcome of the last attempt */
    uint8_t last; /**< GoveeLampLast of the last completed attempt; aborted and superseded ones do not count */
    GoveeLampTimes times; /**< of the last attempt */
} GoveeEngineLamp;

typedef struct {
    uint32_t t_ms;
    uint32_t arg;
    uint8_t type; /**< GoveeEventType */
    uint8_t lamp; /**< engine lamp index or GOVEE_LAMP_NONE */
} GoveeEngineEvent;

typedef struct {
    const GoveeEngineOps* ops;
    void* ctx;

    uint8_t state; /**< GoveeEngineState, plus internal transient states */
    uint8_t fault; /**< GoveeEngineFault */
    bool stop_requested;
    bool in_op;

    GoveeDesired desired;
    uint32_t rev;

    GoveeEngineLamp lamps[GOVEE_MAX_LAMPS];
    uint8_t lamp_count;
    uint8_t target; /**< 0 = all, n = only lamp n (1-based) */

    bool pending_valid;
    GoveeLampId pending_ids[GOVEE_MAX_LAMPS];
    uint8_t pending_last[GOVEE_MAX_LAMPS];
    uint8_t pending_count;
    uint8_t pending_target;
    bool pending_resend; /**< apply_again came while the change above was pending */

    /* pass */
    bool in_pass;
    uint32_t pass_no;
    uint32_t pass_start_ms;
    uint8_t last_visited;

    /* visit */
    uint8_t visit_lamp;
    uint32_t visit_rev;
    GoveeDesired visit_desired;
    uint8_t next_prop;
    bool visit_ok;
    bool visit_closing; /**< the visit ended and its link is being closed */
    bool visit_retry; /**< the visited lamp's previous attempt failed (shorter deadline) */
    bool restart_pass; /**< a change cancelled a retry connect: new pass once the cancel resolved */
    bool visit_connected; /**< the visit's link reached Connected (or was reused) */
    bool visit_relinked; /**< the visit used its one early-loss retry */
    uint8_t visit_prev_last; /**< GoveeLampLast of the visited lamp before this visit */
    uint32_t visit_start_ms;
    uint32_t phase_start_ms;
    uint32_t write_start_ms;

    /* link */
    bool link_open; /**< a link (or connect attempt) exists in the HAL */
    uint8_t link_lamp; /**< engine lamp index of the link, GOVEE_LAMP_NONE if none or removed */
    GoveeLampId link_id;
    uint32_t link_gen;
    uint32_t link_connected_ms;
    uint32_t timer_gen;

    /* event ring */
    GoveeEngineEvent ring[GOVEE_ENGINE_EVENT_RING];
    uint32_t ring_total; /**< events recorded since init or clear; ring holds the last min(total, 64) */
} GoveeEngine;

/* --- Lifecycle and commands ---------------------------------------------- */

/** Initialise. ops must stay valid for the engine's lifetime; every op except changed is required. */
void govee_engine_init(GoveeEngine* e, const GoveeEngineOps* ops, void* ctx, const GoveeDesired* initial);

/**
 * Set the desired state. Increments the revision only if something changed.
 * Values are clamped to the documented ranges.
 * @return the current revision
 */
uint32_t govee_engine_set_desired(GoveeEngine* e, const GoveeDesired* d);

/**
 * Re-send everything to every targeted lamp (signal 106): forgets the last
 * submitted values of the targeted lamps and increments the revision.
 * @return the new revision
 */
uint32_t govee_engine_apply_again(GoveeEngine* e);

/**
 * Replace the lamp list and target. Per-lamp state is kept by identity.
 * target: 0 = all lamps, n = only lamp n (1-based index into ids); out of range means all.
 * During a visit the change is stored and applied at the next lamp boundary.
 */
void govee_engine_set_lamps(GoveeEngine* e, const GoveeLampId* ids, uint8_t count, uint8_t target);

/**
 * As govee_engine_set_lamps, plus the saved result of each lamp's last
 * completed attempt. last may be NULL (all unknown); last[i] is a
 * GoveeLampLast and is taken only for an identity the engine does not hold
 * yet (a lamp it holds keeps what this run learned). A lamp with
 * GoveeLampLastFailed and no attempt in this run is treated exactly like a lamp
 * whose last attempt in this run failed (visited after the others, shorter
 * connect deadline, attempt cancelled by a change).
 */
void govee_engine_set_lamps_last(
    GoveeEngine* e,
    const GoveeLampId* ids,
    const uint8_t* last,
    uint8_t count,
    uint8_t target);

/** Request a stop: cancels or closes the current operation, then Stopped. */
void govee_engine_stop(GoveeEngine* e);

/** Faulted only: the glue has reinitialised the central HAL; resume. */
void govee_engine_link_reset(GoveeEngine* e);

/**
 * Kept only: close the kept link now (the app closes it before a rescan).
 * The engine disconnects and returns to Idle unless a lamp is behind.
 * Ignored in every other state. @return true if a close was started
 */
bool govee_engine_release_link(GoveeEngine* e);

/** Clear the event ring (signal 110). */
void govee_engine_clear_events(GoveeEngine* e);

/* --- Events -------------------------------------------------------------- */

void govee_engine_on_connected(GoveeEngine* e, uint32_t link_gen);
void govee_engine_on_discovery(GoveeEngine* e, uint32_t link_gen, bool found);
void govee_engine_on_disconnected(GoveeEngine* e, uint32_t link_gen);
void govee_engine_on_link_error(GoveeEngine* e, uint32_t link_gen);
void govee_engine_on_timer(GoveeEngine* e, uint32_t timer_gen);

/* --- Getters -------------------------------------------------------------- */

GoveeEngineState govee_engine_state(const GoveeEngine* e);
bool govee_engine_is_stopped(const GoveeEngine* e);
/** True while a pass runs or a link is open. Rescan is allowed only when false. */
bool govee_engine_is_busy(const GoveeEngine* e);
uint32_t govee_engine_revision(const GoveeEngine* e);
/** Number of currently targeted lamps. */
uint8_t govee_engine_targeted_count(const GoveeEngine* e);
/** Engine lamp index for an identity, or GOVEE_LAMP_NONE. */
uint8_t govee_engine_find_lamp(const GoveeEngine* e, const GoveeLampId* id);
/** Lamp record by engine index, or NULL. */
const GoveeEngineLamp* govee_engine_lamp(const GoveeEngine* e, uint8_t index);
/** Number of events in the ring (at most GOVEE_ENGINE_EVENT_RING). */
uint32_t govee_engine_event_count(const GoveeEngine* e);
/**
 * i-th oldest event still in the ring (0 = oldest). *seq receives its
 * sequence number (1-based since init or clear). @return NULL if out of range
 */
const GoveeEngineEvent* govee_engine_event(const GoveeEngine* e, uint32_t i, uint32_t* seq);

const char* govee_engine_state_name(GoveeEngineState s);
const char* govee_engine_outcome_name(GoveeOutcome o);
const char* govee_engine_event_name(GoveeEventType t);
const char* govee_engine_fault_name(GoveeEngineFault f);

/** True for outcomes that mean "this attempt is over and the lamp did not get the revision". */
bool govee_engine_outcome_is_failure(GoveeOutcome o);

#ifdef __cplusplus
}
#endif

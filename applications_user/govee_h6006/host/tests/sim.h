/* Fake clock and fake BLE link for driving the engine on the host.
 * Ops never call the engine; they schedule events that sim_run() delivers
 * in time order, which mirrors the inbox of the real app. */
#pragma once

#include "govee_engine.h"

#define SIM_QUEUE    (32)
#define SIM_LOG      (256)

typedef enum {
    SimEvConnected,
    SimEvDiscovery,
    SimEvDisconnected,
    SimEvError,
    SimEvTimer, /* only through sim_inject: on_timer with the given timer generation */
    SimEvErrorLink, /* terminal link error: the link is gone (glue: latch), then on_link_error */
} SimEvType;

typedef struct {
    bool used;
    uint32_t due;
    uint8_t type;
    uint32_t gen;
    bool found;
} SimEv;

typedef struct {
    bool reachable; /* connect completes after connect_ms */
    uint32_t connect_ms;
    bool connect_fails; /* Disconnected after connect_ms instead of Connected */
    uint32_t discover_ms;
    bool discover_silent; /* no discovery result at all */
    bool has_char;
    uint8_t cancel_mode; /* 0 Disconnected after 40 ms, 1 Error, 2 silent, 3 cancel op refused */
    uint32_t disconnect_ms;
    bool disconnect_silent;
    int reject_write_at; /* 0-based write index within one link, -1 never */
    int drop_after_writes; /* link lost after this many writes on one link, -1 never */
    int drop_in_discovery; /* the next N discoveries end with the link lost instead of a result */
    bool drop_as_error; /* that loss arrives as a terminal link error instead of Disconnected */
} SimLamp;

typedef struct {
    uint32_t now;
    uint32_t start;
    SimEv q[SIM_QUEUE];
    bool timer_armed;
    uint32_t timer_due;
    uint32_t timer_gen;

    GoveeLampId ids[GOVEE_MAX_LAMPS];
    SimLamp lamp[GOVEE_MAX_LAMPS];
    uint8_t n;

    int cur; /* sim lamp index of the current link or connect, -1 none */
    uint32_t cur_gen;
    int writes_on_link;

    int connects, cancels, discovers, writes, disconnects, changed_calls;
    int connects_per_lamp[GOVEE_MAX_LAMPS];
    int writes_per_lamp[GOVEE_MAX_LAMPS];
    int connect_order[SIM_LOG];
    int connect_order_n;
    uint8_t write_log[SIM_LOG][20];
    int write_log_lamp[SIM_LOG];
    uint32_t write_log_t[SIM_LOG];
    int write_log_n;

    bool refuse_connect;
    bool refuse_discover;
    GoveeEngine* eng;
    bool reenter_from_changed;
} Sim;

extern const GoveeEngineOps sim_ops;

void sim_init(Sim* s, GoveeEngine* e, uint8_t lamps, uint32_t start_ms);
/* default profile: reachable, 300 ms connect, 200 ms discovery, char present, 50 ms disconnect */
SimLamp sim_lamp_default(void);
/* engine set_lamps with all sim lamps and the given target */
void sim_set_lamps(Sim* s, uint8_t target);
/* Deliver events in time order until nothing is pending or max_ms of simulated time passed.
 * Returns the number of events delivered. */
int sim_run(Sim* s, uint32_t max_ms);
/* Run until the engine is Idle, Kept, Faulted or Stopped with nothing pending; returns elapsed ms. */
uint32_t sim_settle(Sim* s, uint32_t max_ms);
/* Queue an event for delivery delay ms from now, whatever generation it carries
 * (the ops drop queued events of a cancelled generation; this injects late ones). */
void sim_inject(Sim* s, uint32_t delay, SimEvType type, uint32_t gen);
/* Last packet written to sim lamp i with command byte cmd (packet[1]); NULL if none. */
const uint8_t* sim_last_write(const Sim* s, int lamp, uint8_t cmd);

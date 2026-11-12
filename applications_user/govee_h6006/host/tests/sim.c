#include "sim.h"

#include <string.h>

static int lamp_index(const Sim* s, const GoveeLampId* id) {
    for(int i = 0; i < s->n; i++) {
        if(govee_lamp_id_equal(&s->ids[i], id)) return i;
    }
    return -1;
}

static void schedule(Sim* s, uint32_t delay, SimEvType type, uint32_t gen, bool found) {
    for(int i = 0; i < SIM_QUEUE; i++) {
        if(!s->q[i].used) {
            s->q[i].used = true;
            s->q[i].due = s->now + delay;
            s->q[i].type = (uint8_t)type;
            s->q[i].gen = gen;
            s->q[i].found = found;
            return;
        }
    }
}

static void drop_gen(Sim* s, uint32_t gen) {
    for(int i = 0; i < SIM_QUEUE; i++) {
        if(s->q[i].used && s->q[i].gen == gen) s->q[i].used = false;
    }
}

static const SimLamp* cur_lamp(const Sim* s) {
    static const SimLamp none = {0};
    return (s->cur >= 0) ? &s->lamp[s->cur] : &none;
}

static bool op_connect(void* ctx, const GoveeLampId* id, uint32_t gen) {
    Sim* s = ctx;
    s->connects++;
    int i = lamp_index(s, id);
    if(s->connect_order_n < SIM_LOG) s->connect_order[s->connect_order_n++] = i;
    if(i >= 0) s->connects_per_lamp[i]++;
    if(s->refuse_connect || i < 0) return false;
    s->cur = i;
    s->cur_gen = gen;
    s->writes_on_link = 0;
    const SimLamp* l = &s->lamp[i];
    if(l->reachable) {
        schedule(s, l->connect_ms, l->connect_fails ? SimEvDisconnected : SimEvConnected, gen, false);
    }
    return true;
}

static bool op_cancel(void* ctx, uint32_t gen) {
    Sim* s = ctx;
    s->cancels++;
    const SimLamp* l = cur_lamp(s);
    if(l->cancel_mode == 3) return false;
    drop_gen(s, gen);
    if(l->cancel_mode == 0) schedule(s, 40, SimEvDisconnected, gen, false);
    if(l->cancel_mode == 1) schedule(s, 40, SimEvError, gen, false);
    return true;
}

static bool op_discover(void* ctx, uint32_t gen) {
    Sim* s = ctx;
    s->discovers++;
    if(s->refuse_discover) return false;
    const SimLamp* l = cur_lamp(s);
    if(s->cur >= 0 && s->lamp[s->cur].drop_in_discovery > 0) {
        s->lamp[s->cur].drop_in_discovery--;
        schedule(s, 115, l->drop_as_error ? SimEvErrorLink : SimEvDisconnected, gen, false);
        return true;
    }
    if(!l->discover_silent) schedule(s, l->discover_ms, SimEvDiscovery, gen, l->has_char);
    return true;
}

static bool op_write(void* ctx, uint32_t gen, const uint8_t* data, size_t len) {
    Sim* s = ctx;
    (void)gen;
    s->writes++;
    const SimLamp* l = cur_lamp(s);
    if(l->reject_write_at >= 0 && s->writes_on_link == l->reject_write_at) return false;
    if(s->write_log_n < SIM_LOG && len == 20) {
        memcpy(s->write_log[s->write_log_n], data, 20);
        s->write_log_lamp[s->write_log_n] = s->cur;
        s->write_log_t[s->write_log_n] = s->now;
        s->write_log_n++;
    }
    if(s->cur >= 0) s->writes_per_lamp[s->cur]++;
    s->writes_on_link++;
    if(l->drop_after_writes >= 0 && s->writes_on_link == l->drop_after_writes) {
        schedule(s, 1, SimEvDisconnected, gen, false);
    }
    return true;
}

static bool op_disconnect(void* ctx, uint32_t gen) {
    Sim* s = ctx;
    s->disconnects++;
    if(s->cur < 0) return false;
    drop_gen(s, gen);
    const SimLamp* l = cur_lamp(s);
    if(!l->disconnect_silent) schedule(s, l->disconnect_ms, SimEvDisconnected, gen, false);
    return true;
}

static void op_timer_start(void* ctx, uint32_t ms, uint32_t gen) {
    Sim* s = ctx;
    s->timer_armed = true;
    s->timer_due = s->now + ms;
    s->timer_gen = gen;
}

static void op_timer_stop(void* ctx) {
    Sim* s = ctx;
    s->timer_armed = false;
}

static uint32_t op_now(void* ctx) {
    const Sim* s = ctx;
    return s->now;
}

static void op_changed(void* ctx) {
    Sim* s = ctx;
    s->changed_calls++;
    if(s->reenter_from_changed && s->eng) {
        GoveeDesired d = s->eng->desired;
        d.brightness = (uint8_t)(d.brightness == 10 ? 11 : 10);
        govee_engine_set_desired(s->eng, &d); /* must be refused by the engine */
    }
}

const GoveeEngineOps sim_ops = {
    .connect = op_connect,
    .cancel_connect = op_cancel,
    .discover = op_discover,
    .write = op_write,
    .disconnect = op_disconnect,
    .timer_start = op_timer_start,
    .timer_stop = op_timer_stop,
    .now_ms = op_now,
    .changed = op_changed,
};

SimLamp sim_lamp_default(void) {
    SimLamp l;
    memset(&l, 0, sizeof(l));
    l.reachable = true;
    l.connect_ms = 300;
    l.discover_ms = 200;
    l.has_char = true;
    l.cancel_mode = 0;
    l.disconnect_ms = 50;
    l.reject_write_at = -1;
    l.drop_after_writes = -1;
    return l;
}

void sim_init(Sim* s, GoveeEngine* e, uint8_t lamps, uint32_t start_ms) {
    memset(s, 0, sizeof(*s));
    s->now = start_ms;
    s->start = start_ms;
    s->cur = -1;
    s->eng = e;
    s->n = lamps;
    for(uint8_t i = 0; i < lamps; i++) {
        s->ids[i].addr_type = 0;
        s->ids[i].addr[0] = 0xA4;
        s->ids[i].addr[1] = 0xC1;
        s->ids[i].addr[2] = 0x38;
        s->ids[i].addr[5] = (uint8_t)(i + 1u);
        s->lamp[i] = sim_lamp_default();
    }
    GoveeDesired d = {.power = true, .brightness = 50, .mode = GoveeModeWhite, .kelvin = 4000, .rgb = 0};
    govee_engine_init(e, &sim_ops, s, &d);
}

void sim_set_lamps(Sim* s, uint8_t target) {
    govee_engine_set_lamps(s->eng, s->ids, s->n, target);
}

/* Deliver the earliest event due at or before limit. */
static bool sim_step(Sim* s, uint32_t limit) {
    int best = -1;
    uint32_t best_due = 0;
    bool timer = false;
    for(int i = 0; i < SIM_QUEUE; i++) {
        if(s->q[i].used && (best < 0 || (int32_t)(s->q[i].due - best_due) < 0)) {
            best = i;
            best_due = s->q[i].due;
        }
    }
    if(s->timer_armed && (best < 0 || (int32_t)(s->timer_due - best_due) < 0)) {
        timer = true;
        best_due = s->timer_due;
    }
    if(best < 0 && !timer) return false;
    if((int32_t)(best_due - limit) > 0) return false;
    if((int32_t)(best_due - s->now) > 0) s->now = best_due;

    if(timer) {
        s->timer_armed = false;
        govee_engine_on_timer(s->eng, s->timer_gen);
        return true;
    }
    SimEv ev = s->q[best];
    s->q[best].used = false;
    switch(ev.type) {
    case SimEvConnected:
        govee_engine_on_connected(s->eng, ev.gen);
        break;
    case SimEvDiscovery:
        govee_engine_on_discovery(s->eng, ev.gen, ev.found);
        break;
    case SimEvDisconnected:
        if(ev.gen == s->cur_gen) s->cur = -1;
        govee_engine_on_disconnected(s->eng, ev.gen);
        break;
    case SimEvTimer:
        govee_engine_on_timer(s->eng, ev.gen);
        break;
    case SimEvErrorLink:
        if(ev.gen == s->cur_gen) s->cur = -1;
        govee_engine_on_link_error(s->eng, ev.gen);
        break;
    default:
        govee_engine_on_link_error(s->eng, ev.gen);
        break;
    }
    return true;
}

void sim_inject(Sim* s, uint32_t delay, SimEvType type, uint32_t gen) {
    schedule(s, delay, type, gen, false);
}

int sim_run(Sim* s, uint32_t max_ms) {
    uint32_t limit = s->now + max_ms;
    int n = 0;
    while(n < 100000 && sim_step(s, limit)) n++;
    if((int32_t)(limit - s->now) > 0) s->now = limit;
    return n;
}

static bool link_events_pending(const Sim* s) {
    for(int i = 0; i < SIM_QUEUE; i++) {
        if(s->q[i].used) return true;
    }
    return false;
}

uint32_t sim_settle(Sim* s, uint32_t max_ms) {
    uint32_t t0 = s->now;
    uint32_t limit = s->now + max_ms;
    for(int n = 0; n < 100000; n++) {
        GoveeEngineState st = govee_engine_state(s->eng);
        bool resting = st == GoveeEngineIdle || st == GoveeEngineKept ||
                       st == GoveeEngineFaulted || st == GoveeEngineStopped;
        if(resting && !link_events_pending(s)) break;
        if(!sim_step(s, limit)) break;
    }
    return s->now - t0;
}

const uint8_t* sim_last_write(const Sim* s, int lamp, uint8_t cmd) {
    for(int i = s->write_log_n - 1; i >= 0; i--) {
        if(s->write_log_lamp[i] == lamp && s->write_log[i][1] == cmd) return s->write_log[i];
    }
    return NULL;
}

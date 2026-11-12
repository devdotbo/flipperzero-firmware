#include "govee_engine.h"
#include "govee_h6006.h"

#include <string.h>

/* Internal transient states. They exist only inside drive() and never
 * persist when a public function returns. */
#define S_BOUNDARY   (100u) /* between lamps: apply pending list, pick next lamp */
#define S_WRITE_NEXT (101u) /* submit the next differing property */
#define S_DISCONNECT (102u) /* close the link unconditionally */
#define S_CONNECT    (103u) /* connect the visited lamp again after an early link loss */

/* Bound on transient steps per drive() call: each lamp can pass through at most
 * a boundary, a write step and a disconnect step per pass, two passes at most. */
#define DRIVE_MAX_STEPS (8u * GOVEE_MAX_LAMPS + 16u)

/* ------------------------------------------------------------------------- */
/* Small helpers                                                             */
/* ------------------------------------------------------------------------- */

static uint32_t now(const GoveeEngine* e) {
    return e->ops->now_ms(e->ctx);
}

static void rec(GoveeEngine* e, GoveeEventType type, uint8_t lamp, uint32_t arg) {
    GoveeEngineEvent* ev = &e->ring[e->ring_total % GOVEE_ENGINE_EVENT_RING];
    ev->t_ms = now(e);
    ev->arg = arg;
    ev->type = (uint8_t)type;
    ev->lamp = lamp;
    e->ring_total++;
}

static void notify(GoveeEngine* e) {
    if(e->ops->changed) {
        e->in_op = true;
        e->ops->changed(e->ctx);
        e->in_op = false;
    }
}

static void arm(GoveeEngine* e, uint32_t ms) {
    e->timer_gen++;
    e->in_op = true;
    e->ops->timer_start(e->ctx, ms, e->timer_gen);
    e->in_op = false;
}

static void disarm(GoveeEngine* e) {
    e->timer_gen++;
    e->in_op = true;
    e->ops->timer_stop(e->ctx);
    e->in_op = false;
}

static uint8_t find_lamp(const GoveeEngine* e, const GoveeLampId* id) {
    for(uint8_t i = 0; i < e->lamp_count && i < GOVEE_MAX_LAMPS; i++) {
        if(govee_lamp_id_equal(&e->lamps[i].id, id)) return i;
    }
    return GOVEE_LAMP_NONE;
}

static void clamp_desired(GoveeDesired* d) {
    if(d->brightness < GOVEE_BRIGHTNESS_MIN) d->brightness = GOVEE_BRIGHTNESS_MIN;
    if(d->brightness > GOVEE_BRIGHTNESS_MAX) d->brightness = GOVEE_BRIGHTNESS_MAX;
    if(d->kelvin < GOVEE_KELVIN_MIN) d->kelvin = GOVEE_KELVIN_MIN;
    if(d->kelvin > GOVEE_KELVIN_MAX) d->kelvin = GOVEE_KELVIN_MAX;
    if(d->mode != GoveeModeColour) d->mode = GoveeModeWhite;
    d->rgb &= 0xFFFFFFu;
}

static bool desired_equal(const GoveeDesired* a, const GoveeDesired* b) {
    return a->power == b->power && a->brightness == b->brightness && a->mode == b->mode &&
           a->kelvin == b->kelvin && a->rgb == b->rgb;
}

/* Bit mask of properties whose last submitted value differs from d. With
 * power off only power is compared: brightness and colour are not sent to a
 * lamp that is being switched off, and are sent after the next power on. */
static uint8_t diff_mask(const GoveeEngineLamp* l, const GoveeDesired* d) {
    uint8_t m = 0;
    if(!l->known[GoveePropPower] || l->power != d->power) m |= (uint8_t)(1u << GoveePropPower);
    if(d->power) {
        if(!l->known[GoveePropBrightness] || l->brightness != d->brightness) {
            m |= (uint8_t)(1u << GoveePropBrightness);
        }
        bool colour_differs = !l->known[GoveePropColour] || l->mode != d->mode ||
                              (d->mode == GoveeModeWhite ? l->kelvin != d->kelvin :
                                                           l->rgb != d->rgb);
        if(colour_differs) m |= (uint8_t)(1u << GoveePropColour);
    }
    return m;
}

static bool eligible(const GoveeEngine* e, uint8_t i) {
    const GoveeEngineLamp* l = &e->lamps[i];
    return l->targeted && l->attempt_rev != e->rev && l->reached_rev != e->rev &&
           diff_mask(l, &e->desired) != 0;
}

static uint8_t targeted_count(const GoveeEngine* e) {
    uint8_t n = 0;
    for(uint8_t i = 0; i < e->lamp_count; i++) {
        if(e->lamps[i].targeted) n++;
    }
    return n;
}

static void set_targets(GoveeEngine* e) {
    for(uint8_t i = 0; i < e->lamp_count; i++) {
        e->lamps[i].targeted = (e->target == 0 || e->target > e->lamp_count) ?
                                   true :
                                   (i == (uint8_t)(e->target - 1u));
    }
}

/* ------------------------------------------------------------------------- */
/* Lamp list changes (applied at a lamp boundary)                            */
/* ------------------------------------------------------------------------- */

static bool id_in(const GoveeLampId* id, const GoveeLampId* ids, uint8_t from, uint8_t to) {
    for(uint8_t i = from; i < to; i++) {
        if(govee_lamp_id_equal(id, &ids[i])) return true;
    }
    return false;
}

/* Reorder e->lamps to match ids, keeping per-lamp records by identity,
 * without a second lamp array. */
static void remap_lamps(GoveeEngine* e, const GoveeLampId* ids, const uint8_t* last, uint8_t new_count) {
    uint8_t n_old = e->lamp_count;
    for(uint8_t k = 0; k < new_count; k++) {
        uint8_t j = GOVEE_LAMP_NONE;
        for(uint8_t m = k; m < n_old; m++) {
            if(govee_lamp_id_equal(&e->lamps[m].id, &ids[k])) {
                j = m;
                break;
            }
        }
        if(j != GOVEE_LAMP_NONE) {
            if(j != k) {
                GoveeEngineLamp tmp = e->lamps[k];
                e->lamps[k] = e->lamps[j];
                e->lamps[j] = tmp;
            }
            continue;
        }
        /* fresh record at k; keep the unplaced record at k if it may match later */
        if(k < n_old) {
            if(n_old < GOVEE_MAX_LAMPS) {
                e->lamps[n_old] = e->lamps[k];
                n_old++;
            } else {
                /* all slots used: some unplaced record matches none of the remaining
                 * ids (counting argument); overwrite that one instead */
                for(uint8_t m = k; m < n_old; m++) {
                    if(!id_in(&e->lamps[m].id, ids, (uint8_t)(k + 1u), new_count)) {
                        if(m != k) e->lamps[m] = e->lamps[k];
                        break;
                    }
                }
            }
        } else {
            n_old = (uint8_t)(k + 1u);
        }
        memset(&e->lamps[k], 0, sizeof(e->lamps[k]));
        e->lamps[k].id = ids[k];
        e->lamps[k].last = (last[k] <= GoveeLampLastFailed) ? last[k] : GoveeLampLastUnknown;
    }
    for(uint8_t k = new_count; k < GOVEE_MAX_LAMPS; k++) {
        memset(&e->lamps[k], 0, sizeof(e->lamps[k]));
    }
    e->lamp_count = new_count;
}

static void apply_pending(GoveeEngine* e) {
    if(!e->pending_valid) return;
    e->pending_valid = false;

    GoveeLampId last_id;
    GoveeLampId visit_id;
    bool has_last = e->last_visited < e->lamp_count;
    bool has_visit = e->visit_lamp < e->lamp_count;
    if(has_last) last_id = e->lamps[e->last_visited].id;
    if(has_visit) visit_id = e->lamps[e->visit_lamp].id;
    uint8_t last_pos = e->last_visited;

    remap_lamps(e, e->pending_ids, e->pending_last, e->pending_count);
    /* records keep their old targeted flag until set_targets below */
    uint32_t was_targeted = 0;
    for(uint8_t i = 0; i < e->lamp_count; i++) {
        if(e->lamps[i].targeted) was_targeted |= (uint32_t)1u << i;
    }
    e->target = e->pending_target;
    set_targets(e);
    if(e->pending_resend) {
        /* apply_again came while this change was pending: it reset only the lamps
         * targeted then; a lamp targeted only from now on gets the full state too */
        e->pending_resend = false;
        for(uint8_t i = 0; i < e->lamp_count; i++) {
            if(e->lamps[i].targeted && !(was_targeted & ((uint32_t)1u << i))) {
                for(uint8_t p = 0; p < GoveePropCount; p++) e->lamps[i].known[p] = false;
            }
        }
    }

    e->link_lamp = e->link_open ? find_lamp(e, &e->link_id) : GOVEE_LAMP_NONE;
    e->visit_lamp = has_visit ? find_lamp(e, &visit_id) : GOVEE_LAMP_NONE;
    if(has_visit && e->visit_lamp == GOVEE_LAMP_NONE) e->visit_closing = false;
    if(has_last) {
        uint8_t f = find_lamp(e, &last_id);
        e->last_visited = (f != GOVEE_LAMP_NONE) ?
                              f :
                              ((last_pos < e->lamp_count && last_pos > 0) ? (uint8_t)(last_pos - 1u) :
                                                                            GOVEE_LAMP_NONE);
    } else {
        e->last_visited = GOVEE_LAMP_NONE;
    }
    rec(e, GoveeEvLamps, GOVEE_LAMP_NONE, e->lamp_count);
}

/* ------------------------------------------------------------------------- */
/* Pass and visit bookkeeping                                                */
/* ------------------------------------------------------------------------- */

static void start_pass(GoveeEngine* e) {
    e->in_pass = true;
    e->pass_no++;
    e->pass_start_ms = now(e);
    for(uint8_t i = 0; i < GOVEE_MAX_LAMPS; i++) e->lamps[i].visited = false;
    rec(e, GoveeEvPassStart, GOVEE_LAMP_NONE, e->pass_no);
}

static void end_pass(GoveeEngine* e) {
    if(!e->in_pass) return;
    e->in_pass = false;
    for(uint8_t i = 0; i < GOVEE_MAX_LAMPS; i++) e->lamps[i].visited = false;
    rec(e, GoveeEvPassEnd, GOVEE_LAMP_NONE, now(e) - e->pass_start_ms);
    notify(e);
}

static bool any_eligible(const GoveeEngine* e) {
    for(uint8_t i = 0; i < e->lamp_count; i++) {
        if(eligible(e, i)) return true;
    }
    return false;
}

/* The lamp's last attempt failed; before its first attempt in this
 * run the result saved by the previous run counts. */
static bool lamp_failed(const GoveeEngineLamp* l) {
    if(l->outcome == GoveeOutcomeNone) return l->last == GoveeLampLastFailed;
    return govee_engine_outcome_is_failure((GoveeOutcome)l->outcome);
}

/* Lamps whose last attempt succeeded or that were never tried come
 * before lamps whose last attempt failed; within each group the order is fair
 * (starting after the lamp visited last). */
static uint8_t pick_next(const GoveeEngine* e) {
    uint8_t n = e->lamp_count;
    if(n == 0) return GOVEE_LAMP_NONE;
    uint8_t start = (e->last_visited < n) ? (uint8_t)((e->last_visited + 1u) % n) : 0;
    for(uint8_t group = 0; group < 2; group++) {
        for(uint8_t k = 0; k < n; k++) {
            uint8_t i = (uint8_t)((start + k) % n);
            bool failed = lamp_failed(&e->lamps[i]);
            if(failed != (group == 1u)) continue;
            if(!e->lamps[i].visited && eligible(e, i)) return i;
        }
    }
    return GOVEE_LAMP_NONE;
}

/* Lamps whose differing set is empty reach the revision without a visit. */
static void refresh_unchanged(GoveeEngine* e) {
    for(uint8_t i = 0; i < e->lamp_count; i++) {
        GoveeEngineLamp* l = &e->lamps[i];
        if(l->targeted && l->reached_rev != e->rev && diff_mask(l, &e->desired) == 0) {
            l->reached_rev = e->rev;
            l->attempt_rev = e->rev;
            l->outcome = GoveeOutcomeUnchanged;
            memset(&l->times, 0, sizeof(l->times));
            rec(e, GoveeEvUnchanged, i, e->rev);
        }
    }
}

static void begin_visit(GoveeEngine* e, uint8_t i, bool reuse) {
    GoveeEngineLamp* l = &e->lamps[i];
    e->visit_lamp = i;
    e->visit_rev = e->rev;
    e->visit_desired = e->desired;
    e->next_prop = 0;
    e->visit_ok = false;
    e->visit_start_ms = now(e);
    e->phase_start_ms = e->visit_start_ms;
    l->visited = true;
    e->visit_closing = false;
    e->visit_retry = lamp_failed(l);
    e->visit_connected = reuse;
    e->visit_relinked = false;
    e->visit_prev_last = l->last;
    l->attempt_rev = e->rev;
    l->outcome = GoveeOutcomeNone;
    memset(&l->times, 0, sizeof(l->times));
    l->times.reused = reuse;
    rec(e, GoveeEvVisit, i, e->rev);
}

/* Record the result of the current visit. The link may still be open. */
static void finish_visit(GoveeEngine* e, GoveeOutcome outcome) {
    if(e->visit_lamp >= e->lamp_count) return;
    GoveeEngineLamp* l = &e->lamps[e->visit_lamp];
    l->outcome = (uint8_t)outcome;
    l->times.total_ms = now(e) - e->visit_start_ms;
    if(outcome != GoveeOutcomeAborted) {
        l->last = (outcome == GoveeOutcomeSubmitted) ? GoveeLampLastOk : GoveeLampLastFailed;
    }
    if(outcome == GoveeOutcomeSubmitted) {
        l->reached_rev = e->visit_rev;
        e->visit_ok = true;
        rec(e, GoveeEvSubmitted, e->visit_lamp, e->visit_rev);
    }
    e->last_visited = e->visit_lamp;
    e->visit_closing = true;
    notify(e);
}

static void link_closed(GoveeEngine* e) {
    e->link_open = false;
    e->link_lamp = GOVEE_LAMP_NONE;
}

/* The visit total covers the cancel and the tail before a kept or closed link. */
static void update_visit_total(GoveeEngine* e) {
    if(!e->visit_closing || e->visit_lamp >= e->lamp_count) return;
    e->lamps[e->visit_lamp].times.total_ms = now(e) - e->visit_start_ms;
}

static void enter_fault(GoveeEngine* e, GoveeEngineFault f) {
    e->fault = (uint8_t)f;
    e->restart_pass = false;
    rec(e, GoveeEvFault, GOVEE_LAMP_NONE, (uint32_t)f);
    disarm(e);
    end_pass(e);
    if(e->stop_requested) {
        e->state = GoveeEngineStopped;
        rec(e, GoveeEvStopped, GOVEE_LAMP_NONE, 0);
    } else {
        e->state = GoveeEngineFaulted;
    }
    notify(e);
}

/* The visit's link ended after Connected and before the visit's first
 * write was accepted: connect the same lamp once more within the same visit
 * (state S_CONNECT). Returns false when that does not apply. */
static bool early_loss_retry(GoveeEngine* e) {
    if(!e->visit_connected || e->visit_relinked || e->visit_ok || e->stop_requested) return false;
    if(e->visit_lamp >= e->lamp_count || e->link_lamp != e->visit_lamp) return false;
    GoveeEngineLamp* l = &e->lamps[e->visit_lamp];
    if(l->times.writes != 0) return false;
    rec(e, GoveeEvLinkRetry, e->visit_lamp, e->link_gen);
    e->visit_relinked = true;
    e->visit_connected = false;
    e->visit_closing = false;
    l->outcome = GoveeOutcomeNone;
    /* A failure recorded before the retry (a rejected write on a lost kept link)
     * is not a completed attempt; keep the previous result until the retry ends, so a
     * retry aborted by stop leaves the same last result as a directly reported loss. */
    l->last = e->visit_prev_last;
    l->times.reused = false;
    link_closed(e);
    e->state = S_CONNECT;
    return true;
}

/* Issue a disconnect. Returns false if there was nothing to disconnect
 * (state is then S_BOUNDARY, or S_CONNECT for an early-loss retry, and the caller
 * continues the drive loop). */
static bool start_disconnect(GoveeEngine* e) {
    rec(e, GoveeEvDisconnect, e->link_lamp, e->link_gen);
    e->phase_start_ms = now(e);
    e->in_op = true;
    bool ok = e->ops->disconnect(e->ctx, e->link_gen);
    e->in_op = false;
    if(!ok) {
        if(early_loss_retry(e)) return false;
        /* The link had already ended; the visit total still covers the tail */
        if(e->link_lamp == e->visit_lamp) update_visit_total(e);
        e->visit_closing = false;
        link_closed(e);
        e->state = S_BOUNDARY;
        return false;
    }
    e->state = GoveeEngineDisconnecting;
    arm(e,
        e->stop_requested ? GOVEE_ENGINE_STOP_DISCONNECT_DEADLINE_MS :
                            GOVEE_ENGINE_DISCONNECT_DEADLINE_MS);
    return true;
}

static void start_cancel(GoveeEngine* e) {
    e->phase_start_ms = now(e);
    e->in_op = true;
    bool ok = e->ops->cancel_connect(e->ctx, e->link_gen);
    e->in_op = false;
    if(ok) {
        e->state = GoveeEngineCancelling;
        arm(e, GOVEE_ENGINE_CANCEL_DEADLINE_MS);
        return;
    }
    rec(e, GoveeEvCancelFailed, e->visit_lamp, 0);
    if(e->visit_lamp < e->lamp_count) e->lamps[e->visit_lamp].outcome = GoveeOutcomeCancelFailed;
    enter_fault(e, GoveeFaultCancel);
}

static bool keep_allowed(const GoveeEngine* e) {
    if(e->stop_requested || e->link_lamp >= e->lamp_count) return false;
    if(targeted_count(e) != 1 || !e->lamps[e->link_lamp].targeted) return false;
    return (now(e) - e->link_connected_ms) < GOVEE_ENGINE_KEEP_MAX_MS;
}

static void build_packet(const GoveeDesired* d, uint8_t prop, uint8_t pkt[GOVEE_PACKET_SIZE]) {
    switch(prop) {
    case GoveePropPower:
        govee_h6006_create_power_packet(pkt, d->power);
        break;
    case GoveePropBrightness:
        govee_h6006_create_brightness_packet(pkt, d->brightness);
        break;
    default:
        if(d->mode == GoveeModeWhite) {
            govee_h6006_create_white_packet(pkt, d->kelvin);
        } else {
            govee_h6006_create_color_packet(
                pkt,
                (uint8_t)((d->rgb >> 16) & 0xFFu),
                (uint8_t)((d->rgb >> 8) & 0xFFu),
                (uint8_t)(d->rgb & 0xFFu));
        }
        break;
    }
}

static void mark_submitted(GoveeEngineLamp* l, const GoveeDesired* d, uint8_t prop) {
    l->known[prop] = true;
    if(prop == GoveePropPower) {
        l->power = d->power;
    } else if(prop == GoveePropBrightness) {
        l->brightness = d->brightness;
    } else {
        l->mode = d->mode;
        l->kelvin = d->kelvin;
        l->rgb = d->rgb;
    }
}

/* ------------------------------------------------------------------------- */
/* The driver: runs transient states until the engine waits for an event.   */
/* ------------------------------------------------------------------------- */

/* Connect to the visited lamp (first connect of a visit, or the early-loss retry). */
static void step_connect(GoveeEngine* e, bool* again) {
    uint8_t next = e->visit_lamp;
    *again = false;
    e->phase_start_ms = now(e);
    e->link_gen++;
    e->link_id = e->lamps[next].id;
    e->link_lamp = next;
    rec(e, GoveeEvConnect, next, e->link_gen);
    e->in_op = true;
    bool ok = e->ops->connect(e->ctx, &e->lamps[next].id, e->link_gen);
    e->in_op = false;
    if(ok) {
        e->link_open = true;
        e->state = GoveeEngineConnecting;
        arm(e,
            e->visit_retry ? GOVEE_ENGINE_CONNECT_RETRY_DEADLINE_MS :
                             GOVEE_ENGINE_CONNECT_DEADLINE_MS);
        return;
    }
    link_closed(e);
    rec(e, GoveeEvConnectFailed, next, 0);
    finish_visit(e, GoveeOutcomeConnectFailed);
    e->state = S_BOUNDARY;
    *again = true;
}

static void step_boundary(GoveeEngine* e, bool* again) {
    *again = false;
    apply_pending(e);
    refresh_unchanged(e);

    if(e->stop_requested) {
        if(e->link_open) {
            *again = !start_disconnect(e);
            return;
        }
        end_pass(e);
        disarm(e);
        e->state = GoveeEngineStopped;
        rec(e, GoveeEvStopped, GOVEE_LAMP_NONE, 0);
        notify(e);
        return;
    }

    uint8_t next = pick_next(e);
    if(next == GOVEE_LAMP_NONE && e->in_pass && any_eligible(e)) {
        end_pass(e); /* lamps changed during this pass: next pass right away */
        start_pass(e);
        next = pick_next(e);
    }
    if(next != GOVEE_LAMP_NONE && !e->in_pass) start_pass(e);

    if(e->link_open) {
        if(next != GOVEE_LAMP_NONE && next == e->link_lamp) {
            disarm(e); /* kept-link deadline no longer applies */
            begin_visit(e, next, true);
            rec(e, GoveeEvReuse, next, now(e) - e->link_connected_ms);
            e->state = S_WRITE_NEXT;
            *again = true;
            return;
        }
        if(next == GOVEE_LAMP_NONE && keep_allowed(e)) {
            if(e->link_lamp == e->visit_lamp) update_visit_total(e);
            end_pass(e);
            uint32_t age = now(e) - e->link_connected_ms;
            e->state = GoveeEngineKept;
            e->visit_closing = false;
            rec(e, GoveeEvKeep, e->link_lamp, age);
            arm(e, GOVEE_ENGINE_KEEP_MAX_MS - age);
            notify(e);
            return;
        }
        *again = !start_disconnect(e);
        return;
    }

    if(next == GOVEE_LAMP_NONE) {
        end_pass(e);
        disarm(e);
        e->state = GoveeEngineIdle;
        notify(e);
        return;
    }

    begin_visit(e, next, false);
    step_connect(e, again);
}

static void step_write_next(GoveeEngine* e, bool* again) {
    *again = false;
    GoveeEngineLamp* l = &e->lamps[e->visit_lamp];
    uint8_t mask = diff_mask(l, &e->visit_desired);
    uint8_t prop = GoveePropCount;
    for(uint8_t p = e->next_prop; p < GoveePropCount; p++) {
        if(mask & (1u << p)) {
            prop = p;
            break;
        }
    }
    uint32_t t = now(e);
    if(prop == GoveePropCount) {
        /* nothing (more) to send */
        if(l->times.writes > 0) l->times.write_ms = t - e->write_start_ms;
        finish_visit(e, GoveeOutcomeSubmitted);
        e->state = GoveeEngineTail;
        arm(e, GOVEE_ENGINE_TAIL_MS);
        return;
    }

    uint8_t pkt[GOVEE_PACKET_SIZE];
    build_packet(&e->visit_desired, prop, pkt);
    if(l->times.writes == 0) e->write_start_ms = t;
    e->in_op = true;
    bool ok = e->ops->write(e->ctx, e->link_gen, pkt, GOVEE_PACKET_SIZE);
    e->in_op = false;
    if(!ok) {
        rec(e, GoveeEvWriteRejected, e->visit_lamp, prop);
        l->times.write_ms = now(e) - e->write_start_ms;
        finish_visit(e, GoveeOutcomeWriteRejected);
        e->state = S_DISCONNECT;
        *again = true;
        return;
    }
    mark_submitted(l, &e->visit_desired, prop);
    l->times.writes++;
    rec(e, GoveeEvWrite, e->visit_lamp, prop);
    e->next_prop = (uint8_t)(prop + 1u);

    uint8_t rest = (uint8_t)(mask & ~((1u << (prop + 1u)) - 1u));
    if(rest == 0) {
        l->times.write_ms = now(e) - e->write_start_ms;
        finish_visit(e, GoveeOutcomeSubmitted);
        e->state = GoveeEngineTail;
        arm(e, GOVEE_ENGINE_TAIL_MS);
        return;
    }
    e->state = GoveeEngineWriteGap;
    bool power_on = (prop == GoveePropPower) && e->visit_desired.power;
    arm(e, power_on ? GOVEE_ENGINE_GAP_AFTER_POWER_ON_MS : GOVEE_ENGINE_GAP_MS);
}

static void drive(GoveeEngine* e) {
    for(uint32_t steps = 0; steps < DRIVE_MAX_STEPS; steps++) {
        bool again = false;
        switch(e->state) {
        case S_BOUNDARY:
            step_boundary(e, &again);
            break;
        case S_WRITE_NEXT:
            step_write_next(e, &again);
            break;
        case S_DISCONNECT:
            again = !start_disconnect(e);
            break;
        case S_CONNECT:
            step_connect(e, &again);
            break;
        default:
            return;
        }
        if(!again && e->state < GoveeEngineStateCount) return;
    }
    enter_fault(e, GoveeFaultInternal);
}

static uint8_t visit_last(const GoveeEngine* e) {
    return e->visit_lamp < e->lamp_count ? e->lamps[e->visit_lamp].last : GoveeLampLastUnknown;
}

/* A change while connecting to a lamp whose last attempt failed cancels
 * that attempt at once; the next pass starts when the cancel has resolved. */
static void supersede_retry(GoveeEngine* e) {
    if(e->state != GoveeEngineConnecting || !e->visit_retry) return;
    if(e->visit_lamp < e->lamp_count) {
        e->lamps[e->visit_lamp].times.connect_ms = now(e) - e->phase_start_ms;
    }
    rec(e, GoveeEvConnectSuperseded, e->visit_lamp, e->link_gen);
    uint8_t last = visit_last(e);
    finish_visit(e, GoveeOutcomeConnectTimeout);
    if(e->visit_lamp < e->lamp_count) e->lamps[e->visit_lamp].last = last; /* not a completed attempt */
    e->restart_pass = true;
    start_cancel(e);
}

/* Start work if the engine is waiting for input. */
static void kick(GoveeEngine* e) {
    if(e->state == GoveeEngineIdle || e->state == GoveeEngineKept) {
        e->state = S_BOUNDARY;
        drive(e);
    }
}

/* ------------------------------------------------------------------------- */
/* Public commands                                                           */
/* ------------------------------------------------------------------------- */

#define REENTRY_GUARD(e, ret)                                    \
    do {                                                         \
        if((e)->in_op) {                                         \
            rec((e), GoveeEvReentry, GOVEE_LAMP_NONE, 0);        \
            return ret;                                          \
        }                                                        \
    } while(0)

void govee_engine_init(GoveeEngine* e, const GoveeEngineOps* ops, void* ctx, const GoveeDesired* initial) {
    memset(e, 0, sizeof(*e));
    e->ops = ops;
    e->ctx = ctx;
    e->state = GoveeEngineIdle;
    e->visit_lamp = GOVEE_LAMP_NONE;
    e->link_lamp = GOVEE_LAMP_NONE;
    e->last_visited = GOVEE_LAMP_NONE;
    if(initial) {
        e->desired = *initial;
    } else {
        e->desired.power = true;
        e->desired.brightness = GOVEE_BRIGHTNESS_MAX;
        e->desired.mode = GoveeModeWhite;
        e->desired.kelvin = GOVEE_KELVIN_MIN;
    }
    clamp_desired(&e->desired);
}

uint32_t govee_engine_set_desired(GoveeEngine* e, const GoveeDesired* d) {
    REENTRY_GUARD(e, e->rev);
    GoveeDesired n = *d;
    clamp_desired(&n);
    if(desired_equal(&n, &e->desired)) return e->rev;
    e->desired = n;
    e->rev++;
    rec(e, GoveeEvDesired, GOVEE_LAMP_NONE, e->rev);
    supersede_retry(e);
    kick(e);
    return e->rev;
}

uint32_t govee_engine_apply_again(GoveeEngine* e) {
    REENTRY_GUARD(e, e->rev);
    e->rev++;
    for(uint8_t i = 0; i < e->lamp_count; i++) {
        if(e->lamps[i].targeted) {
            for(uint8_t p = 0; p < GoveePropCount; p++) e->lamps[i].known[p] = false;
        }
    }
    /* a pending lamp list or target change is applied later; its newly targeted
     * lamps are reset when it is applied */
    if(e->pending_valid) e->pending_resend = true;
    rec(e, GoveeEvApplyAgain, GOVEE_LAMP_NONE, e->rev);
    supersede_retry(e);
    kick(e);
    return e->rev;
}

void govee_engine_set_lamps(GoveeEngine* e, const GoveeLampId* ids, uint8_t count, uint8_t target) {
    govee_engine_set_lamps_last(e, ids, NULL, count, target);
}

void govee_engine_set_lamps_last(
    GoveeEngine* e,
    const GoveeLampId* ids,
    const uint8_t* last,
    uint8_t count,
    uint8_t target) {
    REENTRY_GUARD(e, );
    if(count > GOVEE_MAX_LAMPS) count = GOVEE_MAX_LAMPS;
    for(uint8_t i = 0; i < count; i++) {
        e->pending_ids[i] = ids[i];
        e->pending_last[i] = last ? last[i] : (uint8_t)GoveeLampLastUnknown;
    }
    e->pending_count = count;
    e->pending_target = (target > count) ? 0 : target;
    e->pending_valid = true;

    switch(e->state) {
    case GoveeEngineIdle:
    case GoveeEngineKept:
        kick(e);
        break;
    case GoveeEngineFaulted:
    case GoveeEngineStopped:
        apply_pending(e);
        break;
    default:
        rec(e, GoveeEvLampsPending, GOVEE_LAMP_NONE, count);
        break;
    }
}

void govee_engine_stop(GoveeEngine* e) {
    REENTRY_GUARD(e, );
    if(e->state == GoveeEngineStopped) return;
    rec(e, GoveeEvStop, GOVEE_LAMP_NONE, e->state);
    bool first_stop = !e->stop_requested;
    e->stop_requested = true;
    switch(e->state) {
    case GoveeEngineIdle:
    case GoveeEngineFaulted:
        disarm(e);
        end_pass(e);
        e->state = GoveeEngineStopped;
        rec(e, GoveeEvStopped, GOVEE_LAMP_NONE, 0);
        notify(e);
        break;
    case GoveeEngineKept:
    case GoveeEngineTail:
        e->state = S_DISCONNECT;
        drive(e);
        break;
    case GoveeEngineConnecting:
        if(e->visit_lamp < e->lamp_count) {
            e->lamps[e->visit_lamp].times.connect_ms = now(e) - e->phase_start_ms;
        }
        finish_visit(e, GoveeOutcomeAborted);
        start_cancel(e);
        break;
    case GoveeEngineDiscovering:
    case GoveeEngineWriteGap:
        finish_visit(e, GoveeOutcomeAborted);
        e->state = S_DISCONNECT;
        drive(e);
        break;
    case GoveeEngineDisconnecting: {
        /* A disconnect already running gets the stop bound, never more than it had left */
        if(!first_stop) break;
        uint32_t spent = now(e) - e->phase_start_ms;
        uint32_t left = spent < GOVEE_ENGINE_DISCONNECT_DEADLINE_MS ?
                            GOVEE_ENGINE_DISCONNECT_DEADLINE_MS - spent :
                            0u;
        arm(e, left < GOVEE_ENGINE_STOP_DISCONNECT_DEADLINE_MS ? left :
                                                                 GOVEE_ENGINE_STOP_DISCONNECT_DEADLINE_MS);
        break;
    }
    default:
        /* Cancelling (and Disconnecting): completion leads to the boundary, which stops */
        break;
    }
}

void govee_engine_link_reset(GoveeEngine* e) {
    REENTRY_GUARD(e, );
    if(e->state != GoveeEngineFaulted) return;
    link_closed(e);
    e->fault = GoveeFaultNone;
    rec(e, GoveeEvLinkReset, GOVEE_LAMP_NONE, 0);
    e->state = S_BOUNDARY;
    drive(e);
}

bool govee_engine_release_link(GoveeEngine* e) {
    REENTRY_GUARD(e, false);
    if(e->state != GoveeEngineKept) return false;
    e->state = S_DISCONNECT;
    drive(e);
    return true;
}

void govee_engine_clear_events(GoveeEngine* e) {
    memset(e->ring, 0, sizeof(e->ring));
    e->ring_total = 0;
}

/* ------------------------------------------------------------------------- */
/* Events                                                                    */
/* ------------------------------------------------------------------------- */

static bool link_gen_ok(GoveeEngine* e, uint32_t gen) {
    if(gen != e->link_gen) {
        rec(e, GoveeEvStale, GOVEE_LAMP_NONE, gen);
        return false;
    }
    return true;
}

static void unexpected(GoveeEngine* e) {
    rec(e, GoveeEvUnexpected, GOVEE_LAMP_NONE, e->state);
}

static uint32_t phase_ms(GoveeEngine* e) {
    return now(e) - e->phase_start_ms;
}

static GoveeEngineLamp* visit(GoveeEngine* e) {
    return &e->lamps[e->visit_lamp < GOVEE_MAX_LAMPS ? e->visit_lamp : 0];
}

void govee_engine_on_connected(GoveeEngine* e, uint32_t link_gen) {
    REENTRY_GUARD(e, );
    if(!link_gen_ok(e, link_gen)) return;
    if(e->state != GoveeEngineConnecting) {
        unexpected(e);
        return;
    }
    uint32_t ms = phase_ms(e);
    visit(e)->times.connect_ms = ms;
    e->link_connected_ms = now(e);
    e->visit_connected = true;
    rec(e, GoveeEvConnected, e->visit_lamp, ms);

    e->phase_start_ms = now(e);
    rec(e, GoveeEvDiscover, e->visit_lamp, e->link_gen);
    e->in_op = true;
    bool ok = e->ops->discover(e->ctx, e->link_gen);
    e->in_op = false;
    if(ok) {
        e->state = GoveeEngineDiscovering;
        arm(e, GOVEE_ENGINE_DISCOVER_DEADLINE_MS);
        return;
    }
    rec(e, GoveeEvDiscoveryFailed, e->visit_lamp, 0);
    finish_visit(e, GoveeOutcomeDiscoveryFailed);
    e->state = S_DISCONNECT;
    drive(e);
}

void govee_engine_on_discovery(GoveeEngine* e, uint32_t link_gen, bool found) {
    REENTRY_GUARD(e, );
    if(!link_gen_ok(e, link_gen)) return;
    if(e->state != GoveeEngineDiscovering) {
        unexpected(e);
        return;
    }
    uint32_t ms = phase_ms(e);
    visit(e)->times.discover_ms = ms;
    if(found) {
        rec(e, GoveeEvDiscovered, e->visit_lamp, ms);
        e->state = S_WRITE_NEXT;
    } else {
        rec(e, GoveeEvDiscoveryFailed, e->visit_lamp, ms);
        finish_visit(e, GoveeOutcomeDiscoveryFailed);
        e->state = S_DISCONNECT;
    }
    drive(e);
}

void govee_engine_on_disconnected(GoveeEngine* e, uint32_t link_gen) {
    REENTRY_GUARD(e, );
    if(!link_gen_ok(e, link_gen)) return;
    uint32_t ms = phase_ms(e);
    switch(e->state) {
    case GoveeEngineConnecting:
        visit(e)->times.connect_ms = ms;
        rec(e, GoveeEvConnectFailed, e->visit_lamp, ms);
        link_closed(e);
        finish_visit(e, GoveeOutcomeConnectFailed);
        break;
    case GoveeEngineCancelling:
        rec(e, GoveeEvCancelDone, e->visit_lamp, ms);
        update_visit_total(e);
        link_closed(e);
        if(e->restart_pass) {
            e->restart_pass = false;
            end_pass(e); /* the boundary starts the next pass with the lamps that answer */
        }
        break;
    case GoveeEngineDiscovering:
    case GoveeEngineWriteGap:
        rec(e, GoveeEvLinkLost, e->visit_lamp, e->state);
        if(e->state == GoveeEngineDiscovering) visit(e)->times.discover_ms = ms;
        if(early_loss_retry(e)) {
            drive(e);
            return;
        }
        link_closed(e);
        finish_visit(e, GoveeOutcomeLinkLost);
        break;
    case GoveeEngineTail:
    case GoveeEngineKept:
        rec(e, GoveeEvLinkLost, e->link_lamp, e->state);
        if(e->state == GoveeEngineTail && e->link_lamp == e->visit_lamp) update_visit_total(e);
        link_closed(e);
        break;
    case GoveeEngineDisconnecting:
        rec(e, GoveeEvDisconnected, e->link_lamp, ms);
        if(e->visit_closing && e->visit_lamp < e->lamp_count && e->link_lamp == e->visit_lamp) {
            e->lamps[e->visit_lamp].times.disconnect_ms = ms;
            update_visit_total(e);
        }
        e->visit_closing = false;
        link_closed(e);
        break;
    default:
        unexpected(e);
        return;
    }
    e->state = S_BOUNDARY;
    drive(e);
}

void govee_engine_on_link_error(GoveeEngine* e, uint32_t link_gen) {
    REENTRY_GUARD(e, );
    if(!link_gen_ok(e, link_gen)) return;
    uint32_t ms = phase_ms(e);
    switch(e->state) {
    case GoveeEngineConnecting:
        visit(e)->times.connect_ms = ms;
        rec(e, GoveeEvConnectFailed, e->visit_lamp, ms);
        link_closed(e);
        finish_visit(e, GoveeOutcomeConnectFailed);
        e->state = S_BOUNDARY;
        break;
    case GoveeEngineCancelling:
        rec(e, GoveeEvCancelFailed, e->visit_lamp, ms);
        visit(e)->outcome = GoveeOutcomeCancelFailed;
        update_visit_total(e);
        enter_fault(e, GoveeFaultCancel);
        return;
    case GoveeEngineDiscovering:
        visit(e)->times.discover_ms = ms;
        rec(e, GoveeEvDiscoveryFailed, e->visit_lamp, ms);
        finish_visit(e, GoveeOutcomeDiscoveryFailed);
        e->state = S_DISCONNECT;
        break;
    case GoveeEngineWriteGap:
        rec(e, GoveeEvLinkLost, e->visit_lamp, e->state);
        finish_visit(e, GoveeOutcomeLinkLost);
        e->state = S_DISCONNECT;
        break;
    case GoveeEngineTail:
    case GoveeEngineKept:
        e->state = S_DISCONNECT;
        break;
    default:
        /* Disconnecting: keep waiting for Disconnected or the deadline */
        unexpected(e);
        return;
    }
    drive(e);
}

void govee_engine_on_timer(GoveeEngine* e, uint32_t timer_gen) {
    REENTRY_GUARD(e, );
    if(timer_gen != e->timer_gen) {
        rec(e, GoveeEvStale, GOVEE_LAMP_NONE, timer_gen);
        return;
    }
    uint32_t ms = phase_ms(e);
    switch(e->state) {
    case GoveeEngineConnecting:
        visit(e)->times.connect_ms = ms;
        rec(e, GoveeEvConnectTimeout, e->visit_lamp, e->link_gen);
        finish_visit(e, GoveeOutcomeConnectTimeout);
        start_cancel(e);
        return;
    case GoveeEngineCancelling:
        rec(e, GoveeEvCancelFailed, e->visit_lamp, ms);
        visit(e)->outcome = GoveeOutcomeCancelFailed;
        update_visit_total(e);
        enter_fault(e, GoveeFaultCancel);
        return;
    case GoveeEngineDiscovering:
        visit(e)->times.discover_ms = ms;
        rec(e, GoveeEvDiscoveryFailed, e->visit_lamp, ms);
        finish_visit(e, GoveeOutcomeDiscoveryFailed);
        e->state = S_DISCONNECT;
        break;
    case GoveeEngineWriteGap:
        e->state = S_WRITE_NEXT;
        break;
    case GoveeEngineTail:
        e->state = S_BOUNDARY;
        break;
    case GoveeEngineDisconnecting:
        rec(e, GoveeEvDisconnectTimeout, e->link_lamp, e->link_gen);
        enter_fault(e, GoveeFaultDisconnect);
        return;
    case GoveeEngineKept:
        e->state = S_DISCONNECT;
        break;
    default:
        unexpected(e);
        return;
    }
    drive(e);
}

/* ------------------------------------------------------------------------- */
/* Getters and names                                                         */
/* ------------------------------------------------------------------------- */

GoveeEngineState govee_engine_state(const GoveeEngine* e) {
    return e->state < GoveeEngineStateCount ? (GoveeEngineState)e->state : GoveeEngineIdle;
}

bool govee_engine_is_stopped(const GoveeEngine* e) {
    return e->state == GoveeEngineStopped;
}

bool govee_engine_is_busy(const GoveeEngine* e) {
    return e->in_pass || e->link_open ||
           !(e->state == GoveeEngineIdle || e->state == GoveeEngineStopped ||
             e->state == GoveeEngineFaulted);
}

uint32_t govee_engine_revision(const GoveeEngine* e) {
    return e->rev;
}

uint8_t govee_engine_targeted_count(const GoveeEngine* e) {
    return targeted_count(e);
}

uint8_t govee_engine_find_lamp(const GoveeEngine* e, const GoveeLampId* id) {
    return find_lamp(e, id);
}

const GoveeEngineLamp* govee_engine_lamp(const GoveeEngine* e, uint8_t index) {
    return index < e->lamp_count ? &e->lamps[index] : NULL;
}

uint32_t govee_engine_event_count(const GoveeEngine* e) {
    return e->ring_total < GOVEE_ENGINE_EVENT_RING ? e->ring_total : GOVEE_ENGINE_EVENT_RING;
}

const GoveeEngineEvent* govee_engine_event(const GoveeEngine* e, uint32_t i, uint32_t* seq) {
    uint32_t n = govee_engine_event_count(e);
    if(i >= n) return NULL;
    uint32_t first = e->ring_total - n; /* sequence index (0-based) of the oldest kept */
    uint32_t s = first + i;
    if(seq) *seq = s + 1u;
    return &e->ring[s % GOVEE_ENGINE_EVENT_RING];
}

const char* govee_engine_state_name(GoveeEngineState s) {
    static const char* const names[GoveeEngineStateCount] = {
        "idle",
        "connecting",
        "cancelling",
        "discovering",
        "write_gap",
        "tail",
        "disconnecting",
        "kept",
        "faulted",
        "stopped",
    };
    return (unsigned)s < GoveeEngineStateCount ? names[s] : "unknown";
}

const char* govee_engine_outcome_name(GoveeOutcome o) {
    static const char* const names[GoveeOutcomeCount] = {
        "none",
        "submitted",
        "unchanged",
        "connect_failed",
        "connect_timeout",
        "cancel_failed",
        "discovery_failed",
        "write_rejected",
        "link_lost",
        "aborted",
    };
    return (unsigned)o < GoveeOutcomeCount ? names[o] : "unknown";
}

const char* govee_engine_event_name(GoveeEventType t) {
    static const char* const names[GoveeEvCount] = {
        "none",          "desired",        "apply_again",      "lamps",
        "lamps_pending", "pass_start",     "pass_end",         "visit",
        "connect",       "connected",      "connect_failed",   "connect_timeout",
        "cancel_done",   "cancel_failed",  "discover",         "discovered",
        "discovery_failed", "write",       "write_rejected",   "submitted",
        "unchanged",     "reuse",          "keep",             "disconnect",
        "disconnected",  "disconnect_timeout", "link_lost",    "fault",
        "link_reset",    "stop",           "stopped",          "stale",
        "unexpected",    "reentry",        "connect_superseded", "link_retry",
    };
    return (unsigned)t < GoveeEvCount ? names[t] : "unknown";
}

const char* govee_engine_fault_name(GoveeEngineFault f) {
    switch(f) {
    case GoveeFaultNone:
        return "none";
    case GoveeFaultCancel:
        return "cancel";
    case GoveeFaultDisconnect:
        return "disconnect";
    case GoveeFaultInternal:
        return "internal";
    }
    return "unknown";
}

bool govee_engine_outcome_is_failure(GoveeOutcome o) {
    switch(o) {
    case GoveeOutcomeConnectFailed:
    case GoveeOutcomeConnectTimeout:
    case GoveeOutcomeCancelFailed:
    case GoveeOutcomeDiscoveryFailed:
    case GoveeOutcomeWriteRejected:
    case GoveeOutcomeLinkLost:
    case GoveeOutcomeAborted:
        return true;
    default:
        return false;
    }
}

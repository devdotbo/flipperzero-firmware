#include "harness.h"
#include "sim.h"
#include "govee_h6006.h"
#include "govee_registry.h"

#include <string.h>

static GoveeEngine eng;
static Sim sim;

static const GoveeEngineLamp* L(int i) {
    return govee_engine_lamp(&eng, (uint8_t)i);
}

static bool ring_has(GoveeEventType t, uint8_t lamp) {
    for(uint32_t i = 0; i < govee_engine_event_count(&eng); i++) {
        const GoveeEngineEvent* ev = govee_engine_event(&eng, i, NULL);
        if(ev->type == t && (lamp == 0xFE || ev->lamp == lamp)) return true;
    }
    return false;
}

static uint32_t set_bri(uint8_t b) {
    GoveeDesired d = eng.desired;
    d.brightness = b;
    return govee_engine_set_desired(&eng, &d);
}

/* An unreachable lamp does not block the others; also the 3-lamp fake-clock pass
 * with bounded total time, started close to the uint32 ms wrap. */
static void test_unreachable_lamp_does_not_block(void) {
    sim_init(&sim, &eng, 3, 0xFFFFF000u);
    sim.lamp[1].reachable = false;
    sim_set_lamps(&sim, 0);
    uint32_t rev = set_bri(80);
    CHECK_EQ(rev, 1);
    uint32_t elapsed = sim_settle(&sim, 60000);

    CHECK_EQ(govee_engine_state(&eng), GoveeEngineIdle);
    CHECK_EQ(L(0)->outcome, GoveeOutcomeSubmitted);
    CHECK_EQ(L(1)->outcome, GoveeOutcomeConnectTimeout);
    CHECK_EQ(L(2)->outcome, GoveeOutcomeSubmitted);
    CHECK_EQ(L(0)->reached_rev, 1);
    CHECK_EQ(L(1)->reached_rev, 0);
    CHECK_EQ(L(1)->attempt_rev, 1);
    CHECK_EQ(L(2)->reached_rev, 1);
    CHECK_EQ(sim.connect_order_n, 3);
    CHECK_EQ(sim.connect_order[0], 0);
    CHECK_EQ(sim.connect_order[1], 1);
    CHECK_EQ(sim.connect_order[2], 2);
    CHECK_EQ(sim.cancels, 1);
    /* phase timings recorded across the wrap */
    CHECK_EQ(L(0)->times.connect_ms, 300);
    CHECK_EQ(L(0)->times.discover_ms, 200);
    CHECK_EQ(L(0)->times.writes, 3);
    CHECK_EQ(L(0)->times.write_ms, 200 + 50); /* gap after power on, then after brightness */
    CHECK_EQ(L(0)->times.disconnect_ms, 50);
    CHECK_EQ(L(0)->times.total_ms, 300 + 200 + 250 + 150 + 50);
    CHECK_EQ(L(1)->times.connect_ms, GOVEE_ENGINE_CONNECT_DEADLINE_MS);
    /* total bounded: 2 good visits (950 ms) + timeout and cancel (3040 ms) */
    CHECK_EQ(elapsed, 950u * 2u + GOVEE_ENGINE_CONNECT_DEADLINE_MS + 40u);
    CHECK(elapsed <= 3u * GOVEE_ENGINE_VISIT_MAX_MS);
    CHECK(eng.pass_no == 1);
    CHECK(ring_has(GoveeEvConnectTimeout, 1));
    CHECK(ring_has(GoveeEvCancelDone, 1));
    CHECK(ring_has(GoveeEvPassEnd, 0xFE));

    /* no tight retry: nothing happens without a change */
    int connects = sim.connects;
    sim_run(&sim, 30000);
    CHECK_EQ(sim.connects, connects);
}

/* Connect timeout leads to cancel and the next lamp still runs. */
static void test_connect_timeout_cancel_next_runs(void) {
    sim_init(&sim, &eng, 2, 1000);
    sim.lamp[0].reachable = false;
    sim_set_lamps(&sim, 0);
    set_bri(70);
    sim_run(&sim, GOVEE_ENGINE_CONNECT_DEADLINE_MS - 1u);
    CHECK_EQ(govee_engine_state(&eng), GoveeEngineConnecting);
    CHECK_EQ(sim.cancels, 0);
    sim_run(&sim, 1);
    CHECK_EQ(govee_engine_state(&eng), GoveeEngineCancelling);
    CHECK_EQ(sim.cancels, 1);
    sim_settle(&sim, 20000);
    CHECK_EQ(L(0)->outcome, GoveeOutcomeConnectTimeout);
    CHECK_EQ(L(1)->outcome, GoveeOutcomeSubmitted);
    CHECK_EQ(sim.connects_per_lamp[1], 1);
}

/* Cancel failure is reported and the pass ends (three variants). */
static void test_cancel_failure_ends_pass(void) {
    for(uint8_t mode = 1; mode <= 3; mode++) {
        sim_init(&sim, &eng, 3, 0);
        sim.lamp[0].reachable = false;
        sim.lamp[0].cancel_mode = mode; /* 1 Error, 2 silent, 3 refused */
        sim_set_lamps(&sim, 0);
        set_bri(60);
        uint32_t elapsed = sim_settle(&sim, 60000);
        CHECK_EQ(govee_engine_state(&eng), GoveeEngineFaulted);
        CHECK_EQ(eng.fault, GoveeFaultCancel);
        CHECK_EQ(L(0)->outcome, GoveeOutcomeCancelFailed);
        CHECK_EQ(L(1)->outcome, GoveeOutcomeNone);
        CHECK_EQ(L(2)->outcome, GoveeOutcomeNone);
        CHECK_EQ(sim.connects, 1);
        CHECK(!eng.in_pass);
        CHECK(ring_has(GoveeEvCancelFailed, 0));
        CHECK(ring_has(GoveeEvFault, 0xFF));
        CHECK(elapsed <= GOVEE_ENGINE_CONNECT_DEADLINE_MS + GOVEE_ENGINE_CANCEL_DEADLINE_MS);
        /* faulted: changes are recorded but nothing is sent */
        set_bri(61);
        sim_run(&sim, 10000);
        CHECK_EQ(sim.connects, 1);
        /* glue reinitialised the HAL: the pass resumes with the other lamps */
        sim.cur = -1;
        sim.lamp[0].cancel_mode = 0;
        govee_engine_link_reset(&eng);
        sim_settle(&sim, 60000);
        CHECK_EQ(L(1)->outcome, GoveeOutcomeSubmitted);
        CHECK_EQ(L(2)->outcome, GoveeOutcomeSubmitted);
        CHECK_EQ(L(1)->reached_rev, 2);
        /* lamp 0 was behind at revision 2 and not attempted yet at 2: it is tried once */
        CHECK_EQ(L(0)->attempt_rev, 2);
    }
}

/* Single-lamp link reuse. */
static void test_single_lamp_link_reuse(void) {
    sim_init(&sim, &eng, 3, 0);
    sim_set_lamps(&sim, 2); /* only lamp 2 (1-based) */
    set_bri(40);
    sim_settle(&sim, 20000);
    CHECK_EQ(govee_engine_state(&eng), GoveeEngineKept);
    CHECK_EQ(sim.connects, 1);
    CHECK_EQ(sim.connects_per_lamp[1], 1);
    CHECK_EQ(sim.disconnects, 0);
    CHECK_EQ(L(0)->outcome, GoveeOutcomeNone);
    CHECK_EQ(L(1)->outcome, GoveeOutcomeSubmitted);

    sim_run(&sim, 1000);
    int writes = sim.writes;
    set_bri(41);
    sim_settle(&sim, 20000);
    CHECK_EQ(sim.connects, 1); /* reused */
    CHECK_EQ(sim.writes, writes + 1);
    CHECK(L(1)->times.reused);
    CHECK_EQ(L(1)->reached_rev, 2);
    CHECK_EQ(govee_engine_state(&eng), GoveeEngineKept);
    CHECK(ring_has(GoveeEvReuse, 1));

    /* keep limit closes the link before the bulb's own 15 s drop */
    sim_run(&sim, GOVEE_ENGINE_KEEP_MAX_MS);
    sim_settle(&sim, 5000);
    CHECK_EQ(govee_engine_state(&eng), GoveeEngineIdle);
    CHECK_EQ(sim.disconnects, 1);

    /* lamp drops a kept link: next change reconnects */
    set_bri(42);
    sim_settle(&sim, 20000);
    CHECK_EQ(sim.connects, 2);
    CHECK_EQ(govee_engine_state(&eng), GoveeEngineKept);
    sim.q[0].used = true; /* inject the lamp-side drop */
    sim.q[0].due = sim.now + 10;
    sim.q[0].type = SimEvDisconnected;
    sim.q[0].gen = eng.link_gen;
    sim_settle(&sim, 1000);
    CHECK_EQ(govee_engine_state(&eng), GoveeEngineIdle);
    CHECK(ring_has(GoveeEvLinkLost, 1));
    set_bri(43);
    sim_settle(&sim, 20000);
    CHECK_EQ(sim.connects, 3);
    CHECK_EQ(L(1)->reached_rev, 4);

    /* release (rescan): the kept link closes, the engine goes idle */
    CHECK_EQ(govee_engine_state(&eng), GoveeEngineKept);
    int disc_before = sim.disconnects;
    CHECK(govee_engine_release_link(&eng));
    sim_settle(&sim, 5000);
    CHECK_EQ(govee_engine_state(&eng), GoveeEngineIdle);
    CHECK_EQ(sim.disconnects, disc_before + 1);
    CHECK(!govee_engine_is_busy(&eng));
    CHECK(!govee_engine_release_link(&eng)); /* ignored outside Kept */
    set_bri(45);
    sim_settle(&sim, 20000);
    CHECK_EQ(sim.connects, 4);
    CHECK_EQ(L(1)->reached_rev, 5);

    /* target back to all: the kept link is closed and every lamp is served */
    sim_set_lamps(&sim, 0);
    sim_settle(&sim, 60000);
    CHECK_EQ(govee_engine_state(&eng), GoveeEngineIdle);
    CHECK_EQ(L(0)->reached_rev, 5);
    CHECK_EQ(L(2)->reached_rev, 5);
}

/* Only differing properties are sent; with power off only power. */
static void test_only_differing_properties(void) {
    sim_init(&sim, &eng, 1, 0);
    sim_set_lamps(&sim, 0);
    set_bri(30);
    sim_settle(&sim, 20000);
    CHECK_EQ(sim.writes, 3);
    sim_run(&sim, 20000); /* let the kept link expire */
    CHECK_EQ(govee_engine_state(&eng), GoveeEngineIdle);

    set_bri(31);
    sim_settle(&sim, 20000);
    CHECK_EQ(sim.writes, 4);
    CHECK_EQ(sim.write_log[3][1], GOVEE_CMD_BRIGHTNESS);
    CHECK_EQ(sim.write_log[3][2], 31);

    GoveeDesired d = eng.desired;
    d.power = false;
    govee_engine_set_desired(&eng, &d);
    sim_settle(&sim, 20000);
    CHECK_EQ(sim.writes, 5);
    CHECK_EQ(sim.write_log[4][1], GOVEE_CMD_POWER);
    CHECK_EQ(sim.write_log[4][2], 0);
    sim_run(&sim, 20000);

    /* brightness while off: nothing differs that would be sent */
    int connects = sim.connects;
    uint32_t rev = set_bri(90);
    sim_settle(&sim, 20000);
    CHECK_EQ(sim.connects, connects);
    CHECK_EQ(L(0)->outcome, GoveeOutcomeUnchanged);
    CHECK_EQ(L(0)->reached_rev, rev);

    /* power on: power then brightness, 200 ms apart; colour already known */
    d = eng.desired;
    d.power = true;
    govee_engine_set_desired(&eng, &d);
    sim_settle(&sim, 20000);
    CHECK_EQ(sim.writes, 7);
    CHECK_EQ(sim.write_log[5][1], GOVEE_CMD_POWER);
    CHECK_EQ(sim.write_log[6][1], GOVEE_CMD_BRIGHTNESS);
    CHECK_EQ(sim.write_log[6][2], 90);
    CHECK_EQ(sim.write_log_t[6] - sim.write_log_t[5], GOVEE_ENGINE_GAP_AFTER_POWER_ON_MS);

    /* colour mode change: one colour write with the RGB payload */
    d = eng.desired;
    d.mode = GoveeModeColour;
    d.rgb = 0x00FF8800u;
    govee_engine_set_desired(&eng, &d);
    sim_settle(&sim, 20000);
    CHECK_EQ(sim.writes, 8);
    CHECK_EQ(sim.write_log[7][1], GOVEE_CMD_COLOR);
    CHECK_EQ(sim.write_log[7][3], 0xFF);
    CHECK_EQ(sim.write_log[7][4], 0x88);
    CHECK(govee_validate_packet(sim.write_log[7]));

    /* same state again: no revision change */
    CHECK_EQ(govee_engine_set_desired(&eng, &d), eng.rev);
}

/* A change during a pass reaches all lamps (fair order into the next pass). */
static void test_change_during_pass_reaches_all(void) {
    sim_init(&sim, &eng, 3, 0);
    sim_set_lamps(&sim, 0);
    set_bri(20);
    /* run into lamp 2's visit: lamp 1 takes 950 ms */
    sim_run(&sim, 1200);
    CHECK_EQ(eng.visit_lamp, 1);
    GoveeDesired d = eng.desired;
    d.kelvin = 6000;
    uint32_t rev = govee_engine_set_desired(&eng, &d);
    CHECK_EQ(rev, 2);
    sim_settle(&sim, 60000);
    CHECK_EQ(govee_engine_state(&eng), GoveeEngineIdle);
    for(int i = 0; i < 3; i++) {
        CHECK_EQ(L(i)->reached_rev, 2);
        const uint8_t* p = sim_last_write(&sim, i, GOVEE_CMD_COLOR);
        CHECK(p != NULL);
        if(p) CHECK_EQ(((unsigned)p[6] << 8) | p[7], 6000);
    }
    /* order: 1,2,3 then the next pass starts after lamp 3: 1,2 */
    CHECK_EQ(sim.connect_order_n, 5);
    CHECK_EQ(sim.connect_order[3], 0);
    CHECK_EQ(sim.connect_order[4], 1);
    CHECK_EQ(sim.connects_per_lamp[2], 1);
    CHECK_EQ(eng.pass_no, 2);
}

/* A stale event from an old generation is ignored. */
static void test_stale_events_ignored(void) {
    sim_init(&sim, &eng, 2, 0);
    sim_set_lamps(&sim, 0);
    set_bri(55);
    sim_run(&sim, 100); /* lamp 1 connecting */
    uint32_t gen1 = eng.link_gen;
    sim_settle(&sim, 1000); /* lamp 1 done, lamp 2 connecting */
    sim_run(&sim, 100);
    CHECK_EQ(govee_engine_state(&eng), GoveeEngineConnecting);
    CHECK(eng.link_gen != gen1);

    uint32_t total = eng.ring_total;
    govee_engine_on_connected(&eng, gen1);
    govee_engine_on_disconnected(&eng, gen1);
    govee_engine_on_link_error(&eng, gen1);
    govee_engine_on_discovery(&eng, gen1, true);
    govee_engine_on_timer(&eng, eng.timer_gen - 1u);
    CHECK_EQ(govee_engine_state(&eng), GoveeEngineConnecting);
    CHECK_EQ(eng.ring_total, total + 5u);
    CHECK(ring_has(GoveeEvStale, 0xFF));

    /* current gen but wrong state: logged as unexpected, ignored */
    govee_engine_on_discovery(&eng, eng.link_gen, true);
    CHECK_EQ(govee_engine_state(&eng), GoveeEngineConnecting);
    CHECK(ring_has(GoveeEvUnexpected, 0xFF));

    sim_settle(&sim, 20000);
    CHECK_EQ(L(0)->outcome, GoveeOutcomeSubmitted);
    CHECK_EQ(L(1)->outcome, GoveeOutcomeSubmitted);
}

/* Lamp removal during a pass takes effect at the next lamp boundary. */
static void test_lamp_removal_during_pass(void) {
    sim_init(&sim, &eng, 3, 0);
    sim_set_lamps(&sim, 0);
    set_bri(25);
    sim_run(&sim, 400); /* lamp 1 discovering */
    CHECK_EQ(eng.visit_lamp, 0);
    GoveeLampId keep[2] = {sim.ids[0], sim.ids[2]}; /* remove lamp 2 */
    govee_engine_set_lamps(&eng, keep, 2, 0);
    CHECK(eng.pending_valid);
    CHECK_EQ(eng.lamp_count, 3);
    CHECK(ring_has(GoveeEvLampsPending, 0xFF));
    sim_settle(&sim, 60000);
    CHECK_EQ(eng.lamp_count, 2);
    CHECK_EQ(sim.connects_per_lamp[1], 0);
    CHECK_EQ(sim.connects_per_lamp[2], 1);
    CHECK_EQ(L(0)->outcome, GoveeOutcomeSubmitted);
    CHECK(govee_lamp_id_equal(&L(1)->id, &sim.ids[2]));
    CHECK_EQ(L(1)->outcome, GoveeOutcomeSubmitted);
    CHECK_EQ(L(0)->times.disconnect_ms, 50);

    /* removing the lamp being visited: its visit completes, then it is gone */
    set_bri(26);
    sim_run(&sim, 400);
    CHECK_EQ(eng.visit_lamp, 0);
    GoveeLampId only[1] = {sim.ids[2]};
    govee_engine_set_lamps(&eng, only, 1, 0);
    sim_settle(&sim, 60000);
    CHECK_EQ(eng.lamp_count, 1);
    CHECK(govee_lamp_id_equal(&L(0)->id, &sim.ids[2]));
    CHECK_EQ(L(0)->reached_rev, 2);
    CHECK_EQ(sim.connects_per_lamp[0], 2);
    CHECK(sim_last_write(&sim, 0, GOVEE_CMD_BRIGHTNESS)[2] == 26);
}

/* Target change during a pass; per-lamp state survives a reorder and a new lamp. */
static void test_target_change_and_remap(void) {
    sim_init(&sim, &eng, 4, 0);
    sim_set_lamps(&sim, 0);
    set_bri(33);
    sim_run(&sim, 400);
    govee_engine_set_lamps(&eng, sim.ids, 4, 3); /* only lamp 3 */
    sim_settle(&sim, 60000);
    CHECK_EQ(sim.connects_per_lamp[1], 0);
    CHECK_EQ(sim.connects_per_lamp[2], 1);
    CHECK_EQ(sim.connects_per_lamp[3], 0);
    CHECK_EQ(govee_engine_state(&eng), GoveeEngineKept);

    /* reversed order plus a new lamp: records follow their identity */
    GoveeLampId rev_ids[5] = {sim.ids[3], sim.ids[2], sim.ids[1], sim.ids[0], sim.ids[0]};
    rev_ids[4].addr[5] = 0x77;
    govee_engine_set_lamps(&eng, rev_ids, 5, 0);
    CHECK_EQ(eng.lamp_count, 5);
    CHECK(govee_lamp_id_equal(&L(1)->id, &sim.ids[2]));
    CHECK_EQ(L(3)->outcome, GoveeOutcomeSubmitted); /* old lamp 1 */
    CHECK_EQ(L(4)->outcome, GoveeOutcomeNone);
    CHECK_EQ(L(4)->reached_rev, 0);

    /* full table remap: 16 old lamps, 16 new ids with one new identity */
    GoveeLampId ids16[GOVEE_MAX_LAMPS];
    for(uint8_t i = 0; i < GOVEE_MAX_LAMPS; i++) {
        ids16[i] = sim.ids[0];
        ids16[i].addr[4] = i;
    }
    sim_init(&sim, &eng, 1, 0);
    govee_engine_stop(&eng); /* Stopped: set_lamps applies at once, nothing is sent */
    govee_engine_set_lamps(&eng, ids16, GOVEE_MAX_LAMPS, 0);
    CHECK_EQ(eng.lamp_count, GOVEE_MAX_LAMPS);
    for(uint8_t i = 0; i < GOVEE_MAX_LAMPS; i++) eng.lamps[i].outcome = (uint8_t)(i % 5u);
    GoveeLampId ids16b[GOVEE_MAX_LAMPS];
    for(uint8_t i = 0; i < GOVEE_MAX_LAMPS; i++) ids16b[i] = ids16[GOVEE_MAX_LAMPS - 1u - i];
    ids16b[0].addr[3] = 0x99; /* new identity; old lamp 15 drops out */
    govee_engine_set_lamps(&eng, ids16b, GOVEE_MAX_LAMPS, 0);
    CHECK_EQ(eng.lamp_count, GOVEE_MAX_LAMPS);
    bool all_ok = true;
    for(uint8_t i = 1; i < GOVEE_MAX_LAMPS; i++) {
        uint8_t old = (uint8_t)(GOVEE_MAX_LAMPS - 1u - i);
        if(!govee_lamp_id_equal(&L(i)->id, &ids16b[i]) || L(i)->outcome != old % 5u) all_ok = false;
    }
    CHECK(all_ok);
    CHECK(govee_lamp_id_equal(&L(0)->id, &ids16b[0]));
    CHECK_EQ(L(0)->outcome, GoveeOutcomeNone);
}

/* apply_again while a target change is pending (it arrived during a connect): the
 * lamps targeted once the change is applied get the full state, not only the
 * properties that differ from what they last received. */
static void test_apply_again_with_pending_target_change(void) {
    sim_init(&sim, &eng, 3, 0);
    sim_set_lamps(&sim, 0);
    set_bri(30);
    sim_settle(&sim, 60000);
    sim_run(&sim, 20000); /* let a kept link expire */
    CHECK_EQ(govee_engine_state(&eng), GoveeEngineIdle);
    for(int i = 0; i < 3; i++) CHECK_EQ(sim.writes_per_lamp[i], 3);

    govee_engine_set_lamps(&eng, sim.ids, 3, 1); /* only lamp 1; applied at once while idle */
    CHECK(!eng.pending_valid);
    set_bri(40);
    sim_run(&sim, 100);
    CHECK_EQ(govee_engine_state(&eng), GoveeEngineConnecting);
    govee_engine_set_lamps(&eng, sim.ids, 3, 0); /* all lamps again, pending until the boundary */
    CHECK(eng.pending_valid);
    uint32_t rev = govee_engine_apply_again(&eng);
    sim_settle(&sim, 60000);
    for(int i = 0; i < 3; i++) {
        CHECK_EQ(L(i)->reached_rev, rev);
        CHECK_EQ(sim.writes_per_lamp[i], 6); /* power, brightness and colour again */
    }
    CHECK(!eng.pending_resend);
}

/* Discovery failure, missing characteristic, write rejection, link loss. */
static void test_visit_failures(void) {
    sim_init(&sim, &eng, 4, 0);
    sim.lamp[0].has_char = false;
    sim.lamp[1].discover_silent = true;
    sim.lamp[2].reject_write_at = 1;
    sim.lamp[3].drop_after_writes = 1;
    sim_set_lamps(&sim, 0);
    set_bri(45);
    sim_settle(&sim, 60000);
    CHECK_EQ(govee_engine_state(&eng), GoveeEngineIdle);
    CHECK_EQ(L(0)->outcome, GoveeOutcomeDiscoveryFailed);
    CHECK_EQ(L(1)->outcome, GoveeOutcomeDiscoveryFailed);
    CHECK_EQ(L(1)->times.discover_ms, GOVEE_ENGINE_DISCOVER_DEADLINE_MS);
    CHECK_EQ(L(2)->outcome, GoveeOutcomeWriteRejected);
    CHECK_EQ(L(3)->outcome, GoveeOutcomeLinkLost);
    for(int i = 0; i < 4; i++) CHECK_EQ(L(i)->reached_rev, 0);
    CHECK_EQ(sim.disconnects, 3); /* lamp 4 lost its link; no disconnect needed */
    CHECK(ring_has(GoveeEvWriteRejected, 2));

    /* apply again: every targeted lamp is retried with a full resend */
    sim.lamp[0].has_char = true;
    sim.lamp[1].discover_silent = false;
    sim.lamp[2].reject_write_at = -1;
    sim.lamp[3].drop_after_writes = -1;
    int w = sim.writes;
    uint32_t rev = govee_engine_apply_again(&eng);
    sim_settle(&sim, 60000);
    for(int i = 0; i < 4; i++) {
        CHECK_EQ(L(i)->outcome, GoveeOutcomeSubmitted);
        CHECK_EQ(L(i)->reached_rev, rev);
    }
    CHECK_EQ(sim.writes, w + 12);

    /* connect refused by the HAL and connect that fails before Connected */
    sim.refuse_connect = true;
    set_bri(46);
    sim_settle(&sim, 60000);
    for(int i = 0; i < 4; i++) CHECK_EQ(L(i)->outcome, GoveeOutcomeConnectFailed);
    sim.refuse_connect = false;
    sim.lamp[0].connect_fails = true;
    set_bri(47);
    sim_settle(&sim, 60000);
    CHECK_EQ(L(0)->outcome, GoveeOutcomeConnectFailed);
    CHECK_EQ(L(1)->outcome, GoveeOutcomeSubmitted);
    /* discovery refused by the HAL */
    sim.lamp[0].connect_fails = false;
    sim.refuse_discover = true;
    set_bri(48);
    sim_settle(&sim, 60000);
    CHECK_EQ(L(0)->outcome, GoveeOutcomeDiscoveryFailed);
}

/* Disconnect timeout faults the engine; stop is bounded in every state. */
static void test_disconnect_timeout_and_stop(void) {
    sim_init(&sim, &eng, 2, 0);
    sim.lamp[0].disconnect_silent = true;
    sim_set_lamps(&sim, 0);
    set_bri(12);
    sim_settle(&sim, 60000);
    CHECK_EQ(govee_engine_state(&eng), GoveeEngineFaulted);
    CHECK_EQ(eng.fault, GoveeFaultDisconnect);
    CHECK_EQ(L(0)->outcome, GoveeOutcomeSubmitted);
    CHECK(ring_has(GoveeEvDisconnectTimeout, 0));
    govee_engine_stop(&eng);
    CHECK(govee_engine_is_stopped(&eng));

    /* stop while connecting: cancel, then Stopped */
    sim_init(&sim, &eng, 2, 0);
    sim_set_lamps(&sim, 0);
    set_bri(13);
    sim_run(&sim, 100);
    govee_engine_stop(&eng);
    CHECK_EQ(sim.cancels, 1);
    uint32_t t = sim_settle(&sim, 10000);
    CHECK(govee_engine_is_stopped(&eng));
    CHECK(t <= GOVEE_ENGINE_STOP_MAX_MS);
    CHECK_EQ(L(0)->outcome, GoveeOutcomeAborted);
    set_bri(14);
    sim_run(&sim, 10000);
    CHECK_EQ(sim.connects, 1);

    /* stop between writes: disconnect, then Stopped */
    sim_init(&sim, &eng, 2, 0);
    sim_set_lamps(&sim, 0);
    set_bri(15);
    sim_run(&sim, 520);
    CHECK_EQ(govee_engine_state(&eng), GoveeEngineWriteGap);
    govee_engine_stop(&eng);
    t = sim_settle(&sim, 10000);
    CHECK(govee_engine_is_stopped(&eng));
    CHECK(t <= GOVEE_ENGINE_STOP_MAX_MS);
    CHECK_EQ(sim.disconnects, 1);

    /* stop while kept, and while idle */
    sim_init(&sim, &eng, 1, 0);
    sim_set_lamps(&sim, 0);
    set_bri(16);
    sim_settle(&sim, 10000);
    CHECK_EQ(govee_engine_state(&eng), GoveeEngineKept);
    govee_engine_stop(&eng);
    sim_settle(&sim, 10000);
    CHECK(govee_engine_is_stopped(&eng));
    CHECK(!govee_engine_is_busy(&eng));
    sim_init(&sim, &eng, 1, 0);
    govee_engine_stop(&eng);
    CHECK(govee_engine_is_stopped(&eng));

    /* stop with the cancel failing still ends in Stopped */
    sim_init(&sim, &eng, 1, 0);
    sim.lamp[0].reachable = false;
    sim.lamp[0].cancel_mode = 2;
    sim_set_lamps(&sim, 0);
    set_bri(17);
    sim_run(&sim, 100);
    govee_engine_stop(&eng);
    t = sim_settle(&sim, 10000);
    CHECK(govee_engine_is_stopped(&eng));
    CHECK_EQ(eng.fault, GoveeFaultCancel);
    CHECK(t <= GOVEE_ENGINE_STOP_MAX_MS);
}

/* The engine refuses calls made from inside an op. */
static void test_reentry_refused(void) {
    sim_init(&sim, &eng, 1, 0);
    sim.reenter_from_changed = true;
    sim_set_lamps(&sim, 0);
    uint32_t rev = set_bri(70);
    sim_settle(&sim, 10000);
    CHECK(sim.changed_calls > 0);
    CHECK_EQ(eng.rev, rev);
    CHECK(ring_has(GoveeEvReentry, 0xFF));
}

/* Event ring keeps the last 64 with continuous sequence numbers; clear resets. */
static void test_event_ring(void) {
    sim_init(&sim, &eng, 1, 0);
    for(uint32_t i = 0; i < 100; i++) govee_engine_on_timer(&eng, 12345u + i);
    CHECK_EQ(govee_engine_event_count(&eng), GOVEE_ENGINE_EVENT_RING);
    uint32_t seq = 0;
    const GoveeEngineEvent* ev = govee_engine_event(&eng, 0, &seq);
    CHECK(ev != NULL);
    CHECK_EQ(seq, 37);
    CHECK_EQ(ev->arg, 12345u + 36u);
    govee_engine_event(&eng, GOVEE_ENGINE_EVENT_RING - 1u, &seq);
    CHECK_EQ(seq, 100);
    CHECK(govee_engine_event(&eng, GOVEE_ENGINE_EVENT_RING, &seq) == NULL);
    govee_engine_clear_events(&eng);
    CHECK_EQ(govee_engine_event_count(&eng), 0);

    /* every name table entry exists */
    for(int t = 0; t < GoveeEvCount; t++) CHECK(strcmp(govee_engine_event_name((GoveeEventType)t), "unknown") != 0);
    for(int o = 0; o < GoveeOutcomeCount; o++) CHECK(strcmp(govee_engine_outcome_name((GoveeOutcome)o), "unknown") != 0);
    for(int s = 0; s < GoveeEngineStateCount; s++) CHECK(strcmp(govee_engine_state_name((GoveeEngineState)s), "unknown") != 0);
}

/* With one failed lamp among three, the lamps that answered go first (fair
 * order), the failed lamp last. Its total includes the cancel. */
static void test_failed_lamp_visited_last(void) {
    sim_init(&sim, &eng, 3, 0);
    sim.lamp[0].reachable = false;
    sim_set_lamps(&sim, 0);
    set_bri(30);
    sim_settle(&sim, 60000);
    CHECK_EQ(L(0)->outcome, GoveeOutcomeConnectTimeout);
    CHECK_EQ(sim.connect_order_n, 3);
    CHECK_EQ(sim.connect_order[0], 0); /* never tried: plain fair order */
    CHECK_EQ(L(0)->times.total_ms, GOVEE_ENGINE_CONNECT_DEADLINE_MS + 40u); /* cancel included */

    uint32_t rev = set_bri(31);
    sim_settle(&sim, 60000);
    CHECK_EQ(sim.connect_order_n, 6);
    CHECK_EQ(sim.connect_order[3], 1);
    CHECK_EQ(sim.connect_order[4], 2);
    CHECK_EQ(sim.connect_order[5], 0);
    CHECK_EQ(L(1)->reached_rev, rev);
    CHECK_EQ(L(2)->reached_rev, rev);
    CHECK_EQ(L(0)->attempt_rev, rev);
    CHECK_EQ(L(0)->outcome, GoveeOutcomeConnectTimeout);

    /* fair within the group that answered: the next pass starts after lamp 3 */
    sim.lamp[0].reachable = true;
    set_bri(32);
    sim_settle(&sim, 60000);
    CHECK_EQ(sim.connect_order_n, 9);
    CHECK_EQ(sim.connect_order[6], 1);
    CHECK_EQ(sim.connect_order[7], 2);
    CHECK_EQ(sim.connect_order[8], 0);
    CHECK_EQ(L(0)->outcome, GoveeOutcomeSubmitted);
}

/* A lamp whose last attempt failed gets the shorter connect deadline; after a
 * success it gets the full deadline again. */
static void test_failed_lamp_retry_deadline(void) {
    sim_init(&sim, &eng, 2, 0);
    sim.lamp[1].reachable = false;
    sim_set_lamps(&sim, 0);
    set_bri(30);
    sim_settle(&sim, 60000);
    CHECK_EQ(L(1)->times.connect_ms, GOVEE_ENGINE_CONNECT_DEADLINE_MS);
    set_bri(31);
    sim_settle(&sim, 60000);
    CHECK_EQ(L(1)->outcome, GoveeOutcomeConnectTimeout);
    CHECK_EQ(L(1)->times.connect_ms, GOVEE_ENGINE_CONNECT_RETRY_DEADLINE_MS);
    CHECK_EQ(L(1)->times.total_ms, GOVEE_ENGINE_CONNECT_RETRY_DEADLINE_MS + 40u);
    CHECK_EQ(sim.cancels, 2);
    /* the lamp answers within the shorter deadline */
    sim.lamp[1].reachable = true;
    uint32_t rev = set_bri(32);
    sim_settle(&sim, 60000);
    CHECK_EQ(L(1)->outcome, GoveeOutcomeSubmitted);
    CHECK_EQ(L(1)->reached_rev, rev);
    /* last attempt succeeded: the full deadline again */
    sim.lamp[1].reachable = false;
    set_bri(33);
    sim_settle(&sim, 60000);
    CHECK_EQ(L(1)->times.connect_ms, GOVEE_ENGINE_CONNECT_DEADLINE_MS);
}

/* The first attempt on a lamp gets the full 3 s, and a change during it does
 * not cancel it (only a retry is cancelled early). */
static void test_first_attempt_full_deadline(void) {
    sim_init(&sim, &eng, 2, 0);
    sim.lamp[0].reachable = false;
    sim_set_lamps(&sim, 0);
    set_bri(30);
    sim_run(&sim, GOVEE_ENGINE_CONNECT_RETRY_DEADLINE_MS + 100u);
    CHECK_EQ(govee_engine_state(&eng), GoveeEngineConnecting);
    CHECK_EQ(eng.visit_lamp, 0);
    CHECK(!eng.visit_retry);
    set_bri(31);
    CHECK_EQ(sim.cancels, 0);
    CHECK_EQ(govee_engine_state(&eng), GoveeEngineConnecting);
    sim_run(&sim, GOVEE_ENGINE_CONNECT_DEADLINE_MS - (GOVEE_ENGINE_CONNECT_RETRY_DEADLINE_MS + 100u));
    CHECK_EQ(sim.cancels, 1);
    CHECK_EQ(L(0)->times.connect_ms, GOVEE_ENGINE_CONNECT_DEADLINE_MS);
    CHECK(!ring_has(GoveeEvConnectSuperseded, 0xFE));
    sim_settle(&sim, 60000);
    CHECK_EQ(L(1)->reached_rev, 2);
}

/* A change (set_desired or apply_again) while connecting to a lamp whose last
 * attempt failed cancels that attempt at once; after the cancel resolved a new
 * pass reaches the lamps that answer first. */
static void test_change_cancels_retry(void) {
    for(int variant = 0; variant < 2; variant++) {
        sim_init(&sim, &eng, 3, 0);
        sim.lamp[1].reachable = false;
        sim_set_lamps(&sim, 0);
        set_bri(30);
        sim_settle(&sim, 60000);
        set_bri(31); /* pass 2: lamps 1 and 3, then the retry on lamp 2 */
        for(int k = 0; k < 1000; k++) {
            if(govee_engine_state(&eng) == GoveeEngineConnecting && eng.visit_lamp == 1) break;
            sim_run(&sim, 10);
        }
        CHECK_EQ(govee_engine_state(&eng), GoveeEngineConnecting);
        CHECK_EQ(eng.visit_lamp, 1);
        CHECK(eng.visit_retry);
        sim_run(&sim, 400);
        uint32_t pass = eng.pass_no;
        uint32_t connecting_ms = sim.now - eng.phase_start_ms;
        int cancels = sim.cancels;
        int n0 = sim.connect_order_n;
        uint32_t rev = (variant == 0) ? set_bri(32) : govee_engine_apply_again(&eng);
        CHECK_EQ(sim.cancels, cancels + 1); /* at once */
        CHECK_EQ(govee_engine_state(&eng), GoveeEngineCancelling);
        CHECK_EQ(L(1)->times.connect_ms, connecting_ms);
        CHECK(connecting_ms < GOVEE_ENGINE_CONNECT_RETRY_DEADLINE_MS);
        CHECK_EQ(L(1)->outcome, GoveeOutcomeConnectTimeout);
        CHECK(ring_has(GoveeEvConnectSuperseded, 1));
        /* nothing new starts before the cancel resolved */
        sim_run(&sim, 39);
        CHECK_EQ(sim.connect_order_n, n0);
        CHECK_EQ(eng.pass_no, pass);
        sim_settle(&sim, 60000);
        CHECK_EQ(eng.pass_no, pass + 1u);
        CHECK_EQ(sim.connect_order_n, n0 + 3);
        CHECK(sim.connect_order[n0] != 1);
        CHECK(sim.connect_order[n0 + 1] != 1);
        CHECK(sim.connect_order[n0] != sim.connect_order[n0 + 1]);
        CHECK_EQ(sim.connect_order[n0 + 2], 1);
        CHECK_EQ(L(0)->reached_rev, rev);
        CHECK_EQ(L(2)->reached_rev, rev);
        CHECK_EQ(L(1)->attempt_rev, rev);
        CHECK_EQ(L(1)->times.connect_ms, GOVEE_ENGINE_CONNECT_RETRY_DEADLINE_MS);
        CHECK_EQ(govee_engine_state(&eng), GoveeEngineIdle);
    }
}

/* A kept link's visit total includes the tail before the link is kept. */
static void test_visit_total_kept_tail(void) {
    sim_init(&sim, &eng, 1, 0);
    sim_set_lamps(&sim, 0);
    set_bri(40);
    sim_settle(&sim, 20000);
    CHECK_EQ(govee_engine_state(&eng), GoveeEngineKept);
    CHECK_EQ(L(0)->times.total_ms, 300 + 200 + 250 + GOVEE_ENGINE_TAIL_MS);
}

/* The link ended during the tail and the HAL refuses the disconnect; the
 * visit total still includes the tail. */
static void test_visit_total_tail_link_already_closed(void) {
    sim_init(&sim, &eng, 2, 0);
    sim_set_lamps(&sim, 0);
    set_bri(40);
    for(int k = 0; k < 1000 && govee_engine_state(&eng) != GoveeEngineTail; k++) sim_run(&sim, 1);
    CHECK_EQ(govee_engine_state(&eng), GoveeEngineTail);
    CHECK_EQ(eng.visit_lamp, 0);
    uint32_t written_total = L(0)->times.total_ms;
    sim.cur = -1; /* link ended; its terminal event is not delivered yet */
    sim_settle(&sim, 20000);
    CHECK_EQ(L(0)->outcome, GoveeOutcomeSubmitted);
    CHECK_EQ(L(0)->times.total_ms, written_total + GOVEE_ENGINE_TAIL_MS);
    CHECK_EQ(L(1)->reached_rev, 1);
}

/* A change supersedes the retry on a lamp that failed before; late events of
 * that old attempt (connected, link error, disconnected, timer) then arrive at
 * different points of the following pass. They are stale and change nothing:
 * every variant ends exactly like the run without late events. */
static void test_late_events_after_supersede(void) {
    static const int32_t at_ms[] = {-1, 0, 20, 150, 400, 560, 900, 1300, 1700, 2600};
    GoveeEngineLamp ref[3];
    int ref_order[SIM_LOG];
    int ref_order_n = 0, ref_writes = 0, ref_cancels = 0;
    uint32_t ref_pass = 0;
    for(size_t v = 0; v < sizeof(at_ms) / sizeof(at_ms[0]); v++) {
        sim_init(&sim, &eng, 3, 0);
        sim.lamp[1].reachable = false;
        sim_set_lamps(&sim, 0);
        set_bri(30);
        sim_settle(&sim, 60000);
        set_bri(31); /* pass 2: lamps 1 and 3, then the retry on lamp 2 */
        for(int k = 0; k < 1000; k++) {
            if(govee_engine_state(&eng) == GoveeEngineConnecting && eng.visit_lamp == 1) break;
            sim_run(&sim, 10);
        }
        CHECK(eng.visit_retry);
        sim_run(&sim, 400);
        uint32_t old_link = eng.link_gen;
        uint32_t old_timer = eng.timer_gen;
        uint32_t rev = set_bri(32); /* supersedes the retry */
        CHECK_EQ(govee_engine_state(&eng), GoveeEngineCancelling);
        sim_run(&sim, 40); /* cancel resolved: the following pass starts */
        CHECK(govee_engine_state(&eng) != GoveeEngineCancelling);
        if(at_ms[v] >= 0) {
            uint32_t t = (uint32_t)at_ms[v];
            sim_inject(&sim, t, SimEvConnected, old_link);
            sim_inject(&sim, t + 1u, SimEvError, old_link);
            sim_inject(&sim, t + 2u, SimEvDisconnected, old_link);
            sim_inject(&sim, t + 3u, SimEvTimer, old_timer);
        }
        sim_settle(&sim, 60000);
        CHECK_EQ(govee_engine_state(&eng), GoveeEngineIdle);
        CHECK_EQ(eng.fault, GoveeFaultNone);
        CHECK_EQ(L(0)->outcome, GoveeOutcomeSubmitted);
        CHECK_EQ(L(2)->outcome, GoveeOutcomeSubmitted);
        CHECK_EQ(L(0)->reached_rev, rev);
        CHECK_EQ(L(2)->reached_rev, rev);
        CHECK_EQ(L(1)->outcome, GoveeOutcomeConnectTimeout);
        CHECK_EQ(L(1)->attempt_rev, rev);
        CHECK_EQ(L(1)->times.connect_ms, GOVEE_ENGINE_CONNECT_RETRY_DEADLINE_MS);
        if(at_ms[v] < 0) {
            for(int i = 0; i < 3; i++) ref[i] = *L(i);
            memcpy(ref_order, sim.connect_order, sizeof(ref_order));
            ref_order_n = sim.connect_order_n;
            ref_writes = sim.writes;
            ref_cancels = sim.cancels;
            ref_pass = eng.pass_no;
            continue;
        }
        CHECK(ring_has(GoveeEvStale, 0xFE));
        CHECK(!ring_has(GoveeEvUnexpected, 0xFE));
        CHECK_EQ(eng.pass_no, ref_pass);
        CHECK_EQ(sim.connect_order_n, ref_order_n);
        CHECK(memcmp(sim.connect_order, ref_order, sizeof(int) * (size_t)ref_order_n) == 0);
        CHECK_EQ(sim.writes, ref_writes);
        CHECK_EQ(sim.cancels, ref_cancels);
        for(int i = 0; i < 3; i++) {
            CHECK_EQ(L(i)->outcome, ref[i].outcome);
            CHECK_EQ(L(i)->reached_rev, ref[i].reached_rev);
            CHECK_EQ(L(i)->attempt_rev, ref[i].attempt_rev);
            CHECK(memcmp(&L(i)->times, &ref[i].times, sizeof(ref[i].times)) == 0);
        }
    }
}

static int ring_count(GoveeEventType t) {
    int n = 0;
    for(uint32_t i = 0; i < govee_engine_event_count(&eng); i++) {
        if(govee_engine_event(&eng, i, NULL)->type == t) n++;
    }
    return n;
}

static void run_until_state(GoveeEngineState st) {
    for(int k = 0; k < 20000 && govee_engine_state(&eng) != st; k++) sim_run(&sim, 1);
}

/* A link lost after Connected and before the first write (as Disconnected or
 * as a terminal link error) is retried once at once; the retry succeeds and the
 * visit total covers both attempts. */
static void test_early_loss_retried_once(void) {
    for(int variant = 0; variant < 2; variant++) {
        sim_init(&sim, &eng, 2, 0);
        sim.lamp[0].drop_in_discovery = 1;
        sim.lamp[0].drop_as_error = (variant == 1);
        sim_set_lamps(&sim, 0);
        uint32_t rev = set_bri(40);
        sim_settle(&sim, 60000);
        CHECK_EQ(govee_engine_state(&eng), GoveeEngineIdle);
        CHECK_EQ(eng.fault, GoveeFaultNone);
        CHECK_EQ(eng.pass_no, 1);
        CHECK_EQ(L(0)->outcome, GoveeOutcomeSubmitted);
        CHECK_EQ(L(0)->reached_rev, rev);
        CHECK_EQ(L(0)->last, GoveeLampLastOk);
        CHECK_EQ(L(1)->outcome, GoveeOutcomeSubmitted);
        CHECK_EQ(L(1)->reached_rev, rev);
        CHECK_EQ(sim.connect_order_n, 3);
        CHECK_EQ(sim.connect_order[0], 0);
        CHECK_EQ(sim.connect_order[1], 0); /* immediately, before the next lamp */
        CHECK_EQ(sim.connect_order[2], 1);
        CHECK_EQ(ring_count(GoveeEvLinkRetry), 1);
        CHECK(ring_has(GoveeEvLinkRetry, 0));
        CHECK(ring_has(variant == 0 ? GoveeEvLinkLost : GoveeEvDiscoveryFailed, 0));
        CHECK_EQ(L(0)->times.connect_ms, 300);
        CHECK_EQ(L(0)->times.discover_ms, 200);
        CHECK_EQ(L(0)->times.writes, 3);
        /* 300 connect + 115 until the loss, then the full visit of 950 ms */
        CHECK_EQ(L(0)->times.total_ms, 300 + 115 + 300 + 200 + 250 + 150 + 50);
    }
}

/* A second early loss in the same visit gives the failure outcome; no third
 * connect, and the next lamp still runs. */
static void test_early_loss_twice_fails(void) {
    for(int variant = 0; variant < 2; variant++) {
        sim_init(&sim, &eng, 2, 0);
        sim.lamp[0].drop_in_discovery = 2;
        sim.lamp[0].drop_as_error = (variant == 1);
        sim_set_lamps(&sim, 0);
        uint32_t rev = set_bri(40);
        sim_settle(&sim, 60000);
        CHECK_EQ(govee_engine_state(&eng), GoveeEngineIdle);
        CHECK_EQ(eng.fault, GoveeFaultNone);
        CHECK_EQ(L(0)->outcome, variant == 0 ? GoveeOutcomeLinkLost : GoveeOutcomeDiscoveryFailed);
        CHECK_EQ(L(0)->reached_rev, 0);
        CHECK_EQ(L(0)->attempt_rev, rev);
        CHECK_EQ(L(0)->last, GoveeLampLastFailed);
        CHECK_EQ(L(0)->times.writes, 0);
        CHECK_EQ(L(0)->times.total_ms, 300 + 115 + 300 + 115);
        CHECK_EQ(sim.connects_per_lamp[0], 2);
        CHECK_EQ(ring_count(GoveeEvLinkRetry), 1);
        CHECK_EQ(L(1)->outcome, GoveeOutcomeSubmitted);
        CHECK_EQ(L(1)->reached_rev, rev);
        CHECK_EQ(sim.connects_per_lamp[1], 1);
        CHECK_EQ(eng.pass_no, 1);
    }
}

/* A link lost after an accepted write is not retried (the command may have
 * arrived), and a refused write on a live link is no link loss. */
static void test_loss_after_write_not_retried(void) {
    sim_init(&sim, &eng, 2, 0);
    sim.lamp[0].drop_after_writes = 1;
    sim_set_lamps(&sim, 0);
    set_bri(40);
    sim_settle(&sim, 60000);
    CHECK_EQ(L(0)->outcome, GoveeOutcomeLinkLost);
    CHECK_EQ(L(0)->times.writes, 1);
    CHECK_EQ(sim.connects_per_lamp[0], 1);
    CHECK_EQ(ring_count(GoveeEvLinkRetry), 0);
    CHECK_EQ(L(1)->outcome, GoveeOutcomeSubmitted);

    for(int at = 0; at < 2; at++) {
        sim_init(&sim, &eng, 1, 0);
        sim.lamp[0].reject_write_at = at;
        sim_set_lamps(&sim, 0);
        set_bri(41);
        sim_settle(&sim, 60000);
        CHECK_EQ(L(0)->outcome, GoveeOutcomeWriteRejected);
        CHECK_EQ(sim.connects, 1);
        CHECK_EQ(ring_count(GoveeEvLinkRetry), 0);
        CHECK_EQ(govee_engine_state(&eng), GoveeEngineIdle);
    }
}

/* Single lamp with a kept link. The first connect of the visit is lost early:
 * retried, then kept. Later the kept link ends unseen (the first write of a
 * reuse visit is refused and the disconnect finds no link): one fresh connect. */
static void test_kept_link_early_loss(void) {
    sim_init(&sim, &eng, 1, 0);
    sim.lamp[0].drop_in_discovery = 1;
    sim_set_lamps(&sim, 0);
    uint32_t rev = set_bri(40);
    sim_settle(&sim, 20000);
    CHECK_EQ(govee_engine_state(&eng), GoveeEngineKept);
    CHECK_EQ(sim.connects, 2);
    CHECK_EQ(L(0)->outcome, GoveeOutcomeSubmitted);
    CHECK_EQ(L(0)->reached_rev, rev);
    CHECK_EQ(L(0)->times.total_ms, 300 + 115 + 300 + 200 + 250 + GOVEE_ENGINE_TAIL_MS);
    CHECK_EQ(ring_count(GoveeEvLinkRetry), 1);

    sim_run(&sim, 500);
    rev = set_bri(41);
    sim_settle(&sim, 20000);
    CHECK_EQ(sim.connects, 2); /* reused */
    CHECK(L(0)->times.reused);
    CHECK_EQ(L(0)->reached_rev, rev);

    sim_run(&sim, 500);
    sim.cur = -1; /* the link ended; its terminal event is not delivered yet */
    sim.writes_on_link = 0; /* the HAL refuses the first write */
    rev = set_bri(42);
    sim_settle(&sim, 20000);
    CHECK_EQ(sim.connects, 3);
    CHECK_EQ(ring_count(GoveeEvLinkRetry), 2);
    CHECK_EQ(L(0)->outcome, GoveeOutcomeSubmitted);
    CHECK_EQ(L(0)->reached_rev, rev);
    CHECK(!L(0)->times.reused);
    CHECK_EQ(govee_engine_state(&eng), GoveeEngineKept);
}

/* Knowledge saved by the previous run. In the very first pass a lamp saved as
 * failed goes last with the 1.5 s deadline; unknown and saved-ok lamps get 3 s. */
static void test_saved_failed_lamp_first_pass(void) {
    sim_init(&sim, &eng, 4, 0);
    sim.lamp[0].reachable = false;
    sim.lamp[2].reachable = false;
    sim.lamp[3].reachable = false;
    const uint8_t last[4] = {
        GoveeLampLastFailed, GoveeLampLastUnknown, GoveeLampLastOk, GoveeLampLastUnknown};
    govee_engine_set_lamps_last(&eng, sim.ids, last, 4, 0);
    CHECK_EQ(L(0)->last, GoveeLampLastFailed);
    CHECK_EQ(L(2)->last, GoveeLampLastOk);
    uint32_t rev = set_bri(30);
    sim_settle(&sim, 60000);
    CHECK_EQ(eng.pass_no, 1);
    CHECK_EQ(sim.connect_order_n, 4);
    CHECK_EQ(sim.connect_order[0], 1);
    CHECK_EQ(sim.connect_order[1], 2);
    CHECK_EQ(sim.connect_order[2], 3);
    CHECK_EQ(sim.connect_order[3], 0);
    CHECK_EQ(L(1)->reached_rev, rev);
    CHECK_EQ(L(0)->times.connect_ms, GOVEE_ENGINE_CONNECT_RETRY_DEADLINE_MS);
    CHECK_EQ(L(2)->times.connect_ms, GOVEE_ENGINE_CONNECT_DEADLINE_MS);
    CHECK_EQ(L(3)->times.connect_ms, GOVEE_ENGINE_CONNECT_DEADLINE_MS);
    CHECK_EQ(L(0)->last, GoveeLampLastFailed);
    CHECK_EQ(L(1)->last, GoveeLampLastOk);
    CHECK_EQ(L(2)->last, GoveeLampLastFailed);
    CHECK_EQ(L(3)->last, GoveeLampLastFailed);

    /* a list with stale knowledge does not override what this run learned */
    const uint8_t stale[4] = {0, 0, 0, 0};
    govee_engine_set_lamps_last(&eng, sim.ids, stale, 4, 0);
    CHECK_EQ(L(1)->last, GoveeLampLastOk);
    CHECK_EQ(L(2)->last, GoveeLampLastFailed);
    /* without knowledge (old call) every lamp is unknown: plain fair order, 3 s */
    sim_init(&sim, &eng, 2, 0);
    sim.lamp[0].reachable = false;
    sim_set_lamps(&sim, 0);
    CHECK_EQ(L(0)->last, GoveeLampLastUnknown);
    set_bri(30);
    sim_settle(&sim, 60000);
    CHECK_EQ(sim.connect_order[0], 0);
    CHECK_EQ(L(0)->times.connect_ms, GOVEE_ENGINE_CONNECT_DEADLINE_MS);
}

/* A change while connecting to a lamp saved as failed cancels the attempt at
 * once; neither that superseded attempt nor one aborted by stop is a completed
 * attempt, so the saved knowledge stays. */
static void test_saved_failed_superseded_and_aborted(void) {
    sim_init(&sim, &eng, 2, 0);
    sim.lamp[0].reachable = false;
    const uint8_t last[2] = {GoveeLampLastFailed, GoveeLampLastOk};
    govee_engine_set_lamps_last(&eng, sim.ids, last, 2, 0);
    set_bri(30);
    for(int k = 0; k < 1000; k++) {
        if(govee_engine_state(&eng) == GoveeEngineConnecting && eng.visit_lamp == 0) break;
        sim_run(&sim, 10);
    }
    CHECK_EQ(eng.visit_lamp, 0);
    CHECK(eng.visit_retry);
    sim_run(&sim, 200);
    int cancels = sim.cancels;
    uint32_t rev = set_bri(31);
    CHECK_EQ(sim.cancels, cancels + 1);
    CHECK(ring_has(GoveeEvConnectSuperseded, 0));
    CHECK_EQ(L(0)->last, GoveeLampLastFailed);
    sim_settle(&sim, 60000);
    CHECK_EQ(L(1)->reached_rev, rev);
    CHECK_EQ(L(1)->last, GoveeLampLastOk);

    sim_init(&sim, &eng, 1, 0);
    const uint8_t ok[1] = {GoveeLampLastOk};
    govee_engine_set_lamps_last(&eng, sim.ids, ok, 1, 0);
    set_bri(30);
    sim_run(&sim, 100);
    govee_engine_stop(&eng);
    sim_settle(&sim, 10000);
    CHECK(govee_engine_is_stopped(&eng));
    CHECK_EQ(L(0)->outcome, GoveeOutcomeAborted);
    CHECK_EQ(L(0)->last, GoveeLampLastOk);
}

/* The disconnect deadline is 5 s (above the 2 s link supervision timeout);
 * the stop path keeps its short bound, also for a disconnect already running. */
static void test_disconnect_deadline(void) {
    /* completes after 3 s: no fault */
    sim_init(&sim, &eng, 2, 0);
    sim.lamp[0].disconnect_ms = 3000;
    sim_set_lamps(&sim, 0);
    uint32_t rev = set_bri(12);
    sim_settle(&sim, 60000);
    CHECK_EQ(govee_engine_state(&eng), GoveeEngineIdle);
    CHECK_EQ(eng.fault, GoveeFaultNone);
    CHECK_EQ(L(0)->times.disconnect_ms, 3000);
    CHECK_EQ(L(1)->reached_rev, rev);
    CHECK(!ring_has(GoveeEvDisconnectTimeout, 0xFE));

    /* no completion: fault exactly at 5 s, not before */
    sim_init(&sim, &eng, 2, 0);
    sim.lamp[0].disconnect_silent = true;
    sim_set_lamps(&sim, 0);
    set_bri(13);
    run_until_state(GoveeEngineDisconnecting);
    CHECK_EQ(govee_engine_state(&eng), GoveeEngineDisconnecting);
    sim_run(&sim, GOVEE_ENGINE_DISCONNECT_DEADLINE_MS - 1u);
    CHECK_EQ(govee_engine_state(&eng), GoveeEngineDisconnecting);
    sim_run(&sim, 1);
    CHECK_EQ(govee_engine_state(&eng), GoveeEngineFaulted);
    CHECK_EQ(eng.fault, GoveeFaultDisconnect);

    /* stop during a slow disconnect (4.5 s): Stopped within the stop bound */
    sim_init(&sim, &eng, 2, 0);
    sim.lamp[0].disconnect_ms = 4500;
    sim_set_lamps(&sim, 0);
    set_bri(14);
    run_until_state(GoveeEngineDisconnecting);
    sim_run(&sim, 200);
    govee_engine_stop(&eng);
    uint32_t t0 = sim.now;
    sim_run(&sim, 400); /* time passes before the second stop */
    CHECK_EQ(govee_engine_state(&eng), GoveeEngineDisconnecting);
    govee_engine_stop(&eng); /* a second stop does not extend the deadline */
    run_until_state(GoveeEngineStopped); /* the lamp's late Disconnected is still queued */
    uint32_t t = sim.now - t0;
    CHECK(govee_engine_is_stopped(&eng));
    CHECK_EQ(t, GOVEE_ENGINE_STOP_DISCONNECT_DEADLINE_MS);
    CHECK(t <= GOVEE_ENGINE_STOP_MAX_MS);
    /* The queued late Disconnected (4.5 s after the disconnect) is delivered and
     * only logged: still Stopped, same fault (the stop deadline's), same pass */
    uint32_t pass = eng.pass_no;
    uint8_t fault = eng.fault;
    CHECK(!ring_has(GoveeEvUnexpected, 0xFE) && !ring_has(GoveeEvStale, 0xFE));
    sim_settle(&sim, 10000);
    CHECK(govee_engine_is_stopped(&eng));
    CHECK_EQ(eng.fault, fault);
    CHECK_EQ(eng.pass_no, pass);
    CHECK(ring_has(GoveeEvUnexpected, 0xFE) || ring_has(GoveeEvStale, 0xFE));

    /* stop shortly before the 5 s deadline: the earlier deadline stays */
    sim_init(&sim, &eng, 2, 0);
    sim.lamp[0].disconnect_silent = true;
    sim_set_lamps(&sim, 0);
    set_bri(15);
    run_until_state(GoveeEngineDisconnecting);
    sim_run(&sim, GOVEE_ENGINE_DISCONNECT_DEADLINE_MS - 300u);
    govee_engine_stop(&eng);
    t = sim_settle(&sim, 10000);
    CHECK(govee_engine_is_stopped(&eng));
    CHECK_EQ(t, 300);

    /* stop in the tail: the stop path's own disconnect gets the short deadline */
    sim_init(&sim, &eng, 2, 0);
    sim.lamp[0].disconnect_silent = true;
    sim_set_lamps(&sim, 0);
    set_bri(16);
    run_until_state(GoveeEngineTail);
    govee_engine_stop(&eng);
    t = sim_settle(&sim, 10000);
    CHECK(govee_engine_is_stopped(&eng));
    CHECK_EQ(t, GOVEE_ENGINE_STOP_DISCONNECT_DEADLINE_MS);
    CHECK(GOVEE_ENGINE_STOP_MAX_MS == 2500u); /* the app's teardown bound is unchanged */
}

/* An early-loss retry aborted by stop keeps the lamp's previous completed result, for
 * every early-loss path: the loss reported directly (Disconnected in discovery),
 * the loss as a link error (the visit fails first, then the disconnect finds no
 * link), and a kept link that ended unseen (first write refused, no link). */
static void test_early_loss_retry_aborted_keeps_last(void) {
    for(int variant = 0; variant < 3; variant++) {
        if(variant < 2) {
            sim_init(&sim, &eng, 2, 0);
            sim_set_lamps(&sim, 0);
            set_bri(40);
            sim_settle(&sim, 60000);
            CHECK_EQ(L(0)->last, GoveeLampLastOk);
            sim.lamp[0].drop_in_discovery = 1;
            sim.lamp[0].drop_as_error = (variant == 1);
            set_bri(41);
            for(int k = 0; k < 20000 && ring_count(GoveeEvLinkRetry) == 0; k++) sim_run(&sim, 1);
        } else {
            sim_init(&sim, &eng, 1, 0);
            sim_set_lamps(&sim, 0);
            set_bri(40);
            sim_settle(&sim, 20000);
            CHECK_EQ(govee_engine_state(&eng), GoveeEngineKept);
            CHECK_EQ(L(0)->last, GoveeLampLastOk);
            sim_run(&sim, 500);
            sim.cur = -1; /* the kept link ended unseen */
            sim.writes_on_link = 0; /* the HAL refuses the first write */
            set_bri(41);
        }
        CHECK_EQ(ring_count(GoveeEvLinkRetry), 1);
        CHECK_EQ(govee_engine_state(&eng), GoveeEngineConnecting);
        CHECK_EQ(eng.visit_lamp, 0);
        CHECK_EQ(L(0)->last, GoveeLampLastOk); /* no provisional failure while the retry runs */
        govee_engine_stop(&eng);
        sim_settle(&sim, 10000);
        CHECK(govee_engine_is_stopped(&eng));
        CHECK_EQ(L(0)->outcome, GoveeOutcomeAborted);
        CHECK_EQ(L(0)->last, GoveeLampLastOk);
    }
}

/* After an early-loss reconnect the lost link's late events (Disconnected, link error,
 * discovery result, and the old timer) arrive at several points of the retry.
 * Mode 0: the retry completes; mode 1: stop during the retry (events during and
 * after the stop's cancel); mode 2: the retry connect times out (events during the
 * cancel). The retry and the rest of the pass match a run without those events. */
static void test_late_events_of_lost_link(void) {
    static const int32_t at_ms[] = {-1, 0, 1, 20, 150, 299, 300, 450, 520, 900, 1400};
    for(int mode = 0; mode < 3; mode++) {
        GoveeEngineLamp ref[2];
        int ref_order[SIM_LOG];
        int ref_order_n = 0, ref_writes = 0, ref_cancels = 0;
        uint32_t ref_pass = 0, ref_stop_ms = 0;
        GoveeEngineState ref_state = GoveeEngineIdle;
        for(size_t v = 0; v < sizeof(at_ms) / sizeof(at_ms[0]); v++) {
            sim_init(&sim, &eng, 2, 0);
            sim.lamp[0].drop_in_discovery = 1;
            sim_set_lamps(&sim, 0);
            set_bri(40);
            run_until_state(GoveeEngineDiscovering);
            CHECK_EQ(eng.visit_lamp, 0);
            uint32_t old_link = eng.link_gen;
            uint32_t old_timer = eng.timer_gen;
            if(mode == 2) sim.lamp[0].reachable = false; /* the retry connect never completes */
            for(int k = 0; k < 20000 && ring_count(GoveeEvLinkRetry) == 0; k++) sim_run(&sim, 1);
            CHECK_EQ(govee_engine_state(&eng), GoveeEngineConnecting);
            CHECK(eng.link_gen != old_link);
            if(mode == 1) {
                sim_run(&sim, 100);
                govee_engine_stop(&eng);
                CHECK_EQ(govee_engine_state(&eng), GoveeEngineCancelling);
            } else if(mode == 2) {
                run_until_state(GoveeEngineCancelling);
            }
            if(at_ms[v] >= 0) {
                uint32_t t = (uint32_t)at_ms[v];
                sim_inject(&sim, t, SimEvDisconnected, old_link);
                sim_inject(&sim, t + 1u, SimEvError, old_link);
                sim_inject(&sim, t + 2u, SimEvDiscovery, old_link);
                sim_inject(&sim, t + 3u, SimEvTimer, old_timer);
            }
            uint32_t stop_ms = 0;
            if(mode == 1) {
                uint32_t t0 = sim.now;
                run_until_state(GoveeEngineStopped);
                stop_ms = sim.now - t0;
            }
            sim_settle(&sim, 60000);
            CHECK_EQ(eng.fault, GoveeFaultNone);
            CHECK(!ring_has(GoveeEvUnexpected, 0xFE));
            if(mode == 0) {
                CHECK_EQ(govee_engine_state(&eng), GoveeEngineIdle);
                CHECK_EQ(L(0)->outcome, GoveeOutcomeSubmitted);
                CHECK_EQ(L(0)->last, GoveeLampLastOk);
                CHECK_EQ(L(1)->outcome, GoveeOutcomeSubmitted);
            } else if(mode == 1) {
                CHECK(govee_engine_is_stopped(&eng));
                CHECK_EQ(L(0)->outcome, GoveeOutcomeAborted);
                CHECK_EQ(L(0)->last, GoveeLampLastUnknown); /* no completed attempt yet */
                CHECK(stop_ms <= GOVEE_ENGINE_STOP_MAX_MS);
            } else {
                CHECK_EQ(govee_engine_state(&eng), GoveeEngineIdle);
                CHECK_EQ(L(0)->outcome, GoveeOutcomeConnectTimeout);
                CHECK_EQ(L(0)->last, GoveeLampLastFailed);
                CHECK_EQ(L(1)->outcome, GoveeOutcomeSubmitted);
            }
            CHECK_EQ(sim.connects_per_lamp[0], 2);
            CHECK_EQ(ring_count(GoveeEvLinkRetry), 1);
            if(at_ms[v] < 0) {
                for(int i = 0; i < 2; i++) ref[i] = *L(i);
                memcpy(ref_order, sim.connect_order, sizeof(ref_order));
                ref_order_n = sim.connect_order_n;
                ref_writes = sim.writes;
                ref_cancels = sim.cancels;
                ref_pass = eng.pass_no;
                ref_stop_ms = stop_ms;
                ref_state = govee_engine_state(&eng);
                continue;
            }
            CHECK(ring_has(GoveeEvStale, 0xFE));
            CHECK_EQ(govee_engine_state(&eng), ref_state);
            CHECK_EQ(eng.pass_no, ref_pass);
            CHECK_EQ(stop_ms, ref_stop_ms);
            CHECK_EQ(sim.connect_order_n, ref_order_n);
            CHECK(memcmp(sim.connect_order, ref_order, sizeof(int) * (size_t)ref_order_n) == 0);
            CHECK_EQ(sim.writes, ref_writes);
            CHECK_EQ(sim.cancels, ref_cancels);
            for(int i = 0; i < 2; i++) {
                CHECK_EQ(L(i)->outcome, ref[i].outcome);
                CHECK_EQ(L(i)->last, ref[i].last);
                CHECK_EQ(L(i)->reached_rev, ref[i].reached_rev);
                CHECK_EQ(L(i)->attempt_rev, ref[i].attempt_rev);
                CHECK(memcmp(&L(i)->times, &ref[i].times, sizeof(ref[i].times)) == 0);
            }
        }
    }
}

/* A lamp saved as failed that answers again is Ok after the pass, is saved and
 * loaded as ok, and in the next run gets normal priority and the 3 s deadline. */
static void test_saved_failed_lamp_recovers(void) {
    sim_init(&sim, &eng, 3, 0);
    const uint8_t last[3] = {GoveeLampLastFailed, GoveeLampLastOk, GoveeLampLastOk};
    govee_engine_set_lamps_last(&eng, sim.ids, last, 3, 0);
    uint32_t rev = set_bri(30);
    sim_settle(&sim, 60000);
    CHECK_EQ(sim.connect_order_n, 3);
    CHECK_EQ(sim.connect_order[2], 0); /* saved as failed: last in this run's first pass */
    CHECK_EQ(L(0)->times.connect_ms, 300);
    CHECK_EQ(L(0)->reached_rev, rev);
    CHECK_EQ(L(0)->last, GoveeLampLastOk);

    /* the app copies the results into the registry and saves; the next run loads them */
    GoveeRegistry reg;
    govee_registry_init(&reg);
    for(uint8_t i = 0; i < 3; i++) {
        CHECK_EQ(govee_registry_upsert(&reg, "lamp", &sim.ids[i], 1u, NULL), GoveeRegistryAdded);
        reg.lamps[i].last = L(i)->last;
    }
    static char buf[GOVEE_REGISTRY_TEXT_MAX];
    size_t len = 0;
    CHECK(govee_registry_serialize(&reg, buf, sizeof(buf), &len));
    GoveeRegistry loaded;
    CHECK_EQ(govee_registry_parse(&loaded, buf, len, NULL), GoveeRegistryParseOk);
    CHECK_EQ(loaded.count, 3);
    uint8_t loaded_last[3];
    for(uint8_t i = 0; i < 3; i++) loaded_last[i] = loaded.lamps[i].last;
    CHECK_EQ(loaded_last[0], GoveeLampLastOk);

    /* next run: the lamp does not answer this time, so its connect shows the deadline */
    sim_init(&sim, &eng, 3, 0);
    sim.lamp[0].reachable = false;
    govee_engine_set_lamps_last(&eng, sim.ids, loaded_last, 3, 0);
    CHECK_EQ(L(0)->last, GoveeLampLastOk);
    set_bri(31);
    sim_settle(&sim, 60000);
    CHECK_EQ(sim.connect_order_n, 3);
    CHECK_EQ(sim.connect_order[0], 0); /* normal priority: the fair order starts with it */
    CHECK_EQ(L(0)->times.connect_ms, GOVEE_ENGINE_CONNECT_DEADLINE_MS);
    CHECK_EQ(L(0)->outcome, GoveeOutcomeConnectTimeout);
}

void suite_engine(void) {
    RUN(test_unreachable_lamp_does_not_block);
    RUN(test_connect_timeout_cancel_next_runs);
    RUN(test_cancel_failure_ends_pass);
    RUN(test_single_lamp_link_reuse);
    RUN(test_only_differing_properties);
    RUN(test_change_during_pass_reaches_all);
    RUN(test_stale_events_ignored);
    RUN(test_lamp_removal_during_pass);
    RUN(test_target_change_and_remap);
    RUN(test_apply_again_with_pending_target_change);
    RUN(test_visit_failures);
    RUN(test_disconnect_timeout_and_stop);
    RUN(test_reentry_refused);
    RUN(test_event_ring);
    RUN(test_failed_lamp_visited_last);
    RUN(test_failed_lamp_retry_deadline);
    RUN(test_first_attempt_full_deadline);
    RUN(test_change_cancels_retry);
    RUN(test_visit_total_kept_tail);
    RUN(test_visit_total_tail_link_already_closed);
    RUN(test_late_events_after_supersede);
    RUN(test_early_loss_retried_once);
    RUN(test_early_loss_twice_fails);
    RUN(test_loss_after_write_not_retried);
    RUN(test_kept_link_early_loss);
    RUN(test_saved_failed_lamp_first_pass);
    RUN(test_saved_failed_superseded_and_aborted);
    RUN(test_disconnect_deadline);
    RUN(test_early_loss_retry_aborted_keeps_last);
    RUN(test_late_events_of_lost_link);
    RUN(test_saved_failed_lamp_recovers);
}

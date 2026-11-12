#include "govee_status.h"

#include <stdio.h>
#include <string.h>

bool govee_remote_validate(uint32_t signal, uint32_t arg, GoveeRemoteCmd* out) {
    bool ok = false;
    switch(signal) {
    case GoveeSignalStatus:
        ok = true; /* any arg, echoed as req= so the Mac tool can match its answer */
        break;
    case GoveeSignalScan:
    case GoveeSignalApply:
    case GoveeSignalClearEvents:
        ok = (arg == 0);
        break;
    case GoveeSignalPower:
    case GoveeSignalLightshow:
        ok = (arg <= 1u);
        break;
    case GoveeSignalBrightness:
        ok = (arg >= GOVEE_BRIGHTNESS_MIN && arg <= GOVEE_BRIGHTNESS_MAX);
        break;
    case GoveeSignalKelvin:
        ok = (arg >= GOVEE_KELVIN_MIN && arg <= GOVEE_KELVIN_MAX);
        break;
    case GoveeSignalRgb:
        ok = (arg <= 0xFFFFFFu);
        break;
    case GoveeSignalForget:
        ok = (arg >= 1u && arg <= GOVEE_MAX_LAMPS);
        break;
    case GoveeSignalTarget:
        ok = (arg <= GOVEE_MAX_LAMPS);
        break;
    default:
        ok = false;
        break;
    }
    if(ok && out) {
        out->signal = (GoveeSignal)signal;
        out->value = arg;
    }
    return ok;
}

/* Number of fixed lines before the lamp lines. */
#define HEADER_LINES (28u)
/* events= and events.total= */
#define EVENT_HEADER_LINES (2u)

static const char* or_unknown(const char* s) {
    return s ? s : "unknown";
}

static int header_line(const GoveeStatusInput* in, uint32_t k, char* line, size_t size) {
    const GoveeEngine* e = in->engine;
    const GoveeDesired* d = &e->desired;
    switch(k) {
    case 0:
        return snprintf(line, size, "seq=%lu\n", (unsigned long)in->seq);
    case 1:
        return snprintf(line, size, "session=%lu\n", (unsigned long)in->session);
    case 2:
        return snprintf(line, size, "req=%lu\n", (unsigned long)in->req);
    case 3:
        return snprintf(line, size, "uptime_ms=%lu\n", (unsigned long)in->now_ms);
    case 4:
        return snprintf(line, size, "app=%s\n", or_unknown(in->app_state));
    case 5:
        return snprintf(line, size, "inbox_overflow=%lu\n", (unsigned long)in->inbox_overflow);
    case 6:
        return snprintf(line, size, "target=%u\n", (unsigned)in->target);
    case 7:
        return snprintf(line, size, "scanning=%u\n", in->scanning ? 1u : 0u);
    case 8:
        return snprintf(line, size, "scan.refused=%u\n", in->scan_refused ? 1u : 0u);
    case 9:
        return snprintf(line, size, "lightshow=%u\n", in->lightshow ? 1u : 0u);
    case 10:
        return snprintf(line, size, "registry.count=%u\n", (unsigned)in->registry->count);
    case 11:
        return snprintf(line, size, "registry.max=%u\n", (unsigned)GOVEE_MAX_LAMPS);
    case 12:
        return snprintf(line, size, "registry.full=%u\n", in->registry_full ? 1u : 0u);
    case 13:
        return snprintf(line, size, "registry.load=%s\n", or_unknown(in->load_result));
    case 14:
        return snprintf(line, size, "registry.save=%s\n", or_unknown(in->save_result));
    case 15:
        return snprintf(line, size, "desired.rev=%lu\n", (unsigned long)e->rev);
    case 16:
        return snprintf(line, size, "desired.power=%u\n", d->power ? 1u : 0u);
    case 17:
        return snprintf(line, size, "desired.brightness=%u\n", (unsigned)d->brightness);
    case 18:
        return snprintf(
            line, size, "desired.mode=%s\n", d->mode == GoveeModeColour ? "colour" : "white");
    case 19:
        return snprintf(line, size, "desired.kelvin=%u\n", (unsigned)d->kelvin);
    case 20:
        return snprintf(line, size, "desired.rgb=%06lX\n", (unsigned long)d->rgb);
    case 21:
        return snprintf(
            line, size, "engine.state=%s\n", govee_engine_state_name(govee_engine_state(e)));
    case 22:
        return snprintf(
            line, size, "engine.fault=%s\n", govee_engine_fault_name((GoveeEngineFault)e->fault));
    case 23:
        return snprintf(line, size, "engine.pass=%lu\n", (unsigned long)e->pass_no);
    case 24:
        return snprintf(line, size, "engine.in_pass=%u\n", e->in_pass ? 1u : 0u);
    case 25:
        return snprintf(line, size, "engine.pending=%u\n", e->pending_valid ? 1u : 0u);
    case 26:
        return snprintf(
            line, size, "engine.targeted=%u\n", (unsigned)govee_engine_targeted_count(e));
    default:
        return snprintf(line, size, "lamps=%u\n", (unsigned)in->registry->count);
    }
}

static int lamp_line(const GoveeStatusInput* in, uint8_t i, char* line, size_t size) {
    const GoveeRegistryLamp* r = &in->registry->lamps[i];
    const GoveeEngine* e = in->engine;
    const GoveeEngineLamp* l = govee_engine_lamp(e, govee_engine_find_lamp(e, &r->id));
    GoveeEngineLamp empty;
    if(!l) {
        memset(&empty, 0, sizeof(empty));
        l = &empty;
    }
    char addr[18];
    govee_registry_format_addr(r->id.addr, addr);
    return snprintf(
        line,
        size,
        "lamp.%u=rev=%lu try=%lu out=%s targeted=%u conn_ms=%lu disc_ms=%lu write_ms=%lu "
        "dis_ms=%lu total_ms=%lu writes=%u reused=%u type=%u addr=%s name=%s\n",
        (unsigned)(i + 1u),
        (unsigned long)l->reached_rev,
        (unsigned long)l->attempt_rev,
        govee_engine_outcome_name((GoveeOutcome)l->outcome),
        l->targeted ? 1u : 0u,
        (unsigned long)l->times.connect_ms,
        (unsigned long)l->times.discover_ms,
        (unsigned long)l->times.write_ms,
        (unsigned long)l->times.disconnect_ms,
        (unsigned long)l->times.total_ms,
        (unsigned)l->times.writes,
        l->times.reused ? 1u : 0u,
        (unsigned)r->id.addr_type,
        addr,
        r->name);
}

static int event_line(const GoveeStatusInput* in, uint32_t i, char* line, size_t size) {
    uint32_t seq = 0;
    const GoveeEngineEvent* ev = govee_engine_event(in->engine, i, &seq);
    if(!ev) return snprintf(line, size, "ev.0=0 none lamp=- arg=0\n");
    char lamp[8];
    if(ev->lamp == GOVEE_LAMP_NONE) {
        snprintf(lamp, sizeof(lamp), "-");
    } else {
        snprintf(lamp, sizeof(lamp), "%u", (unsigned)(ev->lamp + 1u));
    }
    return snprintf(
        line,
        size,
        "ev.%lu=%lu %s lamp=%s arg=%lu\n",
        (unsigned long)seq,
        (unsigned long)ev->t_ms,
        govee_engine_event_name((GoveeEventType)ev->type),
        lamp,
        (unsigned long)ev->arg);
}

GoveeStatusLineResult govee_status_format_line(
    const GoveeStatusInput* in,
    uint32_t* cursor,
    char* line,
    size_t size) {
    if(size < 2u) {
        if(size == 1u) line[0] = '\0';
        return GoveeStatusLineDone;
    }
    uint32_t k = *cursor;
    uint32_t lamps = in->registry->count;
    uint32_t events = govee_engine_event_count(in->engine);
    int r;

    if(k < HEADER_LINES) {
        r = header_line(in, k, line, size);
    } else if(k < HEADER_LINES + lamps) {
        r = lamp_line(in, (uint8_t)(k - HEADER_LINES), line, size);
    } else if(k == HEADER_LINES + lamps) {
        r = snprintf(line, size, "events=%lu\n", (unsigned long)events);
    } else if(k == HEADER_LINES + lamps + 1u) {
        r = snprintf(
            line, size, "events.total=%lu\n", (unsigned long)in->engine->ring_total);
    } else if(k < HEADER_LINES + lamps + EVENT_HEADER_LINES + events) {
        r = event_line(in, k - (HEADER_LINES + lamps + EVENT_HEADER_LINES), line, size);
    } else if(k == HEADER_LINES + lamps + EVENT_HEADER_LINES + events) {
        r = snprintf(line, size, "end=%lu\n", (unsigned long)in->seq);
    } else {
        line[0] = '\0';
        return GoveeStatusLineDone;
    }
    *cursor = k + 1u;
    if(r < 0) {
        line[0] = '\n';
        line[1] = '\0';
        return GoveeStatusLineTruncated;
    }
    if((size_t)r >= size) {
        line[size - 2u] = '\n';
        line[size - 1u] = '\0';
        return GoveeStatusLineTruncated;
    }
    return GoveeStatusLineOk;
}

bool govee_status_format(const GoveeStatusInput* in, char* buf, size_t size, size_t* out_len) {
    size_t len = 0;
    bool complete = true;
    if(size > 0) buf[0] = '\0';
    uint32_t cursor = 0;
    char line[GOVEE_STATUS_LINE_MAX];
    /* bounded: header + 16 lamps + 2 + 64 events + end */
    for(uint32_t n = 0; n < HEADER_LINES + GOVEE_MAX_LAMPS + EVENT_HEADER_LINES +
                                GOVEE_ENGINE_EVENT_RING + 2u;
        n++) {
        GoveeStatusLineResult res = govee_status_format_line(in, &cursor, line, sizeof(line));
        if(res == GoveeStatusLineDone) break;
        size_t l = strlen(line);
        if(res == GoveeStatusLineTruncated || size == 0 || len + l >= size) {
            complete = false;
            break;
        }
        memcpy(buf + len, line, l + 1u);
        len += l;
    }
    if(out_len) *out_len = len;
    return complete;
}

#include "harness.h"
#include "sim.h"
#include "govee_status.h"

#include <string.h>

static const char* g_fixtures;
static GoveeEngine eng;
static Sim sim;
static GoveeRegistry reg;
static char text[GOVEE_STATUS_TEXT_MAX];

static void test_remote_validate(void) {
    GoveeRemoteCmd c;
    CHECK(govee_remote_validate(100, 0, &c) && c.signal == GoveeSignalStatus);
    /* the status request takes any arg and echoes it as req= */
    CHECK(govee_remote_validate(100, 1, &c) && c.signal == GoveeSignalStatus && c.value == 1u);
    CHECK(govee_remote_validate(100, 0xFFFFFFFFu, &c) && c.value == 0xFFFFFFFFu);
    CHECK(govee_remote_validate(101, 0, &c) && c.value == 0);
    CHECK(govee_remote_validate(101, 1, &c) && c.value == 1);
    CHECK(!govee_remote_validate(101, 2, &c));
    CHECK(!govee_remote_validate(102, 0, &c));
    CHECK(govee_remote_validate(102, 1, &c));
    CHECK(govee_remote_validate(102, 0x64, &c) && c.value == 100);
    CHECK(!govee_remote_validate(102, 0x65, &c));
    CHECK(!govee_remote_validate(103, 2699, &c));
    CHECK(govee_remote_validate(103, 2700, &c));
    CHECK(govee_remote_validate(103, 6500, &c));
    CHECK(!govee_remote_validate(103, 6501, &c));
    CHECK(govee_remote_validate(104, 0xFFFFFF, &c) && c.value == 0xFFFFFFu);
    CHECK(!govee_remote_validate(104, 0x1000000, &c));
    CHECK(govee_remote_validate(105, 0, &c));
    CHECK(govee_remote_validate(106, 0, &c));
    CHECK(!govee_remote_validate(106, 5, &c));
    CHECK(!govee_remote_validate(107, 0, &c));
    CHECK(govee_remote_validate(107, 16, &c));
    CHECK(!govee_remote_validate(107, 17, &c));
    CHECK(govee_remote_validate(108, 0, &c));
    CHECK(govee_remote_validate(108, 16, &c));
    CHECK(!govee_remote_validate(108, 17, &c));
    CHECK(govee_remote_validate(109, 1, &c));
    CHECK(!govee_remote_validate(109, 2, &c));
    CHECK(govee_remote_validate(110, 0, &c) && c.signal == GoveeSignalClearEvents);
    CHECK(!govee_remote_validate(99, 0, &c));
    CHECK(!govee_remote_validate(111, 0, &c));
    CHECK(!govee_remote_validate(2, 0, &c)); /* FuriSignalExit is handled by the glue */
    CHECK(govee_remote_validate(101, 1, NULL));
}

/* 3-lamp pass with lamp 2 unreachable, registry built from the sim lamps. */
static void build_scenario(GoveeStatusInput* in) {
    sim_init(&sim, &eng, 3, 5000);
    sim.lamp[1].reachable = false;
    govee_registry_init(&reg);
    static const char* const names[3] = {"Kitchen", "Hall = lamp 2", "Desk"};
    for(uint8_t i = 0; i < 3; i++) govee_registry_upsert(&reg, names[i], &sim.ids[i], 1731400000u, NULL);
    sim_set_lamps(&sim, 0);
    GoveeDesired d = eng.desired;
    d.brightness = 80;
    govee_engine_set_desired(&eng, &d);
    sim_settle(&sim, 60000);

    memset(in, 0, sizeof(*in));
    in->seq = 7;
    in->session = 0x5EED1234u;
    in->now_ms = sim.now;
    in->app_state = "home";
    in->inbox_overflow = 0;
    in->target = 0;
    in->load_result = "main";
    in->save_result = "ok";
    in->registry = &reg;
    in->engine = &eng;
}

static bool contains_line(const char* t, const char* line) {
    size_t n = strlen(line);
    const char* p = t;
    while((p = strstr(p, line)) != NULL) {
        if((p == t || p[-1] == '\n') && (p[n] == '\n' || p[n] == ' ')) return true;
        p += n;
    }
    return false;
}

static void write_fixture(const char* name, const char* data, size_t len) {
    if(!g_fixtures) return;
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", g_fixtures, name);
    FILE* f = fopen(path, "wb");
    CHECK(f != NULL);
    if(!f) return;
    CHECK_EQ(fwrite(data, 1, len, f), len);
    CHECK_EQ(fclose(f), 0);
}

static void test_status_format(void) {
    GoveeStatusInput in;
    build_scenario(&in);
    size_t len = 0;
    CHECK(govee_status_format(&in, text, sizeof(text), &len));
    CHECK_EQ(strlen(text), len);
    CHECK(strncmp(text, "seq=7\n", 6) == 0);
    CHECK(len > 6 && strcmp(text + len - 6, "end=7\n") == 0);
    CHECK(strncmp(text, "seq=7\nsession=1592594996\nreq=0\n", 31) == 0);
    /* the request arg is echoed */
    GoveeStatusInput echoed = in;
    echoed.req = 0x2Au;
    char echo_text[GOVEE_STATUS_TEXT_MAX];
    CHECK(govee_status_format(&echoed, echo_text, sizeof(echo_text), &len));
    CHECK(strncmp(echo_text, "seq=7\nsession=1592594996\nreq=42\n", 32) == 0);
    /* a refused scan start is reported */
    GoveeStatusInput refused = in;
    refused.scan_refused = true;
    CHECK(govee_status_format(&refused, echo_text, sizeof(echo_text), &len));
    CHECK(contains_line(echo_text, "scanning=0"));
    CHECK(contains_line(echo_text, "scan.refused=1"));
    CHECK(govee_status_format(&in, text, sizeof(text), &len));
    CHECK(contains_line(text, "app=home"));
    CHECK(contains_line(text, "scanning=0"));
    CHECK(contains_line(text, "scan.refused=0"));
    CHECK(contains_line(text, "desired.rev=1"));
    CHECK(contains_line(text, "desired.brightness=80"));
    CHECK(contains_line(text, "desired.mode=white"));
    CHECK(contains_line(text, "engine.state=idle"));
    CHECK(contains_line(text, "lamps=3"));
    CHECK(contains_line(text, "lamp.1=rev=1 try=1 out=submitted"));
    CHECK(contains_line(text, "lamp.2=rev=0 try=1 out=connect_timeout"));
    CHECK(contains_line(text, "lamp.3=rev=1 try=1 out=submitted"));
    CHECK(strstr(text, "conn_ms=3000 ") != NULL);
    CHECK(strstr(text, "name=Hall = lamp 2\n") != NULL);
    CHECK(strstr(text, " connect_timeout lamp=2 ") != NULL);
    /* only ASCII, '\n' line ends */
    bool ascii = true;
    for(size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)text[i];
        if(ch != '\n' && (ch < 0x20u || ch > 0x7Eu)) ascii = false;
    }
    CHECK(ascii);

    /* streaming with a line buffer gives the same text */
    char streamed[GOVEE_STATUS_TEXT_MAX];
    size_t sl = 0;
    uint32_t cursor = 0;
    char line[GOVEE_STATUS_LINE_MAX];
    for(int n = 0; n < 200; n++) {
        GoveeStatusLineResult r = govee_status_format_line(&in, &cursor, line, sizeof(line));
        if(r == GoveeStatusLineDone) break;
        CHECK_EQ(r, GoveeStatusLineOk);
        size_t l = strlen(line);
        memcpy(streamed + sl, line, l + 1u);
        sl += l;
    }
    CHECK_EQ(sl, len);
    CHECK(strcmp(streamed, text) == 0);

    write_fixture("remote_status_sample.txt", text, len);

    /* truncation: cut at a line boundary, no end= line */
    char small[700];
    size_t sml = 0;
    CHECK(!govee_status_format(&in, small, sizeof(small), &sml));
    CHECK(sml > 0 && small[sml - 1u] == '\n');
    CHECK(strstr(small, "end=") == NULL);
    write_fixture("remote_status_partial.txt", small, sml);

    /* a too-short line buffer reports truncation */
    cursor = 28; /* first lamp line */
    CHECK_EQ(govee_status_format_line(&in, &cursor, line, 20), GoveeStatusLineTruncated);
    CHECK_EQ(strlen(line), 19);
    CHECK_EQ(line[18], '\n');
}

/* Worst case: 16 lamps with 31-character names and a full ring fit the documented buffer. */
static void test_status_worst_case_fits(void) {
    sim_init(&sim, &eng, GOVEE_MAX_LAMPS, 0xFFFFFF00u);
    govee_registry_init(&reg);
    for(uint8_t i = 0; i < GOVEE_MAX_LAMPS; i++) {
        sim.lamp[i].reachable = false;
        govee_registry_upsert(&reg, "0123456789012345678901234567890123", &sim.ids[i], 0xFFFFFFFFu, NULL);
    }
    sim_set_lamps(&sim, 0);
    GoveeDesired d = eng.desired;
    d.mode = GoveeModeColour;
    d.rgb = 0xABCDEF;
    govee_engine_set_desired(&eng, &d);
    sim_settle(&sim, 120000);
    for(uint32_t i = 0; i < 200; i++) govee_engine_on_timer(&eng, 0xFFFFFFF0u - i);
    for(uint8_t i = 0; i < GOVEE_MAX_LAMPS; i++) {
        eng.lamps[i].reached_rev = 0xFFFFFFFFu;
        eng.lamps[i].attempt_rev = 0xFFFFFFFFu;
        eng.lamps[i].times.connect_ms = 0xFFFFFFFFu;
        eng.lamps[i].times.discover_ms = 0xFFFFFFFFu;
        eng.lamps[i].times.write_ms = 0xFFFFFFFFu;
        eng.lamps[i].times.disconnect_ms = 0xFFFFFFFFu;
        eng.lamps[i].times.total_ms = 0xFFFFFFFFu;
        eng.lamps[i].outcome = GoveeOutcomeDiscoveryFailed;
    }
    GoveeStatusInput in;
    memset(&in, 0, sizeof(in));
    in.seq = 0xFFFFFFFFu;
    in.session = 0xFFFFFFFFu;
    in.req = 0xFFFFFFFFu;
    in.scan_refused = true;
    in.now_ms = 0xFFFFFFFFu;
    in.app_state = "lightshow_running";
    in.inbox_overflow = 0xFFFFFFFFu;
    in.target = 16;
    in.load_result = "corrupt_kept";
    in.save_result = "write_failed";
    in.registry = &reg;
    in.engine = &eng;
    size_t len = 0;
    CHECK(govee_status_format(&in, text, sizeof(text), &len));
    CHECK(len < GOVEE_STATUS_TEXT_MAX);
    CHECK(strstr(text, "\nend=4294967295\n") != NULL);
    printf("  worst-case status text: %u of %u bytes\n", (unsigned)len, (unsigned)GOVEE_STATUS_TEXT_MAX);
}

void suite_status(const char* fixtures_dir) {
    g_fixtures = fixtures_dir;
    RUN(test_remote_validate);
    RUN(test_status_format);
    RUN(test_status_worst_case_fits);
}

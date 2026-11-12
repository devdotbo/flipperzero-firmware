#include "harness.h"
#include "govee_registry.h"

#include <string.h>

/* Exactly what storage/bulb_cache.c writes (format string at bulb_cache.c:310-323). */
static const char old_file[] =
    "[\n"
    "  {\"name\": \"ihoment_H6006_1A2B\", \"address\": \"A4:C1:38:12:34:56\", \"addr_type\": 0, "
    "\"last_seen_ts\": 1731400000},\n"
    "  {\"name\": \"Kitchen lamp\", \"address\": \"D0:3E:7D:AA:BB:CC\", \"addr_type\": 1, "
    "\"last_seen_ts\": 0}\n"
    "]\n";

static GoveeLampId mk_id(uint8_t type, uint8_t last) {
    GoveeLampId id = {.addr_type = type, .addr = {0xA4, 0xC1, 0x38, 0x00, 0x00, last}};
    return id;
}

static void test_old_format_loads(void) {
    GoveeRegistry reg;
    size_t pos = 99;
    CHECK_EQ(govee_registry_parse(&reg, old_file, strlen(old_file), &pos), GoveeRegistryParseOk);
    CHECK_EQ(pos, 0);
    CHECK_EQ(reg.count, 2);
    CHECK(strcmp(reg.lamps[0].name, "ihoment_H6006_1A2B") == 0);
    CHECK_EQ(reg.lamps[0].id.addr[0], 0xA4);
    CHECK_EQ(reg.lamps[0].id.addr[5], 0x56);
    CHECK_EQ(reg.lamps[0].id.addr_type, 0);
    CHECK_EQ(reg.lamps[0].last_seen_ts, 1731400000u);
    CHECK(strcmp(reg.lamps[1].name, "Kitchen lamp") == 0);
    CHECK_EQ(reg.lamps[1].id.addr_type, 1);
    CHECK_EQ(reg.lamps[1].id.addr[3], 0xAA);

    /* the serialiser writes the same bytes the old app wrote */
    char buf[GOVEE_REGISTRY_TEXT_MAX];
    size_t len = 0;
    CHECK(govee_registry_serialize(&reg, buf, sizeof(buf), &len));
    CHECK_EQ(len, strlen(old_file));
    CHECK(strcmp(buf, old_file) == 0);

    /* empty array, no trailing newline, lowercase hex, unknown scalar keys, escapes,
     * missing optional fields */
    const char* variants =
        "[{\"address\":\"a4:c1:38:00:00:01\",\"addr_type\":0,\"rssi\":-1,"
        "\"future\":\"x\",\"flag\":true,\"n\":null,\"name\":\"A\\u0042\\\"C\\\\\\n\"}]";
    CHECK_EQ(govee_registry_parse(&reg, "[]", 2, NULL), GoveeRegistryParseOk);
    CHECK_EQ(reg.count, 0);
    /* "rssi": -1 is a signed number: rejected as a whole */
    CHECK(govee_registry_parse(&reg, variants, strlen(variants), NULL) != GoveeRegistryParseOk);
    const char* variants_ok =
        "[{\"address\":\"a4:c1:38:00:00:01\",\"addr_type\":0,\"rssi\":7,"
        "\"future\":\"x\",\"flag\":true,\"n\":null,\"name\":\"A\\u0042\\\"C\\\\\\n\"}]";
    CHECK_EQ(govee_registry_parse(&reg, variants_ok, strlen(variants_ok), NULL), GoveeRegistryParseOk);
    CHECK_EQ(reg.count, 1);
    CHECK_EQ(reg.lamps[0].id.addr[5], 1);
    CHECK_EQ(reg.lamps[0].last_seen_ts, 0);
    CHECK(strcmp(reg.lamps[0].name, "AB_C__") == 0);
}

static void test_malformed_rejected_as_whole(void) {
    static const char* const bad[] = {
        "",
        "   \n",
        "[",
        "{}",
        "[{}]",
        "[,]",
        "[{\"name\": \"a\", \"address\": \"A4:C1:38:12:34:56\", \"addr_type\": 0},]",
        "[{\"name\": \"a\", \"address\": \"A4:C1:38:12:34:56\", \"addr_type\": 0}",
        "[{\"name\": \"a\", \"address\": \"A4:C1:38:12:34:56\", \"addr_type\": 0}] x",
        "[{\"name\": \"a\", \"address\": \"A4:C1:38:12:34\", \"addr_type\": 0}]",
        "[{\"name\": \"a\", \"address\": \"A4-C1-38-12-34-56\", \"addr_type\": 0}]",
        "[{\"name\": \"a\", \"address\": \"G4:C1:38:12:34:56\", \"addr_type\": 0}]",
        "[{\"name\": \"a\", \"address\": \"A4:C1:38:12:34:56\", \"addr_type\": 4}]",
        "[{\"name\": \"a\", \"address\": \"A4:C1:38:12:34:56\", \"addr_type\": 1.0}]",
        "[{\"name\": \"a\", \"address\": \"A4:C1:38:12:34:56\", \"addr_type\": 01}]",
        "[{\"name\": \"a\", \"address\": \"A4:C1:38:12:34:56\"}]",
        "[{\"name\": \"a\", \"addr_type\": 0}]",
        "[{\"name\": \"a\", \"address\": \"A4:C1:38:12:34:56\", \"addr_type\": 0, \"last_seen_ts\": 4294967296}]",
        "[{\"name\": \"a\", \"address\": \"A4:C1:38:12:34:56\", \"addr_type\": 0, \"x\": {\"y\": 1}}]",
        "[{\"name\": \"a\", \"address\": \"A4:C1:38:12:34:56\", \"addr_type\": 0, \"x\": [1]}]",
        "[{\"name\": \"a\", \"address\": \"A4:C1:38:12:34:56\", \"addr_type\": 0, \"addr_type\": 1}]",
        "[{\"name\": \"a\\q\", \"address\": \"A4:C1:38:12:34:56\", \"addr_type\": 0}]",
        "[{\"name\": \"a\tb\", \"address\": \"A4:C1:38:12:34:56\", \"addr_type\": 0}]",
        "[{\"name\": \"unterminated, \"address\": \"A4:C1:38:12:34:56\", \"addr_type\": 0}]",
        ("[{\"name\": \"a\", \"address\": \"A4:C1:38:12:34:56\", \"addr_type\": 0},"
         " {\"name\": \"b\", \"address\": \"a4:c1:38:12:34:56\", \"addr_type\": 0}]"),
        "[{\"name\": \"a\" \"address\": \"A4:C1:38:12:34:56\", \"addr_type\": 0}]",
        "[{\"name\": \"a\", \"address\": \"A4:C1:38:12:34:56\", \"addr_type\": tru}]",
    };
    GoveeRegistry reg;
    for(size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        /* first load something valid so a failure must clear it */
        govee_registry_parse(&reg, old_file, strlen(old_file), NULL);
        size_t pos = 0;
        GoveeRegistryParseResult r = govee_registry_parse(&reg, bad[i], strlen(bad[i]), &pos);
        if(r == GoveeRegistryParseOk) fprintf(stderr, "  accepted bad input %u: %s\n", (unsigned)i, bad[i]);
        CHECK(r != GoveeRegistryParseOk);
        CHECK_EQ(reg.count, 0);
    }
    /* distinct results for a few */
    CHECK_EQ(govee_registry_parse(&reg, "", 0, NULL), GoveeRegistryParseEmpty);
    CHECK_EQ(govee_registry_parse(&reg, bad[15], strlen(bad[15]), NULL), GoveeRegistryParseMissingField);
    CHECK_EQ(govee_registry_parse(&reg, bad[12], strlen(bad[12]), NULL), GoveeRegistryParseBadValue);
    CHECK_EQ(govee_registry_parse(&reg, bad[20], strlen(bad[20]), NULL), GoveeRegistryParseDuplicateKey);
    CHECK_EQ(govee_registry_parse(&reg, bad[24], strlen(bad[24]), NULL), GoveeRegistryParseDuplicateLamp);

    /* embedded NUL inside the given length */
    char with_nul[] = "[{\"name\": \"a\0b\", \"address\": \"A4:C1:38:12:34:56\", \"addr_type\": 0}]";
    CHECK(govee_registry_parse(&reg, with_nul, sizeof(with_nul) - 1u, NULL) != GoveeRegistryParseOk);
    /* text truncated in the middle (partial write) */
    CHECK(govee_registry_parse(&reg, old_file, strlen(old_file) - 5u, NULL) != GoveeRegistryParseOk);

    /* 17 objects: too many */
    char big[GOVEE_REGISTRY_TEXT_MAX * 2];
    size_t len = 0;
    len += (size_t)snprintf(big + len, sizeof(big) - len, "[");
    for(unsigned i = 0; i < 17; i++) {
        len += (size_t)snprintf(
            big + len,
            sizeof(big) - len,
            "%s{\"name\": \"n%u\", \"address\": \"A4:C1:38:00:00:%02X\", \"addr_type\": 0}",
            i ? "," : "",
            i,
            i);
    }
    len += (size_t)snprintf(big + len, sizeof(big) - len, "]");
    CHECK_EQ(govee_registry_parse(&reg, big, len, NULL), GoveeRegistryParseTooMany);
    CHECK_EQ(reg.count, 0);
}

static void test_name_sanitising(void) {
    char out[GOVEE_NAME_LEN];
    CHECK_EQ(govee_registry_sanitize_name(out, sizeof(out), "  K\xc3\xbc\"che\\ 1\x01  "), 11);
    CHECK(strcmp(out, "K___che_ 1_") == 0);
    govee_registry_sanitize_name(out, sizeof(out), NULL);
    CHECK(strcmp(out, "") == 0);
    govee_registry_sanitize_name(out, sizeof(out), "0123456789012345678901234567890123456789");
    CHECK_EQ(strlen(out), GOVEE_NAME_LEN - 1u);
    char tiny[4];
    CHECK_EQ(govee_registry_sanitize_name(tiny, sizeof(tiny), "abcdef"), 3);
    CHECK(strcmp(tiny, "abc") == 0);

    GoveeRegistry reg;
    govee_registry_init(&reg);
    GoveeLampId id = mk_id(0, 1);
    govee_registry_upsert(&reg, "bad\"name\\\x7f", &id, 1, NULL);
    CHECK(strcmp(reg.lamps[0].name, "bad_name__") == 0);
}

static void test_upsert_duplicate_updates(void) {
    GoveeRegistry reg;
    govee_registry_init(&reg);
    GoveeLampId id = mk_id(0, 1);
    uint8_t idx = 99;
    CHECK_EQ(govee_registry_upsert(&reg, "first", &id, 10, &idx), GoveeRegistryAdded);
    CHECK_EQ(idx, 0);
    CHECK_EQ(govee_registry_upsert(&reg, "second", &id, 20, &idx), GoveeRegistryUpdated);
    CHECK_EQ(reg.count, 1);
    CHECK(strcmp(reg.lamps[0].name, "second") == 0);
    CHECK_EQ(reg.lamps[0].last_seen_ts, 20);
    /* an empty name from a scan without the scan response keeps the stored name */
    CHECK_EQ(govee_registry_upsert(&reg, "", &id, 30, &idx), GoveeRegistryUpdated);
    CHECK(strcmp(reg.lamps[0].name, "second") == 0);
    CHECK_EQ(reg.lamps[0].last_seen_ts, 30);
    /* same address, other address type: a different identity */
    GoveeLampId id_random = mk_id(1, 1);
    CHECK_EQ(govee_registry_upsert(&reg, "random", &id_random, 1, &idx), GoveeRegistryAdded);
    CHECK_EQ(reg.count, 2);
    CHECK_EQ(govee_registry_find(&reg, &id_random), 1);
    GoveeLampId bad = mk_id(4, 1);
    CHECK_EQ(govee_registry_upsert(&reg, "x", &bad, 1, NULL), GoveeRegistryInvalid);
    CHECK_EQ(govee_registry_upsert(&reg, "x", NULL, 1, NULL), GoveeRegistryInvalid);
    /* remove shifts later lamps */
    CHECK(govee_registry_remove(&reg, 0));
    CHECK_EQ(reg.count, 1);
    CHECK(strcmp(reg.lamps[0].name, "random") == 0);
    CHECK(!govee_registry_remove(&reg, 1));
}

static void test_seventeenth_lamp_refused(void) {
    GoveeRegistry reg;
    govee_registry_init(&reg);
    for(uint8_t i = 0; i < GOVEE_MAX_LAMPS; i++) {
        GoveeLampId id = mk_id(0, i);
        CHECK_EQ(govee_registry_upsert(&reg, "lamp", &id, i, NULL), GoveeRegistryAdded);
    }
    GoveeLampId extra = mk_id(0, 0xEE);
    CHECK_EQ(govee_registry_upsert(&reg, "extra", &extra, 0, NULL), GoveeRegistryFull);
    CHECK_EQ(reg.count, GOVEE_MAX_LAMPS);
    /* an existing lamp still updates when full */
    GoveeLampId first = mk_id(0, 0);
    CHECK_EQ(govee_registry_upsert(&reg, "renamed", &first, 5, NULL), GoveeRegistryUpdated);
}

static void test_round_trip(void) {
    GoveeRegistry reg;
    govee_registry_init(&reg);
    for(uint8_t i = 0; i < GOVEE_MAX_LAMPS; i++) {
        GoveeLampId id = mk_id((uint8_t)(i % 2u), i);
        id.addr[3] = (uint8_t)(0xF0u + i);
        char name[GOVEE_NAME_LEN];
        snprintf(name, sizeof(name), "Lamp %u with a long name %u", (unsigned)i, (unsigned)i * 7u);
        govee_registry_upsert(&reg, name, &id, 0xFFFFFFF0u + i, NULL);
    }
    char a[GOVEE_REGISTRY_TEXT_MAX];
    char b[GOVEE_REGISTRY_TEXT_MAX];
    size_t la = 0, lb = 0;
    CHECK(govee_registry_serialize(&reg, a, sizeof(a), &la));
    CHECK(la < GOVEE_REGISTRY_TEXT_MAX);
    GoveeRegistry back;
    CHECK_EQ(govee_registry_parse(&back, a, la, NULL), GoveeRegistryParseOk);
    CHECK_EQ(back.count, GOVEE_MAX_LAMPS);
    CHECK(memcmp(&reg, &back, sizeof(reg)) == 0);
    CHECK(govee_registry_serialize(&back, b, sizeof(b), &lb));
    CHECK_EQ(la, lb);
    CHECK(strcmp(a, b) == 0);

    /* truncation is reported */
    char small[100];
    size_t ls = 0;
    CHECK(!govee_registry_serialize(&reg, small, sizeof(small), &ls));
    CHECK(ls < sizeof(small));
    CHECK_EQ(small[ls], '\0');
    GoveeRegistry empty;
    govee_registry_init(&empty);
    CHECK(govee_registry_serialize(&empty, small, sizeof(small), &ls));
    CHECK(strcmp(small, "[\n]\n") == 0);
    CHECK_EQ(govee_registry_parse(&back, small, ls, NULL), GoveeRegistryParseOk);
}

/* The optional "ok" field (result of the lamp's last completed attempt). */
static void test_registry_ok_field(void) {
    GoveeRegistry reg;
    /* old format: every lamp unknown, and serialising adds nothing */
    CHECK_EQ(govee_registry_parse(&reg, old_file, strlen(old_file), NULL), GoveeRegistryParseOk);
    CHECK_EQ(reg.lamps[0].last, GoveeLampLastUnknown);
    CHECK_EQ(reg.lamps[1].last, GoveeLampLastUnknown);

    /* round trip with the field on two lamps and without it on the third */
    GoveeRegistry a;
    govee_registry_init(&a);
    GoveeLampId id1 = mk_id(0, 1), id2 = mk_id(0, 2), id3 = mk_id(1, 3);
    CHECK_EQ(govee_registry_upsert(&a, "one", &id1, 5, NULL), GoveeRegistryAdded);
    CHECK_EQ(govee_registry_upsert(&a, "two", &id2, 6, NULL), GoveeRegistryAdded);
    CHECK_EQ(govee_registry_upsert(&a, "three", &id3, 7, NULL), GoveeRegistryAdded);
    CHECK_EQ(a.lamps[0].last, GoveeLampLastUnknown); /* new lamps start unknown */
    a.lamps[0].last = GoveeLampLastOk;
    a.lamps[1].last = GoveeLampLastFailed;
    char buf[GOVEE_REGISTRY_TEXT_MAX];
    size_t len = 0;
    CHECK(govee_registry_serialize(&a, buf, sizeof(buf), &len));
    CHECK(strstr(buf,
                 "{\"name\": \"one\", \"address\": \"A4:C1:38:00:00:01\", \"addr_type\": 0, "
                 "\"last_seen_ts\": 5, \"ok\": 1},\n") != NULL);
    CHECK(strstr(buf, "\"last_seen_ts\": 6, \"ok\": 0},\n") != NULL);
    CHECK(strstr(buf, "\"last_seen_ts\": 7}\n") != NULL);
    GoveeRegistry b;
    CHECK_EQ(govee_registry_parse(&b, buf, len, NULL), GoveeRegistryParseOk);
    CHECK_EQ(b.count, 3);
    CHECK_EQ(b.lamps[0].last, GoveeLampLastOk);
    CHECK_EQ(b.lamps[1].last, GoveeLampLastFailed);
    CHECK_EQ(b.lamps[2].last, GoveeLampLastUnknown);
    /* upsert of a known lamp keeps the field; remove moves it with its lamp */
    CHECK_EQ(govee_registry_upsert(&b, "uno", &id1, 9, NULL), GoveeRegistryUpdated);
    CHECK_EQ(b.lamps[0].last, GoveeLampLastOk);
    CHECK(govee_registry_remove(&b, 0));
    CHECK_EQ(b.lamps[0].last, GoveeLampLastFailed);

    /* malformed "ok" rejects the whole file */
    static const char* const bad[] = {
        "[{\"address\":\"A4:C1:38:00:00:01\",\"addr_type\":0,\"ok\":2}]",
        "[{\"address\":\"A4:C1:38:00:00:01\",\"addr_type\":0,\"ok\":true}]",
        "[{\"address\":\"A4:C1:38:00:00:01\",\"addr_type\":0,\"ok\":\"1\"}]",
        "[{\"address\":\"A4:C1:38:00:00:01\",\"addr_type\":0,\"ok\":-1}]",
        "[{\"address\":\"A4:C1:38:00:00:01\",\"addr_type\":0,\"ok\":1,\"ok\":0}]",
        ("[{\"address\":\"A4:C1:38:00:00:01\",\"addr_type\":0,\"ok\":1},"
         "{\"address\":\"A4:C1:38:00:00:02\",\"addr_type\":0,\"ok\":01}]"),
    };
    for(size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        b.count = 5;
        CHECK(govee_registry_parse(&b, bad[i], strlen(bad[i]), NULL) != GoveeRegistryParseOk);
        CHECK_EQ(b.count, 0);
    }
    const char* ok0 = "[{\"ok\":0,\"address\":\"A4:C1:38:00:00:01\",\"addr_type\":0}]";
    CHECK_EQ(govee_registry_parse(&b, ok0, strlen(ok0), NULL), GoveeRegistryParseOk);
    CHECK_EQ(b.lamps[0].last, GoveeLampLastFailed);
}

void suite_registry(void) {
    RUN(test_old_format_loads);
    RUN(test_malformed_rejected_as_whole);
    RUN(test_name_sanitising);
    RUN(test_upsert_duplicate_updates);
    RUN(test_seventeenth_lamp_refused);
    RUN(test_round_trip);
    RUN(test_registry_ok_field);
}

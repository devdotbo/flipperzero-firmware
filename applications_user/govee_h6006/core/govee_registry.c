#include "govee_registry.h"

#include <stdio.h>
#include <string.h>

void govee_registry_init(GoveeRegistry* reg) {
    memset(reg, 0, sizeof(*reg));
}

static bool name_char_allowed(unsigned char c) {
    return c >= 0x20u && c <= 0x7Eu && c != '"' && c != '\\';
}

size_t govee_registry_sanitize_name(char* dst, size_t dst_size, const char* src) {
    if(dst_size == 0) return 0;
    size_t n = 0;
    if(src) {
        for(size_t i = 0; src[i] != '\0' && n + 1 < dst_size; i++) {
            unsigned char c = (unsigned char)src[i];
            dst[n++] = name_char_allowed(c) ? (char)c : '_';
        }
    }
    dst[n] = '\0';
    /* trim trailing spaces */
    while(n > 0 && dst[n - 1] == ' ') {
        dst[--n] = '\0';
    }
    /* trim leading spaces */
    size_t lead = 0;
    while(lead < n && dst[lead] == ' ') lead++;
    if(lead > 0) {
        memmove(dst, dst + lead, n - lead + 1);
        n -= lead;
    }
    return n;
}

uint8_t govee_registry_find(const GoveeRegistry* reg, const GoveeLampId* id) {
    for(uint8_t i = 0; i < reg->count && i < GOVEE_MAX_LAMPS; i++) {
        if(govee_lamp_id_equal(&reg->lamps[i].id, id)) return i;
    }
    return GOVEE_LAMP_NONE;
}

GoveeRegistryResult govee_registry_upsert(
    GoveeRegistry* reg,
    const char* name,
    const GoveeLampId* id,
    uint32_t last_seen_ts,
    uint8_t* out_index) {
    if(!reg || !id || id->addr_type > GOVEE_ADDR_TYPE_MAX) return GoveeRegistryInvalid;

    char clean[GOVEE_NAME_LEN];
    size_t clean_len = govee_registry_sanitize_name(clean, sizeof(clean), name);

    uint8_t idx = govee_registry_find(reg, id);
    if(idx != GOVEE_LAMP_NONE) {
        GoveeRegistryLamp* lamp = &reg->lamps[idx];
        if(clean_len > 0) memcpy(lamp->name, clean, clean_len + 1);
        lamp->last_seen_ts = last_seen_ts;
        if(out_index) *out_index = idx;
        return GoveeRegistryUpdated;
    }

    if(reg->count >= GOVEE_MAX_LAMPS) return GoveeRegistryFull;

    GoveeRegistryLamp* lamp = &reg->lamps[reg->count];
    memset(lamp, 0, sizeof(*lamp));
    memcpy(lamp->name, clean, clean_len + 1);
    lamp->id = *id;
    lamp->last_seen_ts = last_seen_ts;
    if(out_index) *out_index = reg->count;
    reg->count++;
    return GoveeRegistryAdded;
}

bool govee_registry_remove(GoveeRegistry* reg, uint8_t index) {
    if(index >= reg->count) return false;
    for(uint8_t i = index; i + 1u < reg->count; i++) {
        reg->lamps[i] = reg->lamps[i + 1u];
    }
    reg->count--;
    memset(&reg->lamps[reg->count], 0, sizeof(reg->lamps[0]));
    return true;
}

void govee_registry_format_addr(const uint8_t addr[GOVEE_ADDR_LEN], char buf[18]) {
    snprintf(
        buf,
        18,
        "%02X:%02X:%02X:%02X:%02X:%02X",
        (unsigned)addr[0],
        (unsigned)addr[1],
        (unsigned)addr[2],
        (unsigned)addr[3],
        (unsigned)addr[4],
        (unsigned)addr[5]);
}

/* ------------------------------------------------------------------------- */
/* Parser                                                                    */
/* ------------------------------------------------------------------------- */

typedef struct {
    const char* text;
    size_t len;
    size_t pos;
    GoveeRegistryParseResult err;
} Cursor;

static bool fail(Cursor* c, GoveeRegistryParseResult err) {
    if(c->err == GoveeRegistryParseOk) c->err = err;
    return false;
}

static int peek(const Cursor* c) {
    return c->pos < c->len ? (unsigned char)c->text[c->pos] : -1;
}

static void skip_ws(Cursor* c) {
    while(c->pos < c->len) {
        char ch = c->text[c->pos];
        if(ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') break;
        c->pos++;
    }
}

static bool expect(Cursor* c, char ch) {
    skip_ws(c);
    if(peek(c) != (unsigned char)ch) return fail(c, GoveeRegistryParseSyntax);
    c->pos++;
    return true;
}

static int hex_value(int ch) {
    if(ch >= '0' && ch <= '9') return ch - '0';
    if(ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if(ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

/* Parse a JSON string at the cursor. Decoded bytes go to buf (truncated to
 * size - 1, NUL-terminated). *too_long is set when bytes were dropped. */
static bool parse_string(Cursor* c, char* buf, size_t size, bool* too_long) {
    size_t n = 0;
    *too_long = false;
    skip_ws(c);
    if(peek(c) != '"') return fail(c, GoveeRegistryParseSyntax);
    c->pos++;
    while(c->pos < c->len) {
        unsigned char ch = (unsigned char)c->text[c->pos++];
        if(ch == '"') {
            buf[n] = '\0';
            return true;
        }
        if(ch < 0x20u) return fail(c, GoveeRegistryParseSyntax); /* raw control or NUL */
        if(ch == '\\') {
            int e = peek(c);
            if(e < 0) break;
            c->pos++;
            switch(e) {
            case '"':
            case '\\':
            case '/':
                ch = (unsigned char)e;
                break;
            case 'b':
            case 'f':
            case 'n':
            case 'r':
            case 't':
                ch = '_'; /* control characters are not kept in names */
                break;
            case 'u': {
                uint32_t code = 0;
                for(int k = 0; k < 4; k++) {
                    int h = hex_value(peek(c));
                    if(h < 0) return fail(c, GoveeRegistryParseSyntax);
                    code = (code << 4) | (uint32_t)h;
                    c->pos++;
                }
                ch = (code >= 0x20u && code <= 0x7Eu) ? (unsigned char)code : (unsigned char)'_';
                break;
            }
            default:
                return fail(c, GoveeRegistryParseSyntax);
            }
        }
        if(n + 1 < size) {
            buf[n++] = (char)ch;
        } else {
            *too_long = true;
        }
    }
    return fail(c, GoveeRegistryParseSyntax); /* unterminated */
}

/* Unsigned JSON integer without sign, fraction or exponent, at most UINT32_MAX. */
static bool parse_uint32(Cursor* c, uint32_t* out) {
    skip_ws(c);
    int ch = peek(c);
    if(ch < '0' || ch > '9') return fail(c, GoveeRegistryParseSyntax);
    if(ch == '0' && c->pos + 1 < c->len) {
        char next = c->text[c->pos + 1];
        if(next >= '0' && next <= '9') return fail(c, GoveeRegistryParseSyntax);
    }
    uint32_t v = 0;
    while((ch = peek(c)) >= '0' && ch <= '9') {
        uint32_t d = (uint32_t)(ch - '0');
        if(v > (UINT32_MAX - d) / 10u) return fail(c, GoveeRegistryParseBadValue);
        v = v * 10u + d;
        c->pos++;
    }
    if(ch == '.' || ch == 'e' || ch == 'E') return fail(c, GoveeRegistryParseSyntax);
    *out = v;
    return true;
}

static bool parse_literal(Cursor* c, const char* lit) {
    size_t n = strlen(lit);
    if(c->len - c->pos < n || memcmp(c->text + c->pos, lit, n) != 0) {
        return fail(c, GoveeRegistryParseSyntax);
    }
    c->pos += n;
    return true;
}

/* Skip the value of an unknown key: scalar only. */
static bool skip_scalar(Cursor* c) {
    skip_ws(c);
    int ch = peek(c);
    if(ch == '"') {
        char tmp[8];
        bool too_long;
        return parse_string(c, tmp, sizeof(tmp), &too_long);
    }
    if(ch >= '0' && ch <= '9') {
        uint32_t ignored;
        return parse_uint32(c, &ignored);
    }
    if(ch == 't') return parse_literal(c, "true");
    if(ch == 'f') return parse_literal(c, "false");
    if(ch == 'n') return parse_literal(c, "null");
    return fail(c, GoveeRegistryParseSyntax);
}

static bool parse_addr(const char* s, uint8_t addr[GOVEE_ADDR_LEN]) {
    if(strlen(s) != 17) return false;
    for(uint32_t i = 0; i < GOVEE_ADDR_LEN; i++) {
        int hi = hex_value((unsigned char)s[i * 3u]);
        int lo = hex_value((unsigned char)s[i * 3u + 1u]);
        if(hi < 0 || lo < 0) return false;
        if(i < GOVEE_ADDR_LEN - 1u && s[i * 3u + 2u] != ':') return false;
        addr[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

static bool parse_object(Cursor* c, GoveeRegistryLamp* lamp) {
    bool has_name = false, has_addr = false, has_type = false, has_ts = false, has_ok = false;
    char raw_name[64];
    raw_name[0] = '\0';

    if(!expect(c, '{')) return false;
    skip_ws(c);
    if(peek(c) == '}') {
        c->pos++;
        return fail(c, GoveeRegistryParseMissingField);
    }
    bool closed = false;
    /* bounded: each iteration consumes at least one key */
    while(c->pos < c->len) {
        char key[16];
        bool too_long;
        if(!parse_string(c, key, sizeof(key), &too_long)) return false;
        if(!expect(c, ':')) return false;
        size_t value_pos = c->pos;

        if(!too_long && strcmp(key, "name") == 0) {
            if(has_name) return fail(c, GoveeRegistryParseDuplicateKey);
            bool name_long;
            if(!parse_string(c, raw_name, sizeof(raw_name), &name_long)) return false;
            has_name = true;
        } else if(!too_long && strcmp(key, "address") == 0) {
            if(has_addr) return fail(c, GoveeRegistryParseDuplicateKey);
            char addr_str[20];
            bool addr_long;
            if(!parse_string(c, addr_str, sizeof(addr_str), &addr_long)) return false;
            if(addr_long || !parse_addr(addr_str, lamp->id.addr)) {
                c->pos = value_pos;
                return fail(c, GoveeRegistryParseBadValue);
            }
            has_addr = true;
        } else if(!too_long && strcmp(key, "addr_type") == 0) {
            if(has_type) return fail(c, GoveeRegistryParseDuplicateKey);
            uint32_t t;
            if(!parse_uint32(c, &t)) return false;
            if(t > GOVEE_ADDR_TYPE_MAX) {
                c->pos = value_pos;
                return fail(c, GoveeRegistryParseBadValue);
            }
            lamp->id.addr_type = (uint8_t)t;
            has_type = true;
        } else if(!too_long && strcmp(key, "last_seen_ts") == 0) {
            if(has_ts) return fail(c, GoveeRegistryParseDuplicateKey);
            if(!parse_uint32(c, &lamp->last_seen_ts)) return false;
            has_ts = true;
        } else if(!too_long && strcmp(key, "ok") == 0) {
            if(has_ok) return fail(c, GoveeRegistryParseDuplicateKey);
            uint32_t ok;
            if(!parse_uint32(c, &ok)) return false;
            if(ok > 1u) {
                c->pos = value_pos;
                return fail(c, GoveeRegistryParseBadValue);
            }
            lamp->last = ok ? GoveeLampLastOk : GoveeLampLastFailed;
            has_ok = true;
        } else {
            if(!skip_scalar(c)) return false;
        }

        skip_ws(c);
        int ch = peek(c);
        if(ch == ',') {
            c->pos++;
            continue;
        }
        if(ch == '}') {
            c->pos++;
            closed = true;
            break;
        }
        return fail(c, GoveeRegistryParseSyntax);
    }
    if(c->err != GoveeRegistryParseOk) return false;
    if(!closed) return fail(c, GoveeRegistryParseSyntax);
    if(!has_addr || !has_type) return fail(c, GoveeRegistryParseMissingField);
    govee_registry_sanitize_name(lamp->name, sizeof(lamp->name), raw_name);
    return true;
}

GoveeRegistryParseResult govee_registry_parse(
    GoveeRegistry* out,
    const char* text,
    size_t len,
    size_t* err_pos) {
    Cursor c = {.text = text, .len = text ? len : 0, .pos = 0, .err = GoveeRegistryParseOk};
    govee_registry_init(out);

    skip_ws(&c);
    if(c.pos >= c.len) {
        c.err = GoveeRegistryParseEmpty;
    } else if(expect(&c, '[')) {
        bool closed = false;
        skip_ws(&c);
        if(peek(&c) == ']') {
            c.pos++;
            closed = true;
        } else {
            while(c.err == GoveeRegistryParseOk && c.pos < c.len) {
                if(out->count >= GOVEE_MAX_LAMPS) {
                    fail(&c, GoveeRegistryParseTooMany);
                    break;
                }
                GoveeRegistryLamp lamp;
                memset(&lamp, 0, sizeof(lamp));
                size_t obj_pos = c.pos;
                if(!parse_object(&c, &lamp)) break;
                if(govee_registry_find(out, &lamp.id) != GOVEE_LAMP_NONE) {
                    c.pos = obj_pos;
                    fail(&c, GoveeRegistryParseDuplicateLamp);
                    break;
                }
                out->lamps[out->count++] = lamp;
                skip_ws(&c);
                int ch = peek(&c);
                if(ch == ',') {
                    c.pos++;
                    continue;
                }
                if(ch == ']') {
                    c.pos++;
                    closed = true;
                    break;
                }
                fail(&c, GoveeRegistryParseSyntax);
            }
        }
        if(c.err == GoveeRegistryParseOk && !closed) {
            fail(&c, GoveeRegistryParseSyntax); /* ran out of text inside the array */
        }
        if(c.err == GoveeRegistryParseOk) {
            skip_ws(&c);
            if(c.pos != c.len) fail(&c, GoveeRegistryParseSyntax); /* trailing bytes */
        }
    }

    if(c.err != GoveeRegistryParseOk) {
        govee_registry_init(out);
        if(err_pos) *err_pos = c.pos;
    } else if(err_pos) {
        *err_pos = 0;
    }
    return c.err;
}

/* ------------------------------------------------------------------------- */
/* Serialiser                                                                */
/* ------------------------------------------------------------------------- */

typedef struct {
    char* buf;
    size_t size;
    size_t len;
    bool truncated;
} Writer;

static void w_printf_result(Writer* w, int r) {
    if(r < 0) {
        w->truncated = true;
        return;
    }
    size_t room = w->size - w->len;
    if((size_t)r >= room) {
        w->truncated = true;
        w->len = w->size - 1u;
    } else {
        w->len += (size_t)r;
    }
}

bool govee_registry_serialize(const GoveeRegistry* reg, char* buf, size_t size, size_t* out_len) {
    if(size == 0) {
        if(out_len) *out_len = 0;
        return false;
    }
    Writer w = {.buf = buf, .size = size, .len = 0, .truncated = false};
    buf[0] = '\0';

    w_printf_result(&w, snprintf(w.buf + w.len, w.size - w.len, "[\n"));
    for(uint8_t i = 0; i < reg->count && i < GOVEE_MAX_LAMPS && !w.truncated; i++) {
        const GoveeRegistryLamp* lamp = &reg->lamps[i];
        char name[GOVEE_NAME_LEN];
        char addr[18];
        /* sanitise again so a caller that wrote the struct directly cannot break the file */
        govee_registry_sanitize_name(name, sizeof(name), lamp->name);
        govee_registry_format_addr(lamp->id.addr, addr);
        w_printf_result(
            &w,
            snprintf(
                w.buf + w.len,
                w.size - w.len,
                "  {\"name\": \"%s\", \"address\": \"%s\", \"addr_type\": %u, "
                "\"last_seen_ts\": %lu%s}%s\n",
                name,
                addr,
                (unsigned)lamp->id.addr_type,
                (unsigned long)lamp->last_seen_ts,
                lamp->last == GoveeLampLastOk     ? ", \"ok\": 1" :
                lamp->last == GoveeLampLastFailed ? ", \"ok\": 0" :
                                                    "",
                (i + 1u < reg->count) ? "," : ""));
    }
    if(!w.truncated) w_printf_result(&w, snprintf(w.buf + w.len, w.size - w.len, "]\n"));

    if(out_len) *out_len = w.len;
    return !w.truncated;
}

const char* govee_registry_parse_result_name(GoveeRegistryParseResult r) {
    switch(r) {
    case GoveeRegistryParseOk:
        return "ok";
    case GoveeRegistryParseEmpty:
        return "empty";
    case GoveeRegistryParseSyntax:
        return "syntax";
    case GoveeRegistryParseMissingField:
        return "missing_field";
    case GoveeRegistryParseBadValue:
        return "bad_value";
    case GoveeRegistryParseDuplicateKey:
        return "duplicate_key";
    case GoveeRegistryParseDuplicateLamp:
        return "duplicate_lamp";
    case GoveeRegistryParseTooMany:
        return "too_many";
    }
    return "unknown";
}

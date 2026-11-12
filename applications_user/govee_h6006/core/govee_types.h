/**
 * @file govee_types.h
 * Types shared by the hardware-independent Govee H6006 core
 * (govee_registry, govee_engine, govee_status).
 *
 * Portable C (gnu17): no Furi or firmware includes, no heap, no floating point.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum number of saved lamps. */
#define GOVEE_MAX_LAMPS (16u)

/** BLE address length in bytes. */
#define GOVEE_ADDR_LEN (6u)

/** Lamp name buffer size including the terminating NUL (same as the old bulb cache). */
#define GOVEE_NAME_LEN (32u)

/** Highest BLE peer address type accepted (HCI range 0..3; the HAL reports 0 or 1). */
#define GOVEE_ADDR_TYPE_MAX (3u)

/** Index value meaning "no lamp". */
#define GOVEE_LAMP_NONE (0xFFu)

/** Lamp identity: (address type, address). Address bytes in the order the HAL reports them. */
typedef struct {
    uint8_t addr_type;
    uint8_t addr[GOVEE_ADDR_LEN];
} GoveeLampId;

/** Result of a lamp's last completed attempt, kept in the lamp file across app runs. */
typedef enum {
    GoveeLampLastUnknown = 0, /**< never completed an attempt (or an old file without the field) */
    GoveeLampLastOk = 1, /**< the last completed attempt submitted everything */
    GoveeLampLastFailed = 2, /**< the last completed attempt failed */
} GoveeLampLast;

/** Colour mode of the desired state. */
typedef enum {
    GoveeModeWhite = 0, /**< colour temperature in kelvin */
    GoveeModeColour = 1, /**< RGB colour */
} GoveeMode;

/** Brightness range accepted by the engine and the remote interface. */
#define GOVEE_BRIGHTNESS_MIN (1u)
#define GOVEE_BRIGHTNESS_MAX (100u)

/** Colour temperature range verified on the H6006 (see govee_h6006.c). */
#define GOVEE_KELVIN_MIN (2700u)
#define GOVEE_KELVIN_MAX (6500u)

/** Desired light state. rgb is 0x00RRGGBB. */
typedef struct {
    bool power;
    uint8_t brightness; /**< 1..100 */
    uint8_t mode; /**< GoveeMode */
    uint16_t kelvin; /**< GOVEE_KELVIN_MIN..GOVEE_KELVIN_MAX */
    uint32_t rgb; /**< 0x000000..0xFFFFFF */
} GoveeDesired;

/** True when both identities are equal. */
static inline bool govee_lamp_id_equal(const GoveeLampId* a, const GoveeLampId* b) {
    if(a->addr_type != b->addr_type) return false;
    for(uint32_t i = 0; i < GOVEE_ADDR_LEN; i++) {
        if(a->addr[i] != b->addr[i]) return false;
    }
    return true;
}

/** Wrap-safe "a is at or after b" for uint32_t millisecond stamps (valid for gaps < 2^31 ms). */
static inline bool govee_time_reached(uint32_t t, uint32_t deadline) {
    return (int32_t)(t - deadline) >= 0;
}

#ifdef __cplusplus
}
#endif

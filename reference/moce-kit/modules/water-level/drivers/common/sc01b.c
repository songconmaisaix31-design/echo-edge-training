#include "sc01b.h"
#include <limits.h>
#include <string.h>
static void increment(uint32_t *value)
{
    if (*value != UINT32_MAX) {
        ++*value;
    }
}
static void accumulate(uint64_t *value, uint32_t elapsed)
{
    *value = UINT64_MAX - *value < elapsed ? UINT64_MAX : *value + elapsed;
}
static void invalidate(Sc01b *sensor)
{
    sensor->reading.state = SC01B_UNKNOWN;
    sensor->reading.raw_valid = false;
    sensor->reading.stable_ms = 0;
    sensor->candidate_valid = false;
}
BspStatus sc01b_init(Sc01b *sensor, const Sc01bConfig *config, uint32_t now_ms)
{
    if (!sensor || !config || (config->port >> 8) != BSP_GPIO ||
        config->startup_ms < SC01B_MIN_STARTUP_MS || config->startup_ms > INT32_MAX ||
        config->wet_confirm_ms > INT32_MAX || config->dry_confirm_ms > INT32_MAX ||
        !config->max_sample_gap_ms || config->max_sample_gap_ms > INT32_MAX) {
        return BSP_INVALID_ARG;
    }
    if (sensor->initialized) {
        return BSP_INVALID_STATE;
    }
    uint32_t missing = 0;
    BspStatus status = bsp_port_require(config->port, BSP_CAP_BLOCKING, &missing);
    if (status != BSP_OK) {
        return status;
    }
    Sc01bConfig copy = *config;
    memset(sensor, 0, sizeof(*sensor));
    sensor->config = copy;
    sensor->initialized = true;
    sensor->started_ms = now_ms;
    sensor->last_update_ms = now_ms;
    sensor->reading.last_status = BSP_NOT_READY;
    return BSP_OK;
}
BspStatus sc01b_update(Sc01b *sensor, uint32_t now_ms)
{
    if (!sensor) {
        return BSP_INVALID_ARG;
    }
    if (!sensor->initialized) {
        return BSP_INVALID_STATE;
    }
    Sc01bReading *r = &sensor->reading;
    uint32_t elapsed = now_ms - sensor->last_update_ms;
    sensor->last_update_ms = now_ms;
    r->events = 0;
    if (!sensor->ready) {
        if ((uint32_t)(now_ms - sensor->started_ms) < sensor->config.startup_ms) {
            return BSP_NOT_READY;
        }
        sensor->ready = true;
    }
    if (r->raw_valid && elapsed > sensor->config.max_sample_gap_ms) {
        invalidate(sensor);
        increment(&r->gaps);
        r->events |= SC01B_EVENT_SAMPLE_GAP;
    }
    bool high = false;
    BspStatus status = bsp_gpio_read(sensor->config.port, &high);
    r->last_status = status;
    if (status != BSP_OK) {
        invalidate(sensor);
        increment(&r->read_errors);
        r->events |= SC01B_EVENT_READ_ERROR;
        return status;
    }
    if (r->raw_valid && r->state != SC01B_UNKNOWN) {
        if (r->state == SC01B_WET) {
            accumulate(&r->observed_wet_ms, elapsed);
        } else {
            accumulate(&r->observed_dry_ms, elapsed);
        }
        r->stable_ms = UINT32_MAX - r->stable_ms < elapsed ?
                       UINT32_MAX : r->stable_ms + elapsed;
    }
    r->raw_valid = true;
    r->raw_high = high;
    r->sampled_at_ms = now_ms;
    increment(&r->samples);
    Sc01bState raw_state = high ? SC01B_DRY : SC01B_WET;
    if (raw_state == r->state) {
        sensor->candidate_valid = false;
    } else {
        if (!sensor->candidate_valid || sensor->candidate != raw_state) {
            sensor->candidate_valid = true;
            sensor->candidate = raw_state;
            sensor->candidate_since_ms = now_ms;
        }
        uint32_t confirm_ms = raw_state == SC01B_WET ?
                              sensor->config.wet_confirm_ms : sensor->config.dry_confirm_ms;
        if ((uint32_t)(now_ms - sensor->candidate_since_ms) >= confirm_ms) {
            if (r->state == SC01B_UNKNOWN) {
                r->events |= SC01B_EVENT_INITIAL;
            } else if (raw_state == SC01B_WET) {
                r->events |= SC01B_EVENT_WET;
                increment(&r->wet_entries);
            } else {
                r->events |= SC01B_EVENT_DRY;
                increment(&r->dry_entries);
            }
            r->state = raw_state;
            r->stable_ms = 0;
            sensor->candidate_valid = false;
        }
    }
    return BSP_OK;
}
BspStatus sc01b_get(const Sc01b *sensor, uint32_t now_ms, Sc01bReading *out)
{
    if (!sensor || !out) {
        return BSP_INVALID_ARG;
    }
    if (!sensor->initialized) {
        return BSP_INVALID_STATE;
    }
    *out = sensor->reading;
    if (!out->raw_valid ||
        (uint32_t)(now_ms - out->sampled_at_ms) > sensor->config.max_sample_gap_ms) {
        out->state = SC01B_UNKNOWN;
        out->raw_valid = false;
        out->stable_ms = 0;
        out->events = 0;
        return BSP_NOT_READY;
    }
    return out->state == SC01B_UNKNOWN ? BSP_NOT_READY : BSP_OK;
}
void sc01b_deinit(Sc01b *sensor)
{
    if (sensor) {
        memset(sensor, 0, sizeof(*sensor));
    }
}
const char *sc01b_state_name(Sc01bState state)
{
    switch (state) {
    case SC01B_DRY:
        return "dry";
    case SC01B_WET:
        return "wet";
    default:
        return "unknown";
    }
}

void water_alarm_update(WaterAlarm *alarm, const Sc01bReading *reading,
                        uint32_t wet_ms, uint32_t dry_ms)
{
    if (!alarm || !reading) {
        return;
    }
    alarm->valid = reading->raw_valid && reading->state != SC01B_UNKNOWN;
    alarm->wet_active = alarm->valid && reading->state == SC01B_WET &&
                        reading->stable_ms >= wet_ms;
    alarm->dry_active = alarm->valid && reading->state == SC01B_DRY &&
                        reading->stable_ms >= dry_ms;
    alarm->wet_latched = alarm->wet_latched || alarm->wet_active;
    alarm->dry_latched = alarm->dry_latched || alarm->dry_active;
}
void water_alarm_acknowledge(WaterAlarm *alarm)
{
    if (!alarm || !alarm->valid) {
        return;
    }
    if (!alarm->wet_active) {
        alarm->wet_latched = false;
    }
    if (!alarm->dry_active) {
        alarm->dry_latched = false;
    }
}

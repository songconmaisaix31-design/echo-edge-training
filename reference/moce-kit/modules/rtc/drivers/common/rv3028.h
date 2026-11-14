#ifndef RV3028_H
#define RV3028_H
#include "bsp_i2c.h"

#define RV3028_ADDRESS 0x52u
#define RV3028_STATUS_POR 0x01u
#define RV3028_STATUS_BACKUP 0x20u

/* RV-3028-C7, Application Manual Rev1.4. BSP 0.9.0 only; no Board dependency.
 * Caller owns an initialized hardware I2C port, BLOCKING|RESTART, 100/400kHz,
 * and exclusive task-context access to this device. Not ISR/concurrent safe.
 * Zero-initialize instance. Native transfer timeout must be 1..100ms; a calendar
 * transaction must complete within the device's 950ms window. Native BSP timeout
 * is not a guaranteed whole-operation deadline under scheduler starvation.
 * Init preserves time, selects 24h, disables interrupts/CLKOUT/trickle charging,
 * enables LSM backup and disables automatic EEPROM refresh (EERD=1).
 * Only RAM mirrors are modified: no EEPROM writes. Battery retains settings;
 * after total power loss init is required again. No other owner may refresh EEPROM.
 * Protected devices are rejected. Failed init can leave partial RAM changes;
 * failed set can leave partial calendar writes. No rollback or automatic retry.
 * Alarm/timer/Unix counter are outside this driver's API; calendar set does not
 * synchronize the separate Unix counter. Deinit releases no BSP port and leaves
 * the RTC ticking with its safe RAM configuration. */
typedef struct {
    BspPort port;
    uint8_t address;
    uint32_t timeout_ms;
} Rv3028Config;

typedef struct {
    uint16_t year; /* 2000..2099 */
    uint8_t month, day, weekday; /* month 1..12, day 1..31, weekday 0..6 */
    uint8_t hour, minute, second; /* 24-hour format; weekday mapping is caller-owned */
} Rv3028DateTime;

typedef struct {
    Rv3028DateTime time;
    uint8_t status;
    bool calendar_valid; /* legal BCD/date */
    bool time_valid;     /* legal date AND no power-on-reset flag */
} Rv3028Reading;

typedef struct {
    Rv3028Config config;
    uint8_t device_id; /* informational HID/VID, not a fixed WHO_AM_I signature */
    bool initialized;
} Rv3028;

BspStatus rv3028_init(Rv3028 *device, const Rv3028Config *config);
BspStatus rv3028_read(Rv3028 *device, Rv3028Reading *reading);
/* Explicit caller-supplied time only. Clears PORF after successful calendar write.
 * Error leaves instance uninitialized; re-init/re-read before proceeding. */
BspStatus rv3028_set_datetime(Rv3028 *device, const Rv3028DateTime *time);
void rv3028_deinit(Rv3028 *device);
#endif

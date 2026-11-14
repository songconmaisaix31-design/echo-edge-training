#ifndef TM1650_H
#define TM1650_H
#include "bsp_i2c.h"
#define TM1650_ADDRESS 0x24u
/* Module SCH_matrix keyboard 2026-09-14: ROW1..4=DIG1..4,
 * COL1..4=KI1..4. SW number=(DIG-1)*4+KI.
 * Requires initialized public BSP0.9.0 blocking I2C, 100kHz, task context,
 * exclusive ownership of TM1650 addresses 0x24 and 0x34..0x37.
 * Hardware sends 0x48 on write, 0x49 on read; never use 0x49 as 7-bit address.
 * Zero-initialize instance. Init blanks display RAM and enables scanning.
 * No physical LED populated. Single-key and KI1+KI2 same-DIG combination only;
 * chip priority hides some simultaneous presses. Not full rollover.
 * Successful samples only are committed; error does not mean key release.
 * Transport errors latch instance invalid until explicit reinitialization.
 * Deinit clears software only, leaves scanning active and does not free port.
 * No allocation, ISR or concurrent access; caller owns BSP resource lifetime.
 * powerup_ms is project-selected margin, not a proven 3.3V worst-case timing. */
typedef struct {
    BspPort port;
    uint8_t address;
    uint32_t timeout_ms;
    uint32_t powerup_ms;
} Tm1650Config;
typedef struct {
    uint8_t raw;
    bool pressed;
    bool recognized;
    bool combination;
    uint8_t row, column; /* module net numbers 1..4; column=0 for combo */
    uint16_t keys; /* bit0=SW1 ... bit15=SW16; zero if released/unknown */
} Tm1650Reading;
typedef struct {
    Tm1650Config config;
    bool initialized;
} Tm1650;
BspStatus tm1650_init(Tm1650 *device, const Tm1650Config *config);
BspStatus tm1650_read(Tm1650 *device, Tm1650Reading *reading);
void tm1650_deinit(Tm1650 *device);
#endif

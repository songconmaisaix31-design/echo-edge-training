#ifndef MOCE_MPU6050_H
#define MOCE_MPU6050_H
#include "bsp_i2c.h"
#define MPU6050_ADDRESS_LOW 0x68u
#define MPU6050_ADDRESS_HIGH 0x69u
#define MPU6050_WHO_AM_I 0x68u
typedef struct
{
    BspPort port;
    uint8_t address; /* Unshifted 7-bit address selected by physical AD0. */
    uint32_t timeout_ms;
    uint32_t reset_wait_ms;
    uint32_t startup_wait_ms;
} Mpu6050Config;
typedef struct
{
    int16_t accel[3];
    int16_t gyro[3];
    int16_t temperature;
} Mpu6050Raw;
typedef struct
{
    int32_t accel_mg[3];
    int32_t gyro_mdps[3];
    int32_t temperature_mc; /* Die temperature estimate, not ambient calibration. */
} Mpu6050Sample;
typedef struct
{
    float pitch_deg; /* atan2(-ax, sqrt(ay^2 + az^2)), range [-90,90]. */
    float roll_deg;  /* atan2(ay,az), range [-180,180]. */
} Mpu6050Tilt;
typedef struct
{
    Mpu6050Config config;
    bool configured;
    bool identity_verified;
    bool ready;
    bool faulted;
    uint8_t observed_identity;
    BspError last_error;
    BspStatus cleanup_status;
} Mpu6050;
/* Zero-initialize. Application owns a synchronous I2C port with repeated START
 * and serializes device/BSP calls. Config copied; no retained user buffers,
 * Board/HAL dependency, hidden worker, scan, retry, recover, FIFO or DMP.
 * init checks WHO_AM_I before writes, resets, waits configured finite periods,
 * sets fixed +/-2g, +/-250dps, DLPF=3 and divider=9 (nominal 100Hz).
 * Registers are read back. Device reset/configuration requires exclusive module
 * ownership; WHO_AM_I is family identity, not proof of genuine silicon.
 * read_raw returns BUSY if DATA_RDY is absent; output remains unchanged on every
 * non-OK return. Other I/O failures latch fault and preserve original error.
 * Any init failure after binding (including BUSY) latches fault: partial reset/
 * configuration may already have happened. Capability failure before binding does
 * not latch or write. Local validation/readback/wait errors never borrow stale
 * native I2C errors. A new clean lifecycle verifies identity again before writes.
 * deinit always clears readiness and attempts sleep only after identity matched;
 * a failed sleep latches fault. It does not release BSP or
 * clear a latched fault. Cleanup errors do not replace the original failure.
 * No hard whole-operation deadline is promised by native BSP timeout arguments.
 */
BspStatus mpu6050_init(Mpu6050 *device, const Mpu6050Config *config);
BspStatus mpu6050_read_raw(Mpu6050 *device, Mpu6050Raw *out);
BspStatus mpu6050_convert(const Mpu6050Raw *raw, Mpu6050Sample *out);
/* Pure gravity-based tilt, no bus I/O or gyro integration; uses C libm.
 * Reference: ax=ay=0, az=+1g gives pitch=roll=0; chip axes, no mounting remap.
 * Suitable for static/slow tilt only; norm gate 0.8..1.2g cannot detect all
 * linear acceleration. Returns NOT_READY outside gate or yz projection <0.01g
 * (roll singular near vertical pitch). Non-OK leaves *out unchanged. No yaw.
 */
BspStatus mpu6050_calculate_tilt(const Mpu6050Raw *raw, Mpu6050Tilt *out);
BspStatus mpu6050_deinit(Mpu6050 *device);
#endif

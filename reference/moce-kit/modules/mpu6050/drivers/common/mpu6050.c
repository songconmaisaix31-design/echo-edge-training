#include "mpu6050.h"
#include <limits.h>
#include <math.h>
#define REG_SAMPLE_DIV 0x19u
#define REG_CONFIG 0x1au
#define REG_GYRO_CONFIG 0x1bu
#define REG_ACCEL_CONFIG 0x1cu
#define REG_FIFO_ENABLE 0x23u
#define REG_INT_ENABLE 0x38u
#define REG_INT_STATUS 0x3au
#define REG_DATA_START 0x3bu
#define REG_USER_CONTROL 0x6au
#define REG_POWER1 0x6bu
#define REG_POWER2 0x6cu
#define REG_IDENTITY 0x75u
#define POWER_RESET 0x80u
#define POWER_PLL_X 0x01u
#define POWER_SLEEP 0x40u
#define DATA_READY 0x01u
#define DATA_BYTES 14u
#define SAMPLE_DIVIDER 9u
#define FILTER_CONFIG 3u
#define ACCEL_LSB_PER_G 16384
#define GYRO_LSB_PER_DPS 131
#define TEMP_LSB_PER_C 340
#define TEMP_OFFSET_MC 36530
static BspStatus fail(Mpu6050 *device, BspStatus status, bool current_i2c_error)
{
    device->faulted = true;
    device->ready = false;
    device->last_error = (BspError){.code = status};
    BspError native = {0};
    if (current_i2c_error && status != BSP_BUSY &&
        bsp_port_error(device->config.port, &native) == BSP_OK && native.code == status)
    {
        device->last_error = native;
    }
    return status;
}
static BspStatus read_register(Mpu6050 *device, uint8_t reg, uint8_t *out)
{
    return bsp_i2c_mem_read(device->config.port, device->config.address, reg, 1,
                            out, 1, device->config.timeout_ms);
}
static BspStatus write_register(Mpu6050 *device, uint8_t reg, uint8_t value)
{
    return bsp_i2c_mem_write(device->config.port, device->config.address, reg, 1,
                             &value, 1, device->config.timeout_ms);
}
BspStatus mpu6050_init(Mpu6050 *device, const Mpu6050Config *config)
{
    if (!device || !config || (config->port >> 8) != BSP_I2C ||
        (config->address != MPU6050_ADDRESS_LOW && config->address != MPU6050_ADDRESS_HIGH) ||
        !config->timeout_ms || config->timeout_ms > INT32_MAX ||
        !config->reset_wait_ms || config->reset_wait_ms > INT32_MAX ||
        !config->startup_wait_ms || config->startup_wait_ms > INT32_MAX)
    {
        return BSP_INVALID_ARG;
    }
    if (device->configured || device->faulted)
    {
        return BSP_INVALID_STATE;
    }
    uint32_t missing = 0;
    BspStatus status = bsp_port_require(config->port, BSP_CAP_BLOCKING | BSP_CAP_RESTART, &missing);
    if (status != BSP_OK)
    {
        return status;
    }
    device->config = *config;
    device->configured = true;
    device->identity_verified = false;
    device->observed_identity = 0;
    device->ready = false;
    device->last_error = (BspError){0};
    device->cleanup_status = BSP_OK;
    uint8_t value = 0;
    status = read_register(device, REG_IDENTITY, &value);
    if (status != BSP_OK)
    {
        return fail(device, status, true);
    }
    device->observed_identity = value;
    if (value != MPU6050_WHO_AM_I)
    {
        return fail(device, BSP_UNSUPPORTED, false);
    }
    device->identity_verified = true;
    bool current_i2c_error = true;
    status = write_register(device, REG_POWER1, POWER_RESET);
    if (status == BSP_OK)
    {
        current_i2c_error = false;
        status = bsp_delay_ms(config->reset_wait_ms);
    }
    if (status == BSP_OK)
    {
        current_i2c_error = true;
        status = read_register(device, REG_POWER1, &value);
        if (status == BSP_OK && (value & POWER_RESET))
        {
            current_i2c_error = false;
            status = BSP_NOT_READY;
        }
    }
    const uint8_t registers[] = {REG_POWER1, REG_POWER2, REG_USER_CONTROL, REG_FIFO_ENABLE,
                                 REG_INT_ENABLE, REG_CONFIG, REG_SAMPLE_DIV, REG_GYRO_CONFIG, REG_ACCEL_CONFIG};
    const uint8_t values[] = {POWER_PLL_X, 0, 0, 0, DATA_READY, FILTER_CONFIG, SAMPLE_DIVIDER, 0, 0};
    for (size_t i = 0; status == BSP_OK && i < sizeof(registers); ++i)
    {
        current_i2c_error = true;
        status = write_register(device, registers[i], values[i]);
        if (status == BSP_OK)
        {
            status = read_register(device, registers[i], &value);
            if (status == BSP_OK && value != values[i])
            {
                current_i2c_error = false;
                status = BSP_IO;
            }
        }
    }
    if (status == BSP_OK)
    {
        current_i2c_error = false;
        status = bsp_delay_ms(config->startup_wait_ms);
    }
    if (status != BSP_OK)
    {
        return fail(device, status, current_i2c_error);
    }
    device->ready = true;
    return BSP_OK;
}
static int16_t signed_be(const uint8_t *bytes)
{
    uint16_t value = (uint16_t)(((uint16_t)bytes[0] << 8) | bytes[1]);
    int32_t signed_value = value >= 0x8000u ? (int32_t)value - 65536 : (int32_t)value;
    return (int16_t)signed_value;
}
BspStatus mpu6050_read_raw(Mpu6050 *device, Mpu6050Raw *out)
{
    if (!device || !out)
    {
        return BSP_INVALID_ARG;
    }
    if (device->faulted)
    {
        return BSP_INVALID_STATE;
    }
    if (!device->ready)
    {
        return BSP_NOT_READY;
    }
    uint8_t status_byte = 0;
    BspStatus status = read_register(device, REG_INT_STATUS, &status_byte);
    if (status != BSP_OK)
    {
        return status == BSP_BUSY ? BSP_BUSY : fail(device, status, true);
    }
    if (!(status_byte & DATA_READY))
    {
        return BSP_BUSY;
    }
    uint8_t bytes[DATA_BYTES] = {0};
    status = bsp_i2c_mem_read(device->config.port, device->config.address, REG_DATA_START, 1,
                              bytes, sizeof(bytes), device->config.timeout_ms);
    if (status != BSP_OK)
    {
        return status == BSP_BUSY ? BSP_BUSY : fail(device, status, true);
    }
    Mpu6050Raw sample = {0};
    for (size_t i = 0; i < 3; ++i)
    {
        sample.accel[i] = signed_be(&bytes[i * 2u]);
        sample.gyro[i] = signed_be(&bytes[8u + i * 2u]);
    }
    sample.temperature = signed_be(&bytes[6]);
    *out = sample;
    return BSP_OK;
}
BspStatus mpu6050_convert(const Mpu6050Raw *raw, Mpu6050Sample *out)
{
    if (!raw || !out)
    {
        return BSP_INVALID_ARG;
    }
    Mpu6050Sample sample = {0};
    for (size_t i = 0; i < 3; ++i)
    {
        sample.accel_mg[i] = ((int32_t)raw->accel[i] * 1000) / ACCEL_LSB_PER_G;
        sample.gyro_mdps[i] = ((int32_t)raw->gyro[i] * 1000) / GYRO_LSB_PER_DPS;
    }
    sample.temperature_mc = TEMP_OFFSET_MC + ((int32_t)raw->temperature * 1000) / TEMP_LSB_PER_C;
    *out = sample;
    return BSP_OK;
}
BspStatus mpu6050_calculate_tilt(const Mpu6050Raw *raw, Mpu6050Tilt *out)
{
    if (!raw || !out)
    {
        return BSP_INVALID_ARG;
    }
    const float ax = (float)raw->accel[0] / ACCEL_LSB_PER_G;
    const float ay = (float)raw->accel[1] / ACCEL_LSB_PER_G;
    const float az = (float)raw->accel[2] / ACCEL_LSB_PER_G;
    const float norm_squared = ax * ax + ay * ay + az * az;
    const float yz_squared = ay * ay + az * az;
    /* 0.8^2 <= |a|^2 <= 1.2^2; reject ill-conditioned roll near pitch +/-90. */
    if (norm_squared < 0.64f || norm_squared > 1.44f || yz_squared < 0.0001f)
    {
        return BSP_NOT_READY;
    }
    const float radians_to_degrees = 57.29577951308232f;
    const Mpu6050Tilt tilt = {
        .pitch_deg = atan2f(-ax, sqrtf(yz_squared)) * radians_to_degrees,
        .roll_deg = atan2f(ay, az) * radians_to_degrees,
    };
    *out = tilt;
    return BSP_OK;
}
BspStatus mpu6050_deinit(Mpu6050 *device)
{
    if (!device)
    {
        return BSP_INVALID_ARG;
    }
    if (!device->configured)
    {
        return BSP_NOT_READY;
    }
    BspStatus status = device->identity_verified ? write_register(device, REG_POWER1, POWER_SLEEP) : BSP_OK;
    device->cleanup_status = status;
    device->ready = false;
    if (status == BSP_OK)
    {
        device->configured = false;
        device->identity_verified = false;
    }
    else if (!device->faulted)
    {
        return fail(device, status, true);
    }
    return status;
}



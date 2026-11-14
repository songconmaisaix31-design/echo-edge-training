#ifndef ECW_I2C_TIMING_H
#define ECW_I2C_TIMING_H
#include <stdint.h>
#include "ecw_clock_profile.h"
#include "i2c_reg_struct.h"

/* B02P03 compatibility model: nominal period=(SCLHI+5)*(TPM+1)
 * in units of the SDK's 20 MHz timing reference, NOT measured PCLK/SCL.
 * Round period upward and use the smallest multiplier fitting 9-bit SCLHI.
 * Prefer ratio0; only if unrepresentable use explicit bit13 ratio1.
 * Never let the 9-bit SCLHI field spill into bit13.
 * Public configuration/ABI is unchanged; see I2C_RATE_COMPAT_20261019.md. */
typedef struct
{
    uint32_t setup, tpm;
} EcwI2cTiming;

static inline EcwI2cTiming ecw_i2c_timing(uint32_t rate_hz)
{
    EcwI2cTiming result = {0u, 0u};
    if (rate_hz < 1000u || rate_hz > 400000u)
        return result;
    for (uint32_t multiplier = 1u; multiplier <= 32u; ++multiplier)
    {
        uint32_t divisor = rate_hz * multiplier;
        uint32_t period = (ECW_I2C_INPUT_HZ + divisor - 1u) / divisor;
        if (period >= 12u && period <= 516u)
        {
            result.setup = I2C_SETUP_EN | I2C_SETUP_MASTER |
                I2C_SETUP_T_SUDAT(4u) | I2C_SETUP_T_HDDAT(6u) |
                I2C_SETUP_T_SCLHI(period - 5u);
            result.tpm = multiplier - 1u;
            return result;
        }
    }
    /* Ratio1 fallback only after all ratio0 multipliers fail.
     * Legacy-calibrated nominal period=(3*count+10)*m/2.
     * Reference IP bit13 extends LOW, with earlier ECW single-bit evidence.
     * This is not the exact ECW vendor formula or a measured clock promise. */
    for (uint32_t multiplier = 1u; multiplier <= 32u; ++multiplier)
    {
        uint32_t divisor = rate_hz * multiplier;
        uint32_t units = (2u * ECW_I2C_INPUT_HZ + divisor - 1u) / divisor;
        uint32_t count = (units - 10u + 2u) / 3u;
        if (count >= 7u && count <= 511u)
        {
            result.setup = I2C_SETUP_EN | I2C_SETUP_MASTER |
                I2C_SETUP_T_SUDAT(4u) | I2C_SETUP_T_HDDAT(6u) |
                I2C_SETUP_T_SCLHI(count) | 0x2000u;
            result.tpm = multiplier - 1u;
            return result;
        }
    }
    return result;
}
static inline uint32_t ecw_i2c_timing_setup(uint32_t rate_hz)
{
    return ecw_i2c_timing(rate_hz).setup;
}
#endif

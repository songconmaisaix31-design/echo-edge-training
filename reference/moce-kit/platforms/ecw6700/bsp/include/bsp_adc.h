/* MOCE BSP 0.9.0 public contract; operation support and validation are platform-specific. */
#ifndef BSP_ADC_H
#define BSP_ADC_H
#include "bsp_core.h"
#ifdef __cplusplus
extern "C"
{
#endif
    /* ADC 0.7: fixed Board channel index; no arbitrary pin/channel mapping.
 * RAW is the converter code, never reconstructed from voltage or rescaled.
 * MICROVOLTS is voltage at the MCU ADC input, NOT before a Board divider.
 * Required bits match the active configuration; read NEVER changes resolution.
 * CONVERSION_WAIT passes a finite conversion wait budget to the adapter; setup
 * and safe cleanup are separate. It is NOT a whole-call deadline.
 * SDK_NATIVE explicitly accepts SDK-managed waiting, possibly unbounded;
 * timeout_ms must be zero in that mode. Zero alone never selects this mode.
 * No automatic retries. Options/result are not retained after return.
 * On any non-OK return, the complete result remains unchanged.
 */
    typedef enum
    {
        BSP_ADC_RAW = 0,
        BSP_ADC_MICROVOLTS = 1
    } BspAdcFormat;
    typedef enum
    {
        BSP_ADC_CONVERSION_WAIT = 0,
        BSP_ADC_SDK_NATIVE = 1
    } BspAdcWait;
    typedef struct
    {
        BspAdcFormat format;
        BspAdcWait wait;
        uint32_t timeout_ms;
        uint8_t required_resolution_bits; /* 0: no requirement; otherwise 1..32. */
    } BspAdcReadOptions;
    typedef struct
    {
        BspAdcFormat format;
        uint8_t resolution_bits; /* RAW code width; 0 for voltage. Not accuracy. */
        bool raw_signed;         /* RAW: numeric signed code; never packed two's complement. */
        int64_t value;           /* RAW code or microvolts; tagged by format. */
    } BspAdcResult;
    typedef struct
    {
        uint32_t formats;        /* 1u << BspAdcFormat */
        uint32_t wait_modes;     /* 1u << BspAdcWait; applies to all advertised formats. */
        uint8_t resolution_bits; /* Active/fixed code width; 0 if RAW unavailable. */
        bool raw_signed;
    } BspAdcCapabilities;
    /* Read-only query: no conversion, allocation or pin initialization.
 * Reports the currently bound adapter subset, not all silicon capabilities.
 * Absent Board ports/channels are INVALID_ARG; present unadapted operations
 * return zero formats. Query success does not imply port initialization. */
    BspStatus bsp_adc_capabilities(BspPort id, size_t channel_index, BspAdcCapabilities *out);
    BspStatus bsp_adc_read(BspPort id, size_t channel_index, const BspAdcReadOptions *options,
                           BspAdcResult *out);
#ifdef __cplusplus
}
#endif
#endif

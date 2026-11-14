#ifndef BSP_IO_CONFIG_H
#define BSP_IO_CONFIG_H
#include "bsp_core.h"
#ifdef __cplusplus
extern "C"
{
#endif
    /* Zero means UNSET everywhere, never NONE, LOW, or INPUT. */
    typedef enum
    {
        BSP_IO_UNSET = 0,
        BSP_IO_INPUT = 1,
        BSP_IO_OUTPUT_PP = 2,
        BSP_IO_OUTPUT_OD = 3,
        BSP_IO_INPUT_OUTPUT = 4,
        BSP_IO_INPUT_OUTPUT_OD = 5
    } BspIoMode;
    typedef enum
    {
        BSP_IO_PULL_UNSET = 0,
        BSP_IO_PULL_NONE = 1,
        BSP_IO_PULL_UP = 2,
        BSP_IO_PULL_DOWN = 3
    } BspIoPull;
    typedef enum
    {
        BSP_IO_LEVEL_UNSET = 0,
        BSP_IO_LEVEL_NA = 1,
        BSP_IO_LOW = 2,
        BSP_IO_HIGH = 3
    } BspIoLevel;
    typedef enum
    {
        BSP_IO_SPEED_UNSET = 0,
        BSP_IO_SPEED_NA = 1,
        BSP_IO_SPEED_NATIVE = 2
    } BspIoSpeed;
#define BSP_IO_ALLOW(value) (1u << (unsigned)(value))
#define BSP_IO_ALL_PULLS                                                                           \
    (BSP_IO_ALLOW(BSP_IO_PULL_NONE) | BSP_IO_ALLOW(BSP_IO_PULL_UP) | BSP_IO_ALLOW(BSP_IO_PULL_DOWN))
    typedef struct
    {
        BspIoMode mode;
        BspIoPull pull;
        BspIoLevel initial;
        BspIoSpeed speed;
        uint32_t native_speed; /* Only meaningful for SPEED_NATIVE; no cross-vendor equivalence. */
    } BspGpioConfig;
    /* Board capabilities. source_pin indexes the source's physical pin list.
 * ADC->DI IDs are Board-defined aliases, not arbitrary application pin mappings. */
    typedef struct
    {
        BspPort id, source;
        size_t source_pin;
        uint32_t modes, pulls;
        const char *name;
        uint32_t
            native_speeds; /* STM GPIO output: bit(native HAL speed); zero grants no output speed. */
    } BspIoResource;
    typedef struct
    {
        BspPort port;
        BspGpioConfig gpio; /* Explicit for GPIO; all zero for other protocols. */
        uint32_t adc_mask;  /* ADC only: nonzero subset of original Board channel indices. */
    } BspIoChoice;
    /* Pure validation, no hardware effects. Native adapters additionally validate speed/capability. */
    BspStatus bsp_io_gpio_validate(const BspIoResource *resource, const BspGpioConfig *config);
    bool bsp_io_gpio_unset(const BspGpioConfig *config);
    /* Project ADC indices are compact, ascending by original Board index. */
    BspStatus bsp_io_source_channel(uint32_t mask, size_t project_index, size_t *source_index);
#ifdef __cplusplus
}
#endif
#endif

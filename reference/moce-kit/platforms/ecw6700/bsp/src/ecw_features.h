#ifndef ECW_FEATURES_H
#define ECW_FEATURES_H
/* Private build choices. Legacy integrations retain the complete backend.
 * MOCE-SYNC: optional dependencies | ../BACKENDS.md */
#ifndef BSP_ECW_ENABLE_I2S
#define BSP_ECW_ENABLE_I2S 1
#endif
#ifndef BSP_ECW_ENABLE_DMA
#define BSP_ECW_ENABLE_DMA 1
#endif
#if BSP_ECW_ENABLE_I2S && !BSP_ECW_ENABLE_DMA
#error "ECW I2S requires BSP_ECW_ENABLE_DMA=1"
#endif
#endif

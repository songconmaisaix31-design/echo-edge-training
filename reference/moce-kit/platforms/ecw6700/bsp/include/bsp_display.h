/* MOCE BSP 0.9.0 public contract; operation support and validation are platform-specific. */
#ifndef BSP_DISPLAY_H
#define BSP_DISPLAY_H
#include "bsp_core.h"
#ifdef __cplusplus
extern "C"
{
#endif
    typedef struct
    {
        bool data_mode;
        const uint8_t *data;
        size_t length;
        uint8_t *receive;
    } BspDisplaySegment;
    typedef struct
    {
        BspPort spi, cs;
        bool cs_active_high;
    } BspSpiDevice;
    BspStatus bsp_spi_device_transfer_async(const BspSpiDevice *device, const uint8_t *tx,
                                            uint8_t *rx, size_t n, BspRequest *request);
    typedef struct
    {
        BspPort spi, cs, dc;
        bool cs_active_high;
    } BspDisplayBinding;
    /* Copy <=8 segment descriptors, borrow their data; hold SPI/CS/DC until final
 * successful line completion or verified recovery. No device commands invented. */
    BspStatus bsp_display_submit(const BspDisplayBinding *binding,
                                 const BspDisplaySegment *segments, size_t count,
                                 BspRequest *request);
#ifdef __cplusplus
}
#endif
#endif

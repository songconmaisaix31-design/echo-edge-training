/* SDK-private register dependency stays inside the BSP. It is hash-pinned by
 * integration/check_sdk.py; the application never includes this header.
 * MOCE-SYNC: fixed SDK selection | ../integration/SDK_COMPATIBILITY.md */
#ifndef ECW_SPI_REGISTERS_H
#define ECW_SPI_REGISTERS_H
#include "ecw_sdk_version.h"
#include "spi.h"
#if BSP_ECW_SDK_B02P12
#include "spi_internal.h"
#endif
#endif

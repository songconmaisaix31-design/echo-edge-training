/* MOCE-SYNC: fixed SDK selection | ../integration/SDK_COMPATIBILITY.md */
#ifndef ECW_SDK_VERSION_H
#define ECW_SDK_VERSION_H
#ifndef BSP_ECW_SDK_B02P12
#define BSP_ECW_SDK_B02P12 1
#endif
#if BSP_ECW_SDK_B02P12 != 0 && BSP_ECW_SDK_B02P12 != 1
#error "BSP_ECW_SDK_B02P12 must be 0 or 1; select the matching reviewed SDK"
#endif
#endif

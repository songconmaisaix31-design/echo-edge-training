#ifndef ZW83080_H
#define ZW83080_H
#include "bsp_uart.h"
#define ZW83080_DEFAULT_ADDRESS UINT32_C(0xffffffff)
/* enroll_times: 0 uses the module's reported value; nonzero selects the
 * PS_AutoEnroll command count without rewriting module configuration. */
typedef struct { BspPort port; uint32_t address, password, timeout_ms; uint8_t enroll_times; } Zw83080Config;
typedef struct { uint16_t enroll_times, template_bytes, capacity, security, packet_code, baud_factor; uint32_t address; } Zw83080Parameters;
typedef struct { uint8_t confirmation, stage, index; } Zw83080Progress;
typedef void (*Zw83080ProgressCallback)(const Zw83080Progress *progress, void *context);
typedef struct {
    Zw83080Config config;
    Zw83080Parameters parameters;
    bool initialized, receiver_owned, fault;
    uint8_t confirmation; /* FF until a validated reply; module status distinct from BSP status */
} Zw83080;
/* Zero-initialize; one task owns each instance and UART. Caller owns BSP port.
 * BSP_OK means complete valid exchange; inspect confirmation for module outcome.
 * Init returns BSP_IO on password rejection (confirmation preserved). */
BspStatus zw83080_init(Zw83080 *device,const Zw83080Config *config);
BspStatus zw83080_deinit(Zw83080 *device);
BspStatus zw83080_read_count(Zw83080 *device,uint16_t *count);
BspStatus zw83080_capture(Zw83080 *device);
BspStatus zw83080_generate(Zw83080 *device,uint8_t buffer);
BspStatus zw83080_search(Zw83080 *device,uint16_t *id,uint16_t *score);
/* Persistent write only on explicit call. No overwrite, no duplicate registration.
 * Progress callback runs in caller task; must not re-enter driver or block.
 * On transport timeout, stop issuing commands and reset module before re-init. */
BspStatus zw83080_enroll(Zw83080 *device,uint16_t id,uint32_t timeout_ms,
                      Zw83080ProgressCallback callback,void *context);
/* Explicit destructive API, not called by shipped example. */
BspStatus zw83080_delete_one(Zw83080 *device,uint16_t id);
#endif

#ifndef MOCE_SYN6288E_H
#define MOCE_SYN6288E_H
#include "bsp_uart.h"
#ifdef __cplusplus
extern "C" {
#endif

#define SYN6288E_MAX_TEXT 200u
#define SYN6288E_REPLY_ACCEPTED 0x41u
#define SYN6288E_REPLY_REJECTED 0x45u
#define SYN6288E_REPLY_IDLE 0x4fu
typedef struct
{
    BspPort port;
    uint32_t timeout_ms;
} Syn6288eConfig;
typedef struct
{
    Syn6288eConfig config;
    bool initialized;
    bool has_sent;
    uint32_t last_send_ms;
} Syn6288e;
typedef enum
{
    SYN6288E_STOP = 0x02,
    SYN6288E_PAUSE = 0x03,
    SYN6288E_RESUME = 0x04,
    SYN6288E_QUERY = 0x21
} Syn6288eCommand;
/* Zero-initialize instance. Single task owner; caller initializes UART 9600 8N1.
 * Scalars are copied. Text is borrowed only for the blocking call. Encoding is
 * GBK, not UTF-8. Success means UART wire completion, NOT spoken acceptance.
 * No automatic resend; a failed/partial frame also starts the interframe guard.
 * Module commands preserve >=9 ms between frames (datasheet requires >8 ms).
 * read_reply returns one raw protocol byte; unknown bytes are not success ACKs. */
BspStatus syn6288e_init(Syn6288e *device, const Syn6288eConfig *config);
BspStatus syn6288e_speak_gbk(Syn6288e *device, const uint8_t *text, size_t length);
BspStatus syn6288e_command(Syn6288e *device, Syn6288eCommand command);
BspStatus syn6288e_read_reply(Syn6288e *device, uint8_t *reply);
/* Local lifecycle only: no STOP command and no UART deinit. Caller owns port
 * cleanup and must wait >=9 ms before sending after a deinit/reinit cycle.
 * A read timeout may fault the BSP port; do not blindly repeat reads or resend.
 * Recover/reinitialize via the platform contract outside this driver.
 * Text length is bytes, not characters; no UTF-8 conversion is provided.
 * The 200-byte limit is a driver subset, not a claim about the chip maximum. */
BspStatus syn6288e_deinit(Syn6288e *device);
#ifdef __cplusplus
}
#endif
#endif

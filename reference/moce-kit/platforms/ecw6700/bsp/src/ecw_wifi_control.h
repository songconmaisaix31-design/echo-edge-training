#ifndef ECW_WIFI_CONTROL_H
#define ECW_WIFI_CONTROL_H
#include "ecw_wifi_credentials.h"

/* BSP-private control call ownership, not an authenticated connection or an RF
 * retirement barrier. One platform-lifetime job per exclusively owned VIF.
 * The service must establish SDK readiness and exclusive control of WPA logging.
 * START sets the native warning log threshold before exposing credentials to WPA;
 * a competing CLI/native owner must not lower it during the service lifetime.
 * No other owner may submit control calls for that VIF until release succeeds. */
typedef enum
{
    ECW_WIFI_CONTROL_START,
    ECW_WIFI_CONTROL_STOP,
    ECW_WIFI_CONTROL_DISCONNECT
} EcwWifiControlCommand;

typedef struct
{
    bool held, posting, done, entered, timed_out, start_entered;
    uint8_t vif;
    EcwWifiControlCommand command;
    uint32_t started_ms, timeout_ms, completed_ms;
    BspError result;
    wpa_network_config_t config;
} EcwWifiControl;

/* START copies credentials before acceptance. STOP accepts zero budget and still
 * executes cleanup. DISCONNECT calls the native disconnect command without
 * removing network configuration or retiring the service; its ACK is not a
 * link-down event. It does not erase NV or certify event/scan retirement.
 * No caller pointer is retained. Minimum stack is a starting configuration,
 * not a measured stack-watermark claim. Call only from a task after BSP init. */
BspStatus ecw_wifi_control_submit(EcwWifiControl *job, uint8_t vif, EcwWifiControlCommand command,
                                  const BspWifiCredentials *credentials, uint32_t timeout_ms,
                                  uint32_t stack_bytes, unsigned priority);
/* BUSY while pending within budget. TIMEOUT is frozen even if the native call
 * later returns OK. out is written only for a terminal observation. */
BspStatus ecw_wifi_control_poll(EcwWifiControl *job, BspError *out);
/* BUSY until both task creation and the native call have returned. Success only
 * permits reuse of this control storage, never a new public Wi-Fi generation. */
BspStatus ecw_wifi_control_release(EcwWifiControl *job);
#endif

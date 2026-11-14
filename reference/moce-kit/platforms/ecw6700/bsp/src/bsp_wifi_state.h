/* Private WiFi lifecycle engine. Caller serializes all access, including callbacks.
 * This does not implement a radio backend or prove an SDK retirement barrier. */
#ifndef BSP_WIFI_STATE_INTERNAL_H
#define BSP_WIFI_STATE_INTERNAL_H
#include "bsp_wifi.h"
#define BSP_WIFI_EVENT_DEPTH 8u
typedef struct
{
    BspWifiState state;
    BspWifiCredentials credentials;
    BspWifiEvent events[BSP_WIFI_EVENT_DEPTH];
    uint32_t started_ms, budget_ms, head, count;
    bool initialized, held;
    BspWifiReconnectPolicy reconnect_policy;
} BspWifiMachine;
typedef enum
{
    BSP_WIFI_SIGNAL_LINK,
    BSP_WIFI_SIGNAL_IP,
    BSP_WIFI_SIGNAL_LOST,
    BSP_WIFI_SIGNAL_FAILED
} BspWifiSignal;
BspStatus bsp_wifi_machine_init(BspWifiMachine *m);
BspStatus bsp_wifi_machine_begin(BspWifiMachine *m, const BspWifiCredentials *c, uint32_t now,
                                 uint32_t timeout, uint64_t *serial, uint64_t *operation);
BspStatus bsp_wifi_machine_begin_configured(BspWifiMachine *m, const BspWifiConnectConfig *config,
                                            uint32_t now, uint32_t timeout, uint64_t *serial,
                                            uint64_t *operation);
/* operation gates this BSP storage lifetime only. Link observations do not
 * authenticate or complete a connect command and are published with operation=0. */
void bsp_wifi_machine_signal(BspWifiMachine *m, uint64_t operation, BspWifiSignal signal,
                             BspError error, uint32_t now);
bool bsp_wifi_machine_expire(BspWifiMachine *m, uint32_t now);
BspStatus bsp_wifi_machine_disconnecting(BspWifiMachine *m);
void bsp_wifi_machine_observe(BspWifiMachine *m, BspWifiPhase phase, BspError error);
void bsp_wifi_machine_command_done(BspWifiMachine *m, BspError error, uint32_t now);
void bsp_wifi_machine_stopping(BspWifiMachine *m);
/* Only call retired after all SDK events, sockets and owners have retired. */
void bsp_wifi_machine_retired(BspWifiMachine *m);
void bsp_wifi_machine_cleanup_error(BspWifiMachine *m, BspError error);
BspStatus bsp_wifi_machine_events(BspWifiMachine *m, BspWifiEvent *out, size_t capacity,
                                  size_t *count);
#endif

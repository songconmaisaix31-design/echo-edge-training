#ifndef ECW_BLE_HCI_GATE_H
#define ECW_BLE_HCI_GATE_H
#include "bsp_core.h"
#include "hci_interface.h"
/* Private fixed-P16 transport boundary; static boot-lifetime storage.
 * Install ONCE after native Host/pool initialization and before any controller
 * producer can run. No adoption/replacement of a live external Host. The original
 * SDK init registers an unguarded callback; this must replace it while producers
 * are excluded, and native init must not reinstall it later.
 * The sink borrows packet memory only during its call. This gate neither frees
 * nor queues it: native controller's completion callback retains that ownership. */
BspStatus ecw_ble_hci_gate_install(hci_ll_tx_func sink);
/* Lifecycle owner only, serialized install/enable/close. Enabling requires an
 * initialized persistent Host and a separately proven producer/old-packet
 * boundary. This API does NOT discover that boundary or stamp old packets with
 * generations. In particular quiet() is not sufficient permission to reopen. */
BspStatus ecw_ble_hci_gate_enable(void);
/* Prevents new sink admission immediately. Does not unregister/delete tasks,
 * flush queues, stop a radio, wait or free SDK state. In-flight sinks may finish. */
BspStatus ecw_ble_hci_gate_close(void);
/* True only while closed and no admitted sink call remains. No producer/queue
 * or native resource-release claim. An error query preserves output. */
BspStatus ecw_ble_hci_gate_quiet(bool *out);
#endif

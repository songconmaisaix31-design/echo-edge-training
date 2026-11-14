/* Connection/event/send state logic follows the existing ESP32 0.8.0 adapter.
 * Native calls and lifecycle below are ECW B02P03-specific. */
#include "ecw_ble_peripheral.h"
#include "syscfg/syscfg.h"
#include "ecw_ble_service.h"
#include "ecw_ble_gatt.h"
#include "ecw_ble_retire.h"
#include "bsp_request_internal.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
#include "host/ble_att.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_npl.h"
#include "os/os_mbuf.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>
#ifndef BSP_ECW_BLE_EVENTS
#define BSP_ECW_BLE_EVENTS 16u
#endif
#ifndef BSP_ECW_BLE_CONNECTIONS
#define BSP_ECW_BLE_CONNECTIONS 4u
#endif
#define EVENTS BSP_ECW_BLE_EVENTS
#define CONNECTIONS BSP_ECW_BLE_CONNECTIONS
#define ACTIVE_CONNECTIONS                                                                         \
    (MYNEWT_VAL(BLE_MAX_CONNECTIONS) < CONNECTIONS ? MYNEWT_VAL(BLE_MAX_CONNECTIONS) : CONNECTIONS)
_Static_assert(EVENTS >= 3 && EVENTS <= 256, "Explicit BLE event capacity out of range");
_Static_assert(CONNECTIONS >= 1 && CONNECTIONS <= 16,
               "Explicit BLE connection capacity out of range");
_Static_assert(CONFIG_BT_NIMBLE_ENABLED &&MYNEWT_VAL(BLE_ROLE_PERIPHERAL),
               "GATT database ownership requires native NimBLE peripheral role");
#if defined(CONFIG_CMD_BLE) && CONFIG_CMD_BLE
#error "Exclusive BSP GATT owner cannot coexist with the native BLE CLI owner"
#endif
#define HS_DOMAIN 0x4e494d42u
typedef struct
{
    bool live, reserved_disconnect, admitting, completion;
    uint16_t native, attribute;
    uint64_t generation, operation;
    uint8_t subscriptions[ECW_BSP_BLE_CHARACTERISTICS];
    BspError result;
} Connection;
static EcwBspBleGatt gatt;
static Connection connections[CONNECTIONS];
static BspBleEvent events[EVENTS];
static unsigned head, queued, reserved;
static BspBleState state;
static uint64_t serial, operation_serial;
static uintptr_t service_serial;
static bool owned;
static struct ble_npl_event command;
static bool command_done, command_retire, command_waiting_disconnect;
static void finish_host_command(int result);
static int command_result;
static BspError hs_error(int rc)
{
    BspStatus s = rc == 0 || rc == BLE_HS_EDONE                      ? BSP_OK
                  : rc == BLE_HS_ENOMEM                              ? BSP_NO_RESOURCE
                  : rc == BLE_HS_EBUSY                               ? BSP_BUSY
                  : rc == BLE_HS_EINVAL || rc == BLE_HS_EMSGSIZE     ? BSP_INVALID_ARG
                  : rc == BLE_HS_ENOTSUP                             ? BSP_UNSUPPORTED
                  : rc == BLE_HS_ETIMEOUT                            ? BSP_TIMEOUT
                  : rc == BLE_HS_ENOTCONN || rc == BLE_HS_ENOTSYNCED ? BSP_INVALID_STATE
                                                                     : BSP_IO;
    return (BspError){
        .code = s, .native_valid = true, .native_domain = HS_DOMAIN, .native_code = rc};
}

static bool enqueue(BspBleEvent e, bool use_reservation)
{
    if (use_reservation)
    {
        if (!reserved)
        {
            return false;
        }
        --reserved;
    }
    else if (queued + reserved >= EVENTS)
    {
        if (state.dropped_events != UINT32_MAX)
        {
            ++state.dropped_events;
        }
        return false;
    }
    events[(head + queued) % EVENTS] = e;
    ++queued;
    return true;
}

static BspBleConnection token(Connection *c)
{
    return (BspBleConnection){.slot = (uint16_t)(c - connections), .generation = c->generation};
}

static Connection *by_native(uint16_t handle)
{
    for (unsigned i = 0; i < CONNECTIONS; ++i)
    {
        if ((connections[i].live || connections[i].operation) && connections[i].native == handle)
        {
            return &connections[i];
        }
    }
    return NULL;
}

static int attr_handle(uint16_t handle)
{
    for (size_t i = 0; i < gatt.count; ++i)
    {
        if (gatt.attributes[i].handle == handle)
        {
            return (int)i;
        }
    }
    return -1;
}

static int attr_id(uint16_t id)
{
    for (size_t i = 0; i < gatt.count; ++i)
    {
        if (gatt.attributes[i].id == id)
        {
            return (int)i;
        }
    }
    return -1;
}

static void finish_indication(Connection *c, BspError result)
{
    if (!c->operation || c->completion)
    {
        return;
    }
    c->result = result;
    c->completion = true;
    if (c->admitting)
    {
        return;
    }
    (void)enqueue((BspBleEvent){.type = BSP_BLE_INDICATION_DONE,
                                .connection = token(c),
                                .characteristic = c->attribute,
                                .operation = c->operation,
                                .error = result},
                  true);
    c->operation = 0;
    c->completion = false;
}

static void disconnect(Connection *c, BspError result)
{
    if (result.code == BSP_OK)
    {
        result.code = BSP_INVALID_STATE;
    }
    finish_indication(c, result);
    if (c->reserved_disconnect)
    {
        (void)enqueue(
            (BspBleEvent){.type = BSP_BLE_DISCONNECTED, .connection = token(c), .error = result},
            true);
        c->reserved_disconnect = false;
    }
    c->live = false;
    memset(c->subscriptions, 0, sizeof(c->subscriptions));
}

static void write_rejected(void *context)
{
    (void)context;
    if (state.dropped_events != UINT32_MAX)
    {
        ++state.dropped_events;
    }
}

static bool written(void *context, uint16_t handle, uint16_t id, const uint8_t *data, uint16_t n)
{
    (void)context;
    Connection *c = by_native(handle);
    if (!c || !c->live || state.phase != BSP_BLE_READY)
    {
        write_rejected(context);
        return false;
    }
    BspBleEvent e = {
        .type = BSP_BLE_WRITTEN, .connection = token(c), .characteristic = id, .length = n};
    if (n)
    {
        memcpy(e.data, data, n);
    }
    return enqueue(e, false);
}

int ecw_bsp_ble_peripheral_gap(struct ble_gap_event *e, void *context)
{
    bool reject = false, finish_retire = false;
    int retire_result = 0;
    uint16_t reject_handle = 0;
    uintptr_t key = bsp_request_lock();
    if (!owned || (uintptr_t)context != service_serial)
    {
        bsp_request_unlock(key);
        return 0;
    }
    if (e->type == BLE_GAP_EVENT_CONNECT && !e->connect.status)
    {
        Connection *free_slot = NULL;
        for (unsigned i = 0; i < ACTIVE_CONNECTIONS; ++i)
        {
            if (!connections[i].live && !connections[i].operation && !connections[i].admitting)
            {
                free_slot = &connections[i];
                break;
            }
        }
        if (state.phase != BSP_BLE_READY || !free_slot || serial == UINT64_MAX ||
            queued + reserved + 2 > EVENTS || by_native(e->connect.conn_handle))
        {
            reject = true;
            reject_handle = e->connect.conn_handle;
            if (state.dropped_events != UINT32_MAX)
            {
                ++state.dropped_events;
            }
        }
        else
        {
            *free_slot = (Connection){.live = true,
                                      .reserved_disconnect = true,
                                      .native = e->connect.conn_handle,
                                      .generation = ++serial};
            ++reserved;
            (void)enqueue((BspBleEvent){.type = BSP_BLE_CONNECTED, .connection = token(free_slot)},
                          false);
        }
    }
    else if (e->type == BLE_GAP_EVENT_DISCONNECT)
    {
        Connection *c = by_native(e->disconnect.conn.conn_handle);
        if (c)
        {
            disconnect(c, hs_error(e->disconnect.reason));
        }
        if (command_waiting_disconnect)
        {
            finish_retire = true;
            for (size_t i = 0; i < CONNECTIONS; ++i)
            {
                if (connections[i].live) { finish_retire = false; }
            }
        }
    }
    else if (e->type == BLE_GAP_EVENT_TERM_FAILURE && command_waiting_disconnect)
    {
        if (by_native(e->term_failure.conn_handle))
        {
            finish_retire = true;
            retire_result = e->term_failure.status;
        }
    }
    else if (e->type == BLE_GAP_EVENT_SUBSCRIBE)
    {
        Connection *c = by_native(e->subscribe.conn_handle);
        int i = attr_handle(e->subscribe.attr_handle);
        if (c && c->live && i >= 0)
        {
            c->subscriptions[i] =
                (uint8_t)((e->subscribe.cur_notify ? 1 : 0) | (e->subscribe.cur_indicate ? 2 : 0));
            (void)enqueue((BspBleEvent){.type = BSP_BLE_SUBSCRIBED,
                                        .connection = token(c),
                                        .characteristic = gatt.attributes[i].id,
                                        .notify_enabled = e->subscribe.cur_notify,
                                        .indicate_enabled = e->subscribe.cur_indicate},
                          false);
        }
    }
    else if (e->type == BLE_GAP_EVENT_NOTIFY_TX && e->notify_tx.indication && e->notify_tx.status)
    {
        Connection *c = by_native(e->notify_tx.conn_handle);
        int i = attr_handle(e->notify_tx.attr_handle);
        if (c && i >= 0 && c->attribute == gatt.attributes[i].id)
        {
            finish_indication(c, hs_error(e->notify_tx.status));
        }
    }
    bsp_request_unlock(key);
    if (finish_retire)
    {
        /* B02P03 deletes the native link before this GAP callback and invokes
         * it without the Host mutex. This is the same sole Host consumer as
         * on_host; do not enqueue blocking work onto its own bounded queue. */
        if (!retire_result && gatt.prepared)
        {
            retire_result = ecw_bsp_ble_gatt_detach_on_host(&gatt);
        }
        finish_host_command(retire_result);
    }
    if (reject)
    {
        int rc = ble_gap_terminate(reject_handle, 0x13); /* Remote user termination. */
        if (rc && rc != BLE_HS_ENOTCONN)
        {
            key = bsp_request_lock();
            state.phase = BSP_BLE_FAULT;
            state.error = hs_error(rc);

            bsp_request_unlock(key);
            ecw_bsp_ble_service_fault(hs_error(rc));
        }
    }
    return 0;
}

void *ecw_bsp_ble_peripheral_cookie(void)
{
    return (void *)service_serial; /* Value token only, never dereferenced. */
}
BspStatus ecw_bsp_ble_peripheral_prepare(const BspBleService *services, size_t count)
{
    if (owned || service_serial == UINTPTR_MAX)
    {
        return owned ? BSP_BUSY : BSP_NO_RESOURCE;
    }
    for (size_t s = 0; s < count; ++s)
    {
        for (size_t c = 0; c < services[s].count; ++c)
        {
            uint32_t properties = services[s].items[c].properties;
            if (((properties & BSP_BLE_NOTIFY) && !MYNEWT_VAL(BLE_GATT_NOTIFY)) ||
                ((properties & BSP_BLE_INDICATE) && !MYNEWT_VAL(BLE_GATT_INDICATE)))
            {
                return BSP_UNSUPPORTED;
            }
        }
    }
    BspStatus result =
        count ? ecw_bsp_ble_gatt_prepare(&gatt, services, count, written, write_rejected, NULL)
              : BSP_OK;
    if (result != BSP_OK)
    {
        return result;
    }
    if (!count)
    {
        gatt.prepared = true; /* Empty, terminated service table for the exclusive owner. */
    }
    memset(connections, 0, sizeof(connections));
    head = queued = reserved = 0;
    state = (BspBleState){.phase = BSP_BLE_STARTING};
    ++service_serial;
    owned = true;
    return BSP_OK;
}
void ecw_bsp_ble_peripheral_discard_unregistered(void)
{
    /* Only the failed public admission path, before any native queue submission. */
    if (!gatt.registration_attempted && state.phase == BSP_BLE_STARTING)
    {
        ecw_bsp_ble_gatt_forget_quiescent(&gatt);
        owned = false;
        state.phase = BSP_BLE_OFF;
    }
}
static void finish_host_command(int rc)
{
    uintptr_t key = bsp_request_lock();
    if (!rc && command_retire)
    {
        for (size_t i = 0; i < CONNECTIONS; ++i)
        {
            if (connections[i].live || connections[i].operation)
            {
                disconnect(&connections[i], (BspError){.code = BSP_INVALID_STATE});
            }
        }
        /* Executing on Host after native detach: no GATT callback can be running
         * concurrently on that consumer. Old GAP value cookies remain invalid. */
        owned = false;
        ecw_bsp_ble_gatt_forget_quiescent(&gatt);
    }
    state.error = hs_error(rc);
    state.phase = rc ? BSP_BLE_FAULT : command_retire ? BSP_BLE_OFF : BSP_BLE_READY;
    command_waiting_disconnect = false;
    command_result = rc;
    command_done = true;
    bsp_request_unlock(key);
}

static void on_host(struct ble_npl_event *event)
{
    (void)event;
    int rc = 0;
    if (!command_retire)
    {
        rc = gatt.prepared ? ecw_bsp_ble_gatt_activate_on_host(&gatt)
                           : (ble_hs_synced() ? 0 : BLE_HS_ENOTSYNCED);
    }
    else
    {
        uint16_t handles[CONNECTIONS];
        size_t count = 0;
        uintptr_t key = bsp_request_lock();
        for (size_t i = 0; i < CONNECTIONS; ++i)
        {
            if (connections[i].live)
            {
                handles[count++] = connections[i].native;
            }
        }
        bsp_request_unlock(key);
        bool waiting = false;
        if (gatt.prepared)
        {
            rc = ecw_ble_retire_begin_on_host(&gatt, handles, count, &waiting);
        }
        else
        {
            for (size_t i = 0; i < count; ++i)
            {
                int result = ble_gap_terminate(handles[i], 0x13u);
                if (result && result != BLE_HS_EALREADY && result != BLE_HS_ENOTCONN)
                {
                    rc = result;
                    break;
                }
            }
            /* A command ACK does not remove a connection. Inspect native state. */
            for (size_t i = 0; !rc && i < count; ++i)
            {
                struct ble_gap_conn_desc desc;
                if (ble_gap_conn_find(handles[i], &desc) == 0)
                {
                    waiting = true;
                }
            }
        }
        if (!rc && waiting)
        {
            key = bsp_request_lock();
            command_waiting_disconnect = true;
            state.phase = BSP_BLE_STOPPING;
            bsp_request_unlock(key);
            return; /* Worker remains retained; public stop budget still applies. */
        }
    }
    finish_host_command(rc);
}

static BspError run_on_host(bool retire)
{
    struct ble_npl_eventq *queue = nimble_port_get_dflt_eventq();
    if (!queue || !queue->q)
    {
        return (BspError){.code = BSP_NOT_READY};
    }
    uintptr_t key = bsp_request_lock();
    command_done = false;
    command_waiting_disconnect = false;
    command_retire = retire;
    if (retire)
    {
        state.phase = BSP_BLE_STOPPING;
    }
    bsp_request_unlock(key);
    ble_npl_event_init(&command, on_host, NULL);
    /* Existing retained service worker only. SDK queue admission can block;
     * public init is asynchronous and public stop retains its finite budget. */
    ble_npl_eventq_put(queue, &command);
    for (;;)
    {
        key = bsp_request_lock();
        bool done = command_done;
        int result = command_result;
        bsp_request_unlock(key);
        if (done)
        {
            return hs_error(result);
        }
        vTaskDelay(1);
    }
}
BspError ecw_bsp_ble_peripheral_activate(void)
{
    return run_on_host(false);
}
BspError ecw_bsp_ble_peripheral_retire(void)
{
    return run_on_host(true);
}
void ecw_bsp_ble_peripheral_query(BspBleState *out)
{
    /* Caller holds BSP state lock. Main service remains lifecycle authority. */
    out->dropped_events = state.dropped_events;
    if (state.phase == BSP_BLE_FAULT)
    {
        out->phase = state.phase;
        out->error = state.error;
    }
}

BspStatus bsp_ble_send(BspRadio id, BspBleConnection conn, uint16_t attribute, const void *data,
                       size_t n, bool indicate, uint64_t *operation)
{
    if ((!data && n) || !operation)
    {
        return BSP_INVALID_ARG;
    }
    BspStatus s = ecw_bsp_ble_check_radio(id);
    if (s != BSP_OK)
    {
        return s;
    }
    if ((indicate && !MYNEWT_VAL(BLE_GATT_INDICATE)) || (!indicate && !MYNEWT_VAL(BLE_GATT_NOTIFY)))
    {
        return BSP_UNSUPPORTED; /* Native disabled path does not consume mbuf. */
    }
    s = ecw_bsp_ble_api_claim(id);
    if (s != BSP_OK)
    {
        return s;
    }
    uintptr_t key = bsp_request_lock();
    int a = attr_id(attribute);
    Connection *c = conn.slot < CONNECTIONS ? &connections[conn.slot] : NULL;
    if (!c || !c->live || c->generation != conn.generation || a < 0)
    {
        s = BSP_INVALID_ARG;
    }
    else if (!(gatt.attributes[a].properties & (indicate ? BSP_BLE_INDICATE : BSP_BLE_NOTIFY)) ||
             !(c->subscriptions[a] & (indicate ? 2 : 1)))
    {
        s = BSP_INVALID_STATE;
    }
    else if (n > gatt.attributes[a].max_length)
    {
        s = BSP_INVALID_ARG;
    }
    else if (indicate && c->operation)
    {
        s = BSP_BUSY;
    }
    else if (operation_serial == UINT64_MAX || (indicate && queued + reserved >= EVENTS))
    {
        s = BSP_NO_RESOURCE;
    }
    uint16_t native = c ? c->native : 0, handle = a >= 0 ? gatt.attributes[a].handle : 0;
    bsp_request_unlock(key);
    if (s != BSP_OK)
    {
        return ecw_bsp_ble_api_release((BspError){.code = s});
    }
    uint16_t mtu = ble_att_mtu(native);
    if (mtu < 3 || n > (size_t)(mtu - 3))
    {
        return ecw_bsp_ble_api_release((BspError){.code = BSP_UNSUPPORTED});
    }
    /* Recheck after native query: disconnect can arrive without API ownership. */
    key = bsp_request_lock();
    if (!c->live || c->generation != conn.generation || !(c->subscriptions[a] & (indicate ? 2 : 1)))
    {
        s = BSP_INVALID_STATE;
    }
    else if (indicate && queued + reserved >= EVENTS)
    {
        s = BSP_NO_RESOURCE;
    }
    uint64_t op = 0;
    if (s == BSP_OK)
    {
        op = ++operation_serial;
        if (indicate)
        {
            ++reserved;
            c->operation = op;
            c->attribute = attribute;
            c->admitting = true;
            c->completion = false;
        }
    }
    bsp_request_unlock(key);
    if (s != BSP_OK)
    {
        return ecw_bsp_ble_api_release((BspError){.code = s});
    }
    uint8_t empty = 0;
    struct os_mbuf *mbuf = ble_hs_mbuf_from_flat(n ? data : &empty, (uint16_t)n);
    int rc = BLE_HS_ENOMEM;
    if (mbuf)
    {
        rc = indicate ? ble_gattc_indicate_custom(native, handle, mbuf)
                      : ble_gattc_notify_custom(native, handle, mbuf);
    }
    /* Both APIs consume mbuf on success AND failure. Never free it here. */
    key = bsp_request_lock();
    if (indicate)
    {
        c->admitting = false;
        if (rc)
        {
            --reserved;
            c->operation = 0;
            c->completion = false;
        }
        else if (c->completion)
        {
            BspError result = c->result;
            c->completion = false;
            finish_indication(c, result);
        }
    }
    if (!rc)
    {
        *operation = op;
    }
    bsp_request_unlock(key);
    return ecw_bsp_ble_api_release(hs_error(rc));
}

BspStatus bsp_ble_events(BspRadio id, BspBleEvent *out, size_t capacity, size_t *count)
{
    if (!out || !capacity || !count)
    {
        return BSP_INVALID_ARG;
    }
    BspStatus s = ecw_bsp_ble_check_radio(id);
    if (s != BSP_OK)
    {
        return s;
    }
    uintptr_t key = bsp_request_lock();
    size_t n = queued < capacity ? queued : capacity;
    for (size_t i = 0; i < n; ++i)
    {
        out[i] = events[head];
        head = (head + 1) % EVENTS;
        --queued;
    }
    *count = n;
    bsp_request_unlock(key);
    return BSP_OK;
}

BspStatus bsp_ble_set_value(BspRadio id, uint16_t attribute, const void *data, size_t n)
{
    if (!data && n)
    {
        return BSP_INVALID_ARG;
    }
    BspStatus status = ecw_bsp_ble_api_claim(id);
    if (status != BSP_OK)
    {
        return status;
    }
    status = ecw_bsp_ble_gatt_set(&gatt, attribute, data, n);
    return ecw_bsp_ble_api_release((BspError){.code = status});
}

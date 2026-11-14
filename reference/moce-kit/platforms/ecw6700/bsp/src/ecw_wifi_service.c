#include "ecw_wifi_scan_native.h"
#include "ecw_wifi_ap_p16.h"
#include "ecw_wifi_service.h"
#include "ecw_wifi_control.h"
#include "ecw_netif_p16.h"
#include "bsp_wifi_state.h"
#include "bsp_request_internal.h"
#include "system_wifi.h"
#include "system_event.h"
#include "FreeRTOS.h"
#include "task.h"
#ifdef BSP_ECW_NATIVE_NET
#include "ecw_net_service.h"
#include "ecw_wifi_net.h"
static BspRadio active_radio;
static uint64_t link_epoch, link_operation;
static unsigned link_index;
static bool link_bound, link_revoked, net_suspended;
/* These generations describe processed SDK observations, not physical sessions. */
static void observe_net(unsigned index)
{
    if (link_bound && (link_revoked || !index || index != link_index))
    {
        (void)ecw_bsp_wifi_net_revoke(link_operation, link_epoch);
        link_revoked = true;
        if (ecw_bsp_wifi_net_retire(link_operation, link_epoch) != BSP_OK)
        {
            return; /* Application still owns old sockets; never close them here. */
        }
        link_bound = false;
    }
    if (index && !link_bound && link_operation && link_epoch != UINT64_MAX)
    {
        if (ecw_bsp_wifi_net_publish(active_radio, index, link_operation, link_epoch + 1) == BSP_OK)
        {
            ++link_epoch;
            link_index = index;
            link_bound = true;
            link_revoked = false;
        }
    }
}
#endif

static BspWifiMachine machine;
static EcwWifiControl control;
static uint64_t serial;
static TaskHandle_t worker;
static uint32_t worker_bytes, control_bytes;
static unsigned task_priority;
static bool exclusive, api_busy, working, subscribed, stop_requested, stop_submitted;
static BspStatus release_result, disconnect_result;
static bool disconnect_done, native_stopped;
static BspWifiApConfig ap_config;
static BspWifiApState ap_state;
static bool ap_start_command, ap_owned, ap_concurrent, ap_waiter;
static uint32_t ap_started, ap_budget;
static struct
{
    bool pending, waiter, done;
    uint32_t started, budget;
    BspStatus result;
    size_t capacity, written, total;
    BspWifiAccessPoint results[BSP_ECW_WIFI_SCAN_CAPACITY];
} scan;
static void scan_execute(void)
{
    uintptr_t key = bsp_request_lock();
    bool expired = (uint32_t)(bsp_time_ms() - scan.started) >= scan.budget;
    bsp_request_unlock(key);
    BspStatus result = expired ? BSP_TIMEOUT
        : ecw_wifi_scan_native(scan.results, scan.capacity, &scan.written, &scan.total);
    key = bsp_request_lock();
    scan.result = (uint32_t)(bsp_time_ms() - scan.started) >= scan.budget ? BSP_TIMEOUT : result;
    scan.done = true;
    scan.pending = false;
    bsp_request_unlock(key);
}


/* Caller holds the BSP lock. Deadline fixes a result, never storage ownership. */
static void ap_expire(void)
{
    if (ap_state.command_pending && !ap_state.command_completed &&
        (uint32_t)(bsp_time_ms() - ap_started) >= ap_budget)
    {
        ap_state.command_completed = true;
        ap_state.error = (BspError){.code = BSP_TIMEOUT};
    }
}
static void ap_execute(void)
{
    uintptr_t key = bsp_request_lock();
    ap_expire();
    bool expired = ap_state.command_completed;
    bool canceled = stop_requested;
    bool start = ap_start_command;
    bsp_request_unlock(key);
    bool entered = false;
    BspError result = {.code = canceled ? BSP_INVALID_STATE : BSP_TIMEOUT};
    if (!expired && !canceled)
    {
        result = start ? ecw_wifi_ap_start_native(&ap_config, ap_started, ap_budget, &entered)
                       : ecw_wifi_ap_stop_native();
    }
    /* Only the worker accesses config while pending; caller memory was copied. */
    bool concurrent = ap_config.concurrent_sta;
    volatile unsigned char *secret = (volatile unsigned char *)&ap_config;
    for (size_t i = 0; i < sizeof(ap_config); ++i)
    {
        secret[i] = 0;
    }
    key = bsp_request_lock();
    ap_expire();
    if (entered)
    {
        ap_owned = true;
        ap_concurrent = concurrent;
    }
    if (!start && !expired && !canceled && result.code == BSP_OK)
    {
        ap_owned = false;
    }
    if (!ap_state.command_completed)
    {
        ap_state.command_completed = true;
        ap_state.error = result;
    }
    ap_state.command_pending = false;
    bsp_request_unlock(key);
}

static BspStatus radio(BspRadio id)
{
    BspStatus status = bsp_delay_ms(0);
    if (status != BSP_OK)
    {
        return status;
    }
    const BspRadioInventory *list = bsp_board_get_radio_inventory();
    if (!list || (!list->radios && list->count))
    {
        return BSP_NOT_READY;
    }
    bool first = true;
    for (size_t i = 0; i < list->count; ++i)
    {
        bool wifi = (list->radios[i].hardware_features & BSP_RADIO_WIFI) != 0;
        if (list->radios[i].id == id)
        {
            return first && wifi ? BSP_OK : BSP_UNSUPPORTED;
        }
        if (wifi)
        {
            first = false;
        }
    }
    return BSP_INVALID_ARG;
}
static BspError native_error(sys_err_t raw)
{
    return (BspError){.code = raw == SYS_OK           ? BSP_OK
                              : raw == SYS_ERR_NO_MEM ? BSP_NO_RESOURCE
                                                      : BSP_IO,
                      .native_valid = true,
                      .native_domain = 0x45435745u,
                      .native_code = raw};
}
static sys_err_t changed(void *context, system_event_t *event)
{
    (void)context;
    (void)event;
    /* A wakeup only: never attach a current command ID to an old SDK event. */
    xTaskNotifyGive(worker);
    return SYS_OK;
}
static void observe(void)
{
    bool ap_active = false;
    bool ap_valid = ecw_wifi_ap_observe(&ap_active);
    uintptr_t ap_key = bsp_request_lock();
    ap_state.observation_valid = ap_valid;
    ap_state.active = ap_active;
    bsp_request_unlock(ap_key);
    unsigned index = 0;
    wifi_status_e native = sys_wifi_sta_get_status();
    BspWifiPhase phase = native == STA_STATUS_CONNECTED ? BSP_WIFI_LINK_UP
                         : native == STA_STATUS_START   ? BSP_WIFI_CONNECTING
                         : native == STATUS_ERROR       ? BSP_WIFI_FAULT
                                                        : BSP_WIFI_DISCONNECTED;
    BspError error = {0};
    if (native != STA_STATUS_CONNECTED && native != STA_STATUS_START && native != STA_STATUS_STOP &&
        native != STA_STATUS_DISCON)
    {
        phase = BSP_WIFI_FAULT;
        error = (BspError){.code = BSP_IO,
                           .native_valid = true,
                           .native_domain = 0x45435753u,
                           .native_code = native};
    }
    if (native == STA_STATUS_CONNECTED)
    {
        EcwBspNetifSnapshot snapshot;
        if (ecw_bsp_netif_snapshot(&snapshot, &error) == BSP_OK)
        {
            if (snapshot.ipv4_ready)
            {
                phase = BSP_WIFI_IP_READY;
            }
            /* Interface ownership also permits IPv6-only native networking. */
            index = snapshot.link_up ? snapshot.index : 0;
        }
    }
#ifdef BSP_ECW_NATIVE_NET
    observe_net(net_suspended ? 0 : index);
#else
    (void)index;
#endif
    uintptr_t key = bsp_request_lock();
    bsp_wifi_machine_observe(&machine, phase, error);
    bsp_request_unlock(key);
}
static void finish_release(void)
{
    sys_err_t raw =
        subscribed
            ? sys_event_unsubscribe_group((EVENT_GROUP_STA_ALL | EVENT_GROUP_AP_ALL), changed)
            : SYS_OK;
    uintptr_t key = bsp_request_lock();
    release_result = native_error(raw).code;
    stop_requested = false;
    stop_submitted = false;
    if (raw == SYS_OK)
    {
        subscribed = false;
        bsp_wifi_machine_retired(&machine);
    }
    else
    {
        bsp_wifi_machine_cleanup_error(&machine, native_error(raw));
    }
    bsp_request_unlock(key);
}
void ecw_wifi_service_step(void)
{
    uintptr_t key = bsp_request_lock();
    if (api_busy || working || !machine.initialized)
    {
        bsp_request_unlock(key);
        return;
    }
    working = true;
    bool held = control.held;
    bsp_request_unlock(key);
#ifdef BSP_ECW_NATIVE_NET
    ecw_bsp_net_service();
#endif
    if (scan.pending)
    {
        scan_execute();
        goto leave;
    }
    if (ap_state.command_pending)
    {
        ap_execute();
        /* Refresh native observation separately, even if the command failed. */
        observe();
        goto leave;
    }
    if (held)
    {
        BspError result = {0};
        BspStatus status = ecw_wifi_control_poll(&control, &result);
        key = bsp_request_lock();
        EcwWifiControlCommand command = control.command;
        bool done = control.done && !control.posting;
        if (command == ECW_WIFI_CONTROL_START && (status != BSP_BUSY || done))
        {
            bsp_wifi_machine_command_done(&machine, result,
                                          done ? control.completed_ms : bsp_time_ms());
        }
        if (command == ECW_WIFI_CONTROL_DISCONNECT && (status != BSP_BUSY || done))
        {
            if (!disconnect_done && status != BSP_OK)
            {
                bsp_wifi_machine_cleanup_error(&machine, result);
            }
            disconnect_result = status;
            disconnect_done = true;
        }
        bsp_request_unlock(key);
        if (!done)
        {
            goto leave;
        }
        /* Native storage is reusable only after the actual call returned. */
        BspError native = control.result;
        (void)ecw_wifi_control_release(&control);
        if (command == ECW_WIFI_CONTROL_STOP)
        {
            if (native.code != BSP_OK)
            {
                key = bsp_request_lock();
                release_result = native.code;
                stop_requested = false;
                stop_submitted = false;
                bsp_wifi_machine_cleanup_error(&machine, native);
                bsp_request_unlock(key);
                goto leave;
            }
            native_stopped = true;
            finish_release();
            goto leave;
        }
    }
    key = bsp_request_lock();
    bool stop = stop_requested && !stop_submitted;
    bsp_request_unlock(key);
    if (stop)
    {
        if (ap_owned)
        {
            BspError result = ecw_wifi_ap_stop_native();
            key = bsp_request_lock();
            if (result.code != BSP_OK)
            {
                release_result = result.code;
                stop_requested = false;
                bsp_wifi_machine_cleanup_error(&machine, result);
                bsp_request_unlock(key);
                goto leave; /* Retry only on a later explicit release call. */
            }
            ap_owned = false;
            bsp_request_unlock(key);
        }
        if (native_stopped)
        {
            finish_release();
            goto leave;
        }
        /* Admission was withdrawn with no socket holders before this release. */
        BspStatus status = ecw_wifi_control_submit(&control, 0, ECW_WIFI_CONTROL_STOP, NULL,
                                                   INT32_MAX, control_bytes, task_priority);
        key = bsp_request_lock();
        stop_submitted = status == BSP_OK;
        if (status != BSP_OK)
        {
            release_result = status;
            stop_requested = false;
            bsp_wifi_machine_cleanup_error(&machine, (BspError){.code = status});
        }
        bsp_request_unlock(key);
    }
    else if (!stop_submitted && !native_stopped && release_result == BSP_OK)
    {
        observe();
    }
leave:
    key = bsp_request_lock();
    working = false;
    bsp_request_unlock(key);
}
static void service_task(void *arg)
{
    (void)arg;
    for (;;)
    {
        ecw_wifi_service_step();
        uintptr_t key = bsp_request_lock();
        bool pending = scan.pending || control.held || stop_requested || ap_state.command_pending;
        bsp_request_unlock(key);
#ifdef BSP_ECW_NATIVE_NET
        pending |= ecw_bsp_net_pending();
#endif
        (void)ulTaskNotifyTake(pdTRUE, pending ? 1 : portMAX_DELAY);
    }
}
void ecw_wifi_service_wake(void)
{
    if (worker)
    {
        xTaskNotifyGive(worker);
    }
}
BspStatus ecw_wifi_service_configure(uint32_t ws, uint32_t cs, unsigned priority, bool owner)
{
    if (worker || machine.initialized)
    {
        return BSP_BUSY;
    }
    if ((ws || cs || priority || owner) &&
        (ws < 4096 || cs < 4096 || ws % sizeof(StackType_t) || cs % sizeof(StackType_t) ||
         !priority || priority >= configMAX_PRIORITIES || !owner))
    {
        return BSP_INVALID_ARG;
    }
    worker_bytes = ws;
    control_bytes = cs;
    task_priority = priority;
    exclusive = owner;
    return BSP_OK;
}
BspStatus bsp_wifi_capabilities(BspRadio id, BspWifiCapabilities *out)
{
    if (!out)
    {
        return BSP_INVALID_ARG;
    }
    BspStatus status = radio(id);
    if (status != BSP_OK && status != BSP_UNSUPPORTED)
    {
        return status;
    }
    *out = (BspWifiCapabilities){
        .operations = status == BSP_OK ? (BSP_WIFI_STA | BSP_WIFI_AP | BSP_WIFI_STA_AP | BSP_WIFI_SCAN) : 0,
        .scan_capacity = status == BSP_OK ? BSP_ECW_WIFI_SCAN_CAPACITY : 0};
    return BSP_OK;
}
BspStatus bsp_wifi_reconnect_capabilities(BspRadio id, BspWifiReconnectCapabilities *out)
{
    if (!out)
    {
        return BSP_INVALID_ARG;
    }
    BspStatus status = radio(id);
    if (status != BSP_OK && status != BSP_UNSUPPORTED)
    {
        return status;
    }
    *out = (BspWifiReconnectCapabilities){.native = status == BSP_OK};
    return BSP_OK;
}
BspStatus bsp_wifi_init(BspRadio id)
{
    BspStatus status = radio(id);
    if (status != BSP_OK)
    {
        return status;
    }
    if (!exclusive || !worker_bytes || !sys_wifi_is_ready())
    {
        return BSP_NOT_READY;
    }
    uintptr_t key = bsp_request_lock();
    if (scan.pending || scan.waiter || working || api_busy || ap_waiter)
    {
        bsp_request_unlock(key);
        return BSP_BUSY;
    }
    if (machine.initialized || machine.held)
    {
        bsp_request_unlock(key);
        return BSP_INVALID_STATE;
    }
    api_busy = true;
    bsp_request_unlock(key);
    if (!worker && xTaskCreate(service_task, "bsp_wifi", worker_bytes / sizeof(StackType_t), NULL,
                               task_priority, &worker) != pdPASS)
    {
        status = BSP_NO_RESOURCE;
    }
    if (status == BSP_OK)
    {
        /* Explicit exclusive owner: runtime policy only, never an NV write. */
        sys_wifi_set_auto_save_nv(false);
        sys_err_t raw = sys_event_subscribe_group_app((EVENT_GROUP_STA_ALL | EVENT_GROUP_AP_ALL),
                                                      changed, EVENT_APP_PRIO_NORMAL, NULL);
        status = native_error(raw).code;
        subscribed = raw == SYS_OK;
    }
    key = bsp_request_lock();
    if (status == BSP_OK)
    {
        status = bsp_wifi_machine_init(&machine);
        ap_state = (BspWifiApState){0};
        native_stopped = false;
        release_result = BSP_OK;
#ifdef BSP_ECW_NATIVE_NET
        active_radio = id;
        link_operation = 0;
        net_suspended = false;
#endif
    }
    api_busy = false;
    bsp_request_unlock(key);
    if (status == BSP_OK)
    {
        xTaskNotifyGive(worker);
    }
    return status;
}
BspStatus bsp_wifi_connect_configured(BspRadio id, const BspWifiConnectConfig *config,
                                      uint32_t timeout, uint64_t *operation)
{
    if (!config || !operation || timeout > INT32_MAX ||
        (config->reconnect_policy != BSP_WIFI_RECONNECT_DISABLED &&
         config->reconnect_policy != BSP_WIFI_RECONNECT_NATIVE))
    {
        return BSP_INVALID_ARG;
    }
    BspStatus status = ecw_wifi_credentials_check(&config->credentials);
    if (status != BSP_OK)
    {
        return status;
    }
    status = radio(id);
    if (status != BSP_OK)
    {
        return status;
    }
    if (config->reconnect_policy != BSP_WIFI_RECONNECT_NATIVE)
    {
        return BSP_UNSUPPORTED;
    }
    uintptr_t key = bsp_request_lock();
    if (scan.pending || scan.waiter || api_busy || working || ap_waiter || control.held || stop_requested || machine.held ||
        ap_state.command_pending || (ap_owned && !ap_concurrent))
    {
        bsp_request_unlock(key);
        return BSP_BUSY;
    }
    /* Retain the old queryable result if task admission fails. */
    BspWifiMachine previous = machine;
    uint64_t previous_serial = serial, admitted = 0;
    /* The native observation may be DISCONNECTED even before the first command. */
    if (!machine.held && machine.initialized)
    {
        machine.state.phase = BSP_WIFI_IDLE;
    }
    status = bsp_wifi_machine_begin_configured(&machine, config, bsp_time_ms(), timeout, &serial,
                                               &admitted);
    if (status != BSP_OK)
    {
        machine = previous;
        bsp_request_unlock(key);
        return status;
    }
    api_busy = true;
    bsp_request_unlock(key);
    status = ecw_wifi_control_submit(&control, 0, ECW_WIFI_CONTROL_START, &config->credentials,
                                     timeout, control_bytes, task_priority);
    key = bsp_request_lock();
    if (status == BSP_OK)
    {
        *operation = admitted;
#ifdef BSP_ECW_NATIVE_NET
        link_operation = admitted;
#endif
    }
    else
    {
        machine = previous;
        serial = previous_serial;
    }
    api_busy = false;
    bsp_request_unlock(key);
    volatile unsigned char *secret = (volatile unsigned char *)&previous;
    for (size_t i = 0; i < sizeof(previous); ++i)
    {
        secret[i] = 0;
    }
    if (status == BSP_OK)
    {
        xTaskNotifyGive(worker);
    }
    return status;
}
BspStatus bsp_wifi_connect(BspRadio id, const BspWifiCredentials *credentials, uint32_t timeout,
                           uint64_t *operation)
{
    if (!credentials)
    {
        return BSP_INVALID_ARG;
    }
    BspWifiConnectConfig config = {.credentials = *credentials};
    return bsp_wifi_connect_configured(id, &config, timeout, operation);
}
BspStatus bsp_wifi_connect_result(BspRadio id, uint64_t operation, BspWifiConnectResult *out)
{
    if (!operation || !out)
    {
        return BSP_INVALID_ARG;
    }
    BspStatus status = radio(id);
    if (status != BSP_OK)
    {
        return status;
    }
    BspError native = {0};
    uintptr_t key = bsp_request_lock();
    if (operation != machine.state.operation)
    {
        bsp_request_unlock(key);
        return BSP_INVALID_ARG;
    }
    if (scan.pending || scan.waiter || api_busy || working || ap_waiter)
    {
        bsp_request_unlock(key);
        return BSP_BUSY;
    }
    /* Exclude release/reuse of control storage while observing its outcome. */
    api_busy = true;
    bool command_held = control.held && control.command == ECW_WIFI_CONTROL_START;
    bsp_request_unlock(key);
    BspStatus observed = command_held ? ecw_wifi_control_poll(&control, &native) : BSP_BUSY;
    key = bsp_request_lock();
    if (command_held && (observed != BSP_BUSY || control.done))
    {
        bsp_wifi_machine_command_done(&machine, native,
                                      control.done ? control.completed_ms : bsp_time_ms());
    }
    if (operation != machine.state.operation)
    {
        api_busy = false;
        bsp_request_unlock(key);
        return BSP_INVALID_ARG;
    }
    (void)bsp_wifi_machine_expire(&machine, bsp_time_ms());
    *out = (BspWifiConnectResult){operation, machine.state.settled, machine.state.error};
    api_busy = false;
    bsp_request_unlock(key);
    return BSP_OK;
}
BspStatus bsp_wifi_query(BspRadio id, BspWifiState *out)
{
    if (!out)
    {
        return BSP_INVALID_ARG;
    }
    BspStatus status = radio(id);
    if (status != BSP_OK)
    {
        return status;
    }
    uintptr_t key = bsp_request_lock();
    *out = machine.state;
    bsp_request_unlock(key);
    if (worker)
    {
        xTaskNotifyGive(worker);
    }
    return BSP_OK;
}
BspStatus bsp_wifi_events(BspRadio id, BspWifiEvent *out, size_t capacity, size_t *count)
{
    if (!out || !capacity || !count)
    {
        return BSP_INVALID_ARG;
    }
    BspStatus status = radio(id);
    if (status != BSP_OK)
    {
        return status;
    }
    uintptr_t key = bsp_request_lock();
    status = bsp_wifi_machine_events(&machine, out, capacity, count);
    bsp_request_unlock(key);
    return status;
}
BspStatus bsp_wifi_scan(BspRadio id, BspWifiAccessPoint *out, size_t capacity, size_t *written,
                        size_t *total, uint32_t timeout)
{
    if (!out || !capacity || !written || !total || timeout > INT32_MAX)
    {
        return BSP_INVALID_ARG;
    }
    BspStatus status = radio(id);
    if (status != BSP_OK)
    {
        return status;
    }
    uintptr_t key = bsp_request_lock();
    if (scan.pending || scan.waiter || api_busy || working || ap_waiter || control.held ||
        stop_requested || ap_state.command_pending)
    {
        bsp_request_unlock(key);
        return BSP_BUSY;
    }
    if (!machine.initialized)
    {
        bsp_request_unlock(key);
        return BSP_NOT_READY;
    }
    if (!timeout)
    {
        bsp_request_unlock(key);
        return BSP_TIMEOUT;
    }
    scan.started = bsp_time_ms();
    scan.budget = timeout;
    scan.capacity = capacity < BSP_ECW_WIFI_SCAN_CAPACITY ? capacity : BSP_ECW_WIFI_SCAN_CAPACITY;
    scan.written = scan.total = 0;
    scan.done = false;
    scan.pending = scan.waiter = true;
    bsp_request_unlock(key);
    xTaskNotifyGive(worker);
    for (;;)
    {
        key = bsp_request_lock();
        if (scan.done)
        {
            status = scan.result;
            if (status == BSP_OK)
            {
                memcpy(out, scan.results, scan.written * sizeof(*out));
                *written = scan.written;
                *total = scan.total;
            }
            scan.waiter = false;
            bsp_request_unlock(key);
            return status;
        }
        if ((uint32_t)(bsp_time_ms() - scan.started) >= timeout)
        {
            scan.waiter = false;
            bsp_request_unlock(key);
            return BSP_TIMEOUT; /* Retained job contains no caller pointers. */
        }
        bsp_request_unlock(key);
        vTaskDelay(1);
    }
}
BspStatus bsp_wifi_disconnect(BspRadio id, uint32_t timeout)
{
    if (timeout > INT32_MAX)
    {
        return BSP_INVALID_ARG;
    }
    BspStatus status = radio(id);
    if (status != BSP_OK)
    {
        return status;
    }
    uintptr_t key = bsp_request_lock();
    if (scan.pending || scan.waiter || api_busy || working || ap_waiter || control.held || stop_requested ||
        ap_state.command_pending)
    {
        bsp_request_unlock(key);
        return BSP_BUSY;
    }
    if (!machine.initialized)
    {
        bsp_request_unlock(key);
        return BSP_NOT_READY;
    }
    if (!machine.held)
    {
        bsp_request_unlock(key);
        return BSP_OK;
    }
    if (!timeout)
    {
        bsp_request_unlock(key);
        return BSP_TIMEOUT;
    }
    api_busy = true;
    disconnect_done = false;
    uint32_t start = bsp_time_ms();
    bsp_request_unlock(key);
    status = ecw_wifi_control_submit(&control, 0, ECW_WIFI_CONTROL_DISCONNECT, NULL, timeout,
                                     control_bytes, task_priority);
#ifdef BSP_ECW_NATIVE_NET
    if (status == BSP_OK)
    {
        net_suspended = true;
        observe_net(0);
    }
#endif
    key = bsp_request_lock();
    if (status == BSP_OK)
    {
        (void)bsp_wifi_machine_disconnecting(&machine);
    }
    api_busy = false;
    bsp_request_unlock(key);
    if (status != BSP_OK)
    {
        return status;
    }
    xTaskNotifyGive(worker);
    for (;;)
    {
        key = bsp_request_lock();
        bool done = disconnect_done;
        status = disconnect_result;
        bsp_request_unlock(key);
        if (done)
        {
            return status;
        }
        if ((uint32_t)(bsp_time_ms() - start) >= timeout)
        {
            return BSP_TIMEOUT;
        }
        vTaskDelay(1);
    }
}
BspStatus bsp_wifi_stop(BspRadio id, uint32_t timeout)
{
    if (timeout > INT32_MAX)
    {
        return BSP_INVALID_ARG;
    }
    BspStatus status = radio(id);
    if (status != BSP_OK)
    {
        return status;
    }
    uint32_t start = bsp_time_ms();
    uintptr_t key = bsp_request_lock();
    if (scan.pending || scan.waiter || api_busy || working || ap_waiter)
    {
        bsp_request_unlock(key);
        return BSP_BUSY;
    }
    if (!machine.initialized)
    {
        bsp_request_unlock(key);
        return BSP_OK;
    }
#ifdef BSP_ECW_NATIVE_NET
    api_busy = true;
    bsp_request_unlock(key);
    status = ecw_bsp_wifi_net_withdraw();
    key = bsp_request_lock();
    api_busy = false;
    if (status != BSP_OK)
    {
        bsp_request_unlock(key);
        return status;
    }
    link_bound = false;
    link_revoked = false;
#endif
    stop_requested = true;
    release_result = BSP_OK;
    bsp_wifi_machine_stopping(&machine);
    bsp_request_unlock(key);
    xTaskNotifyGive(worker);
    for (;;)
    {
        key = bsp_request_lock();
        bool done = !machine.initialized;
        status = release_result;
        bsp_request_unlock(key);
        if (done || status != BSP_OK)
        {
            return status;
        }
        if ((uint32_t)(bsp_time_ms() - start) >= timeout)
        {
            return BSP_TIMEOUT;
        }
        vTaskDelay(1);
    }
}

static BspStatus ap_command(BspRadio id, const BspWifiApConfig *config, uint32_t timeout)
{
    if (timeout > INT32_MAX)
    {
        return BSP_INVALID_ARG;
    }
    BspStatus status = radio(id);
    if (status != BSP_OK)
    {
        return status;
    }
    if (config)
    {
        status = ecw_wifi_ap_check(config);
        if (status != BSP_OK)
        {
            return status;
        }
    }
    uintptr_t key = bsp_request_lock();
    if (scan.pending || scan.waiter || api_busy || working || ap_waiter || control.held || stop_requested ||
        ap_state.command_pending)
    {
        bsp_request_unlock(key);
        return BSP_BUSY;
    }
    if (!machine.initialized)
    {
        bsp_request_unlock(key);
        return BSP_NOT_READY;
    }
    if (machine.state.phase == BSP_WIFI_STOPPING || native_stopped || release_result != BSP_OK)
    {
        bsp_request_unlock(key);
        return BSP_INVALID_STATE;
    }
    if (config && (ap_owned || (!config->concurrent_sta && machine.held)))
    {
        bsp_request_unlock(key);
        return BSP_BUSY;
    }
    if (!config && !ap_owned)
    {
        bsp_request_unlock(key);
        return BSP_OK;
    }
    if (!timeout)
    {
        bsp_request_unlock(key);
        return BSP_TIMEOUT;
    }
    ap_started = bsp_time_ms();
    ap_budget = timeout;
    ap_start_command = config != NULL;
    ap_waiter = true;
    if (config)
    {
        ap_config = *config;
    }
    ap_state.command_pending = true;
    ap_state.command_completed = false;
    ap_state.error = (BspError){0};
    bsp_request_unlock(key);
    xTaskNotifyGive(worker);
    for (;;)
    {
        key = bsp_request_lock();
        ap_expire();
        bool done = ap_state.command_completed;
        status = ap_state.error.code;
        if (done)
        {
            ap_waiter = false;
        }
        bsp_request_unlock(key);
        if (done)
        {
            return status;
        }
        vTaskDelay(1);
    }
}
BspStatus bsp_wifi_ap_start(BspRadio id, const BspWifiApConfig *config, uint32_t timeout)
{
    if (!config)
    {
        return BSP_INVALID_ARG;
    }
    return ap_command(id, config, timeout);
}
BspStatus bsp_wifi_ap_stop(BspRadio id, uint32_t timeout)
{
    return ap_command(id, NULL, timeout);
}
BspStatus bsp_wifi_ap_query(BspRadio id, BspWifiApState *out)
{
    if (!out)
    {
        return BSP_INVALID_ARG;
    }
    BspStatus status = radio(id);
    if (status != BSP_OK)
    {
        return status;
    }
    uintptr_t key = bsp_request_lock();
    if (!machine.initialized)
    {
        bsp_request_unlock(key);
        return BSP_NOT_READY;
    }
    ap_expire();
    *out = ap_state;
    bsp_request_unlock(key);
    xTaskNotifyGive(worker);
    return BSP_OK;
}

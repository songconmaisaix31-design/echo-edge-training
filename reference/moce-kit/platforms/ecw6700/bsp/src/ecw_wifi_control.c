#include "ecw_wifi_control.h"
#include "bsp_request_internal.h"
#include "wpas_cntrl_api.h"
#include "wpa_debug.h"
#include "FreeRTOS.h"
#include "task.h"
#include <limits.h>

#define ECW_WIFI_CONTROL_DOMAIN 0x45435743u

static BspError native_error(sys_err_t raw)
{
    BspStatus status;
    switch (raw)
    {
    case SYS_OK:
        status = BSP_OK;
        break;
    case SYS_ERR_NO_MEM:
        status = BSP_NO_RESOURCE;
        break;
    case SYS_ERR_INVALID_ARG:
        status = BSP_INVALID_ARG;
        break;
    case SYS_ERR_INVALID_STATE:
        status = BSP_NOT_READY;
        break;
    case SYS_ERR_NOT_SUPPORTED:
        status = BSP_UNSUPPORTED;
        break;
    case SYS_ERR_TIMEOUT:
    case SYS_ERR_WIFI_TIMEOUT:
        status = BSP_TIMEOUT;
        break;
    case SYS_ERR_BUSY:
        status = BSP_BUSY;
        break;
    default:
        status = BSP_IO;
        break;
    }
    return (BspError){.code = status,
                      .native_valid = true,
                      .native_domain = ECW_WIFI_CONTROL_DOMAIN,
                      .native_code = raw};
}

static bool expired(EcwWifiControl *j)
{
    return (uint32_t)(bsp_time_ms() - j->started_ms) >= j->timeout_ms;
}

static void run(void *arg)
{
    EcwWifiControl *j = arg;
    uintptr_t key = bsp_request_lock();
    j->timed_out = j->timed_out || expired(j);
    bool enter = j->command == ECW_WIFI_CONTROL_STOP || !j->timed_out;
    j->entered = enter;
    bsp_request_unlock(key);

    BspError result = {.code = BSP_TIMEOUT};
    if (enter)
    {
        if (j->command == ECW_WIFI_CONTROL_START)
        {
            /* P16 network_config logs counted SSID/password with %s at DEBUG.
             * Use its checked owner-thread setter, never a direct global write.
             * This is global WPA policy: the service owns it exclusively. */
            result = native_error(wpas_ctrl_system_set_log_level(MSG_WARNING));
            if (result.code == BSP_OK)
            {
                key = bsp_request_lock();
                j->timed_out = j->timed_out || expired(j);
                j->start_entered = !j->timed_out;
                bool start_allowed = j->start_entered;
                bsp_request_unlock(key);
                result = start_allowed ? native_error(wpas_ctrl_network_start(j->vif, &j->config))
                                       : (BspError){.code = BSP_TIMEOUT};
            }
        }
        else if (j->command == ECW_WIFI_CONTROL_DISCONNECT)
        {
            result = native_error(wpas_ctrl_network_disconnect(j->vif));
        }
        else
        {
            result = native_error(wpas_ctrl_network_stop(j->vif));
        }
    }

    /* The synchronous native wrapper has returned; its configuration access has
     * finished. Native WPA copies are owned by WPA and are not erased here. */
    ecw_wifi_credentials_clear(&j->config);
    key = bsp_request_lock();
    j->timed_out = j->timed_out || expired(j);
    j->result = result;
    j->completed_ms = bsp_time_ms();
    j->done = true;
    bsp_request_unlock(key);
    /* No job access after publishing done, including during task deletion. */
    vTaskDelete(NULL);
}

BspStatus ecw_wifi_control_submit(EcwWifiControl *j, uint8_t vif, EcwWifiControlCommand command,
                                  const BspWifiCredentials *credentials, uint32_t timeout,
                                  uint32_t stack_bytes, unsigned priority)
{
    if (!j || vif >= ESWIN_WIFI_IF_NUM ||
        (command != ECW_WIFI_CONTROL_START && command != ECW_WIFI_CONTROL_STOP &&
         command != ECW_WIFI_CONTROL_DISCONNECT) ||
        timeout > INT32_MAX || stack_bytes < 4096 || !priority ||
        priority >= configMAX_PRIORITIES || stack_bytes % sizeof(StackType_t) ||
        (uint64_t)(configSTACK_DEPTH_TYPE)(stack_bytes / sizeof(StackType_t)) !=
            stack_bytes / sizeof(StackType_t) ||
        (command != ECW_WIFI_CONTROL_START && credentials))
    {
        return BSP_INVALID_ARG;
    }
    if (command == ECW_WIFI_CONTROL_START)
    {
        BspStatus status = ecw_wifi_credentials_check(credentials);
        if (status != BSP_OK)
        {
            return status;
        }
        if (!timeout)
        {
            return BSP_UNSUPPORTED;
        }
    }
    if (command == ECW_WIFI_CONTROL_DISCONNECT && !timeout)
    {
        return BSP_TIMEOUT;
    }
    uintptr_t key = bsp_request_lock();
    if (j->held)
    {
        bsp_request_unlock(key);
        return BSP_BUSY;
    }
    *j = (EcwWifiControl){.held = true,
                          .posting = true,
                          .vif = vif,
                          .command = command,
                          .started_ms = bsp_time_ms(),
                          .timeout_ms = timeout};
    if (command == ECW_WIFI_CONTROL_START)
    {
        BspStatus status = ecw_wifi_credentials_copy(&j->config, credentials);
        if (status != BSP_OK)
        {
            *j = (EcwWifiControl){0};
            bsp_request_unlock(key);
            return status;
        }
    }
    bsp_request_unlock(key);
    BaseType_t created =
        xTaskCreate(run, "bsp_wifi_ctl",
                    (configSTACK_DEPTH_TYPE)(stack_bytes / sizeof(StackType_t)), j, priority, NULL);
    key = bsp_request_lock();
    j->posting = false;
    if (created != pdPASS)
    {
        ecw_wifi_credentials_clear(&j->config);
        *j = (EcwWifiControl){0};
    }
    bsp_request_unlock(key);
    return created == pdPASS ? BSP_OK : BSP_NO_RESOURCE;
}

BspStatus ecw_wifi_control_poll(EcwWifiControl *j, BspError *out)
{
    if (!j || !out)
    {
        return BSP_INVALID_ARG;
    }
    uintptr_t key = bsp_request_lock();
    BspStatus status = BSP_BUSY;
    if (!j->held)
    {
        status = BSP_NOT_READY;
    }
    else
    {
        /* Observation after a timely completion cannot retroactively time out. */
        if (!j->done && expired(j))
        {
            j->timed_out = true;
        }
        if (j->timed_out)
        {
            *out = (BspError){.code = BSP_TIMEOUT};
            status = BSP_TIMEOUT;
        }
        else if (j->done)
        {
            *out = j->result;
            status = out->code;
        }
    }
    bsp_request_unlock(key);
    return status;
}

BspStatus ecw_wifi_control_release(EcwWifiControl *j)
{
    if (!j)
    {
        return BSP_INVALID_ARG;
    }
    uintptr_t key = bsp_request_lock();
    if (j->held && (j->posting || !j->done))
    {
        bsp_request_unlock(key);
        return BSP_BUSY;
    }
    ecw_wifi_credentials_clear(&j->config);
    *j = (EcwWifiControl){0};
    bsp_request_unlock(key);
    return BSP_OK;
}

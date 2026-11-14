#include "bsp_wifi_state.h"
#include <string.h>
static BspError result(BspStatus status)
{
    BspError e = {0};
    e.code = status;
    return e;
}
static void scrub(BspWifiCredentials *c)
{
    volatile unsigned char *p = (volatile unsigned char *)c;
    for (size_t i = 0; i < sizeof(*c); ++i)
    {
        p[i] = 0;
    }
}
static void event(BspWifiMachine *m, BspWifiPhase phase, BspError error)
{
    if (m->count == BSP_WIFI_EVENT_DEPTH)
    {
        if (m->state.dropped_events != UINT32_MAX)
        {
            ++m->state.dropped_events;
        }
        return;
    }
    uint32_t index = (m->head + m->count) % BSP_WIFI_EVENT_DEPTH;
    m->events[index] = (BspWifiEvent){0, phase, error};
    ++m->count;
}
static void terminal(BspWifiMachine *m, BspWifiPhase phase, BspError error)
{
    if (m->state.settled)
    {
        return;
    }
    m->state.settled = true;
    m->state.terminal_phase = phase;
    m->state.error = error;
}
static bool valid(const BspWifiCredentials *c)
{
    if (!c || !c->ssid_length || c->ssid_length > 32 || c->secret_length > 64)
    {
        return false;
    }
    if (c->security == BSP_WIFI_OPEN)
    {
        return c->secret_length == 0;
    }
    if (c->security == BSP_WIFI_WPA3_SAE)
    {
        return c->secret_length >= 1 && c->secret_length <= 63;
    }
    if (c->security != BSP_WIFI_WPA2_PSK || c->secret_length < 8)
    {
        return false;
    }
    if (c->secret_length == 64)
    {
        for (unsigned i = 0; i < 64; ++i)
        {
            unsigned char x = c->secret[i];
            if (!((x >= '0' && x <= '9') || (x >= 'a' && x <= 'f') || (x >= 'A' && x <= 'F')))
            {
                return false;
            }
        }
    }
    return true;
}
BspStatus bsp_wifi_machine_init(BspWifiMachine *m)
{
    if (!m)
    {
        return BSP_INVALID_ARG;
    }
    if (m->initialized || m->held)
    {
        return BSP_INVALID_STATE;
    }
    /* Reinitialization must not erase the latest admitted operation's outcome.
  * Only begin replaces it. A new service instance gets a fresh event queue. */
    BspWifiState previous = m->state;
    memset(m, 0, sizeof(*m));
    m->initialized = true;
    m->state.phase = BSP_WIFI_IDLE;
    if (previous.settled)
    {
        m->state.operation = previous.operation;
        m->state.settled = true;
        m->state.terminal_phase = previous.terminal_phase;
        m->state.error = previous.error;
    }
    return BSP_OK;
}
BspStatus bsp_wifi_machine_begin(BspWifiMachine *m, const BspWifiCredentials *c, uint32_t now,
                                 uint32_t timeout, uint64_t *serial, uint64_t *operation)
{
    if (!c)
    {
        return BSP_INVALID_ARG;
    }
    BspWifiConnectConfig config = {.credentials = *c,
                                   .reconnect_policy = BSP_WIFI_RECONNECT_DISABLED};
    BspStatus status =
        bsp_wifi_machine_begin_configured(m, &config, now, timeout, serial, operation);
    scrub(&config.credentials);
    return status;
}
BspStatus bsp_wifi_machine_begin_configured(BspWifiMachine *m, const BspWifiConnectConfig *config,
                                            uint32_t now, uint32_t timeout, uint64_t *serial,
                                            uint64_t *operation)
{
    if (!config || (config->reconnect_policy != BSP_WIFI_RECONNECT_DISABLED &&
                    config->reconnect_policy != BSP_WIFI_RECONNECT_NATIVE))
    {
        return BSP_INVALID_ARG;
    }
    const BspWifiCredentials *c = &config->credentials;
    if (!m || !serial || !operation || !valid(c) || timeout > INT32_MAX)
    {
        return BSP_INVALID_ARG;
    }
    if (!m->initialized)
    {
        return BSP_NOT_READY;
    }
    if (m->held || m->state.phase != BSP_WIFI_IDLE)
    {
        return BSP_BUSY;
    }
    if (!timeout)
    {
        return BSP_TIMEOUT;
    }
    if (*serial == UINT64_MAX)
    {
        return BSP_NO_RESOURCE;
    }
    m->credentials = *c;
    m->reconnect_policy = config->reconnect_policy;
    m->started_ms = now;
    m->budget_ms = timeout;
    m->held = true;
    m->state.operation = ++*serial;
    m->state.phase = BSP_WIFI_CONNECTING;
    m->state.settled = false;
    m->state.terminal_phase = BSP_WIFI_OFF;
    m->state.error = result(BSP_OK);
    *operation = m->state.operation;
    event(m, BSP_WIFI_CONNECTING, result(BSP_OK));
    return BSP_OK;
}
bool bsp_wifi_machine_expire(BspWifiMachine *m, uint32_t now)
{
    /* Link observations cannot cancel the native command deadline. Explicit
     * stop/disconnect already fixes the command result through terminal(). */
    if (!m || !m->held || m->state.settled)
    {
        return false;
    }
    if ((uint32_t)(now - m->started_ms) < m->budget_ms)
    {
        return false;
    }
    terminal(m, BSP_WIFI_FAULT, result(BSP_TIMEOUT));
    return true;
}
void bsp_wifi_machine_signal(BspWifiMachine *m, uint64_t operation, BspWifiSignal signal,
                             BspError error, uint32_t now)
{
    if (!m || !m->held || operation != m->state.operation)
    {
        return;
    }
    (void)now;
    if (m->state.phase == BSP_WIFI_STOPPING || m->state.phase == BSP_WIFI_FAULT)
    {
        return;
    }
    if (m->state.phase == BSP_WIFI_DISCONNECTED)
    {
        return; /* Late LINK/IP cannot revive an explicitly disconnected service. */
    }
    if (m->state.phase == BSP_WIFI_DISCONNECTING)
    {
        if (signal == BSP_WIFI_SIGNAL_LOST)
        {
            m->state.phase = BSP_WIFI_DISCONNECTED;
            error.code = BSP_OK; /* Requested link-down; preserve native reason if present. */
            event(m, BSP_WIFI_DISCONNECTED, error);
        }
        return;
    }
    if (signal == BSP_WIFI_SIGNAL_LINK)
    {
        if (m->state.phase != BSP_WIFI_CONNECTING)
        {
            return;
        }
        m->state.phase = BSP_WIFI_LINK_UP;
        event(m, BSP_WIFI_LINK_UP, result(BSP_OK));
    }
    else if (signal == BSP_WIFI_SIGNAL_IP)
    {
        if (m->state.phase == BSP_WIFI_IP_READY)
        {
            return;
        }
        m->state.phase = BSP_WIFI_IP_READY;
        event(m, BSP_WIFI_IP_READY, result(BSP_OK));
    }
    else if (signal == BSP_WIFI_SIGNAL_LOST)
    {
        m->state.phase = m->reconnect_policy == BSP_WIFI_RECONNECT_NATIVE ? BSP_WIFI_CONNECTING
                                                                          : BSP_WIFI_DISCONNECTED;
        event(m, m->state.phase, error.code == BSP_OK ? result(BSP_IO) : error);
    }
    else if (signal == BSP_WIFI_SIGNAL_FAILED)
    {
        bsp_wifi_machine_command_done(m, error.code == BSP_OK ? result(BSP_IO) : error, now);
        m->state.phase = BSP_WIFI_FAULT;
    }
}
void bsp_wifi_machine_observe(BspWifiMachine *m, BspWifiPhase phase, BspError error)
{
    if (!m || !m->initialized || m->state.phase == BSP_WIFI_STOPPING || m->state.phase == phase)
    {
        return;
    }
    m->state.phase = phase;
    event(m, phase, error);
}
void bsp_wifi_machine_command_done(BspWifiMachine *m, BspError error, uint32_t now)
{
    if (!m || !m->held || m->state.settled)
    {
        return;
    }
    if (bsp_wifi_machine_expire(m, now))
    {
        return;
    }
    terminal(m, error.code == BSP_OK ? BSP_WIFI_CONNECTING : BSP_WIFI_FAULT, error);
}

BspStatus bsp_wifi_machine_disconnecting(BspWifiMachine *m)
{
    if (!m || !m->initialized)
    {
        return BSP_NOT_READY;
    }
    if (!m->held || m->state.phase == BSP_WIFI_STOPPING || m->state.phase == BSP_WIFI_OFF)
    {
        return BSP_INVALID_STATE;
    }
    m->state.phase = BSP_WIFI_DISCONNECTING;
    if (!m->state.settled)
    {
        terminal(m, BSP_WIFI_OFF, result(BSP_INVALID_STATE));
    }
    event(m, BSP_WIFI_DISCONNECTING, result(BSP_OK));
    return BSP_OK;
}
void bsp_wifi_machine_stopping(BspWifiMachine *m)
{
    if (!m || !m->initialized)
    {
        return;
    }
    m->state.phase = BSP_WIFI_STOPPING;
    /* An explicit cancellation has terminal phase OFF, not a fabricated IP result. */
    if (m->held && !m->state.settled)
    {
        terminal(m, BSP_WIFI_OFF, result(BSP_INVALID_STATE));
    }
}
void bsp_wifi_machine_cleanup_error(BspWifiMachine *m, BspError error)
{
    if (!m || !m->initialized || error.code == BSP_OK)
    {
        return;
    }
    m->state.phase = BSP_WIFI_FAULT;
    if (m->held && !m->state.settled)
    {
        terminal(m, BSP_WIFI_FAULT, error);
    }
    else
    {
        event(m, BSP_WIFI_FAULT, error);
    }
    /* Frozen connection outcome is not overwritten by subsequent cleanup failure. */
}
void bsp_wifi_machine_retired(BspWifiMachine *m)
{
    if (!m)
    {
        return;
    }
    if (m->initialized && m->state.phase != BSP_WIFI_STOPPING && m->state.phase != BSP_WIFI_FAULT)
    {
        return;
    }
    scrub(&m->credentials);
    m->held = false;
    m->initialized = false;
    m->state.phase = BSP_WIFI_OFF;
}
BspStatus bsp_wifi_machine_events(BspWifiMachine *m, BspWifiEvent *out, size_t capacity,
                                  size_t *count)
{
    if (!m || !out || !capacity || !count)
    {
        return BSP_INVALID_ARG;
    }
    size_t n = capacity < m->count ? capacity : m->count;
    for (size_t i = 0; i < n; ++i)
    {
        out[i] = m->events[m->head];
        m->head = (m->head + 1) % BSP_WIFI_EVENT_DEPTH;
    }
    m->count -= (uint32_t)n;
    *count = n;
    return BSP_OK;
}

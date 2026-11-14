#ifndef BSP_SPP_VALIDATION_H
#define BSP_SPP_VALIDATION_H
#include "bsp_spp.h"
static inline bool bsp_spp_name_valid(const char *name)
{
    if (!name || !name[0])
    {
        return false;
    }
    for (size_t i = 1; i <= 31; ++i)
    {
        if (!name[i])
        {
            return true;
        }
    }
    return false;
}
static inline bool bsp_spp_config_valid(const BspSppConfig *c)
{
    return c && bsp_spp_name_valid(c->device_name) && c->pin_length <= 16 &&
           (!c->pin_length || c->pin) &&
           (c->reconnect_policy == BSP_SPP_RECONNECT_DISABLED ||
            c->reconnect_policy == BSP_SPP_RECONNECT_ON_LINK_LOSS);
}
static inline bool bsp_spp_security_valid(BspSppSecurity s)
{
    return s == BSP_SPP_SECURITY_NONE || s == BSP_SPP_SECURITY_ENCRYPTED;
}
static inline bool bsp_spp_listen_valid(const BspSppListenConfig *c)
{
    return c && bsp_spp_name_valid(c->service_name) && c->channel <= 30 &&
           bsp_spp_security_valid(c->security);
}
static inline bool bsp_spp_connect_valid(const BspSppConnectConfig *c)
{
    return c && c->channel >= 1 && c->channel <= 30 && bsp_spp_security_valid(c->security);
}
#endif

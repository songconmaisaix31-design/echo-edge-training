#include "bsp_spp_validation.h"
static BspStatus unavailable(BspRadio id)
{
    BspStatus s = bsp_delay_ms(0);
    if (s != BSP_OK)
    {
        return s;
    }
    const BspRadioInventory *v = bsp_board_get_radio_inventory();
    if (!v || (v->count && !v->radios))
    {
        return BSP_NOT_READY;
    }
    for (size_t i = 0; i < v->count; ++i)
    {
        if (v->radios[i].id == id)
        {
            return BSP_UNSUPPORTED;
        }
    }
    return BSP_INVALID_ARG;
}
BspStatus bsp_spp_capabilities(BspRadio id, BspSppCapabilities *out)
{
    if (!out)
    {
        return BSP_INVALID_ARG;
    }
    BspStatus s = unavailable(id);
    if (s != BSP_UNSUPPORTED)
    {
        return s;
    }
    *out = (BspSppCapabilities){0};
    return BSP_OK;
}
BspStatus bsp_spp_init(BspRadio id, const BspSppConfig *c)
{
    return bsp_spp_config_valid(c) ? unavailable(id) : BSP_INVALID_ARG;
}
BspStatus bsp_spp_query(BspRadio id, BspSppState *out)
{
    return out ? unavailable(id) : BSP_INVALID_ARG;
}
BspStatus bsp_spp_listen(BspRadio id, const BspSppListenConfig *c)
{
    return bsp_spp_listen_valid(c) ? unavailable(id) : BSP_INVALID_ARG;
}
BspStatus bsp_spp_discover(BspRadio id, const uint8_t address[6])
{
    return address ? unavailable(id) : BSP_INVALID_ARG;
}
BspStatus bsp_spp_connect(BspRadio id, const BspSppConnectConfig *c)
{
    return bsp_spp_connect_valid(c) ? unavailable(id) : BSP_INVALID_ARG;
}
BspStatus bsp_spp_disconnect(BspRadio id, BspSppConnection c)
{
    return c.generation ? unavailable(id) : BSP_INVALID_ARG;
}
BspStatus bsp_spp_write(BspRadio id, BspSppConnection c, const void *data, size_t n, uint64_t *op)
{
    return c.generation && data && n && op ? unavailable(id) : BSP_INVALID_ARG;
}
BspStatus bsp_spp_write_result(BspRadio id, BspSppWriteResult *out)
{
    return out ? unavailable(id) : BSP_INVALID_ARG;
}
BspStatus bsp_spp_read(BspRadio id, BspSppConnection c, void *data, size_t capacity, size_t *count)
{
    return c.generation && data && capacity && count ? unavailable(id) : BSP_INVALID_ARG;
}
BspStatus bsp_spp_stop(BspRadio id)
{
    return unavailable(id);
}

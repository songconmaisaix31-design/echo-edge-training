#include "ecw_wifi_net.h"
#include "bsp_request_internal.h"
#define LEASES 4u
static struct
{
    bool bound, available;
    BspRadio radio;
    unsigned index;
    uint64_t operation, epoch, next_ticket;
    EcwBspWifiNetLease *holders[LEASES];
    uint64_t tickets[LEASES];
} owner;
/* All state and token access is under the BSP lock. Native calls never occur. */
static unsigned slot(const EcwBspWifiNetLease *lease)
{
    if (!owner.bound || !lease->active || lease->operation != owner.operation ||
        lease->epoch != owner.epoch || lease->netif_index != owner.index)
    {
        return LEASES;
    }
    for (unsigned i = 0; i < LEASES; ++i)
    {
        if (owner.holders[i] == lease && owner.tickets[i] == lease->ticket)
        {
            return i;
        }
    }
    return LEASES;
}
BspStatus ecw_bsp_wifi_net_publish(BspRadio radio, unsigned index, uint64_t operation,
                                   uint64_t epoch)
{
    if (!index || index > 255u || !operation || !epoch)
    {
        return BSP_INVALID_ARG;
    }
    uintptr_t key = bsp_request_lock();
    BspStatus result = owner.bound ? BSP_BUSY : BSP_OK;
    if (result == BSP_OK && (operation < owner.operation || epoch <= owner.epoch ||
                             (operation == owner.operation && radio != owner.radio)))
    {
        result = BSP_INVALID_STATE;
    }
    if (result == BSP_OK)
    {
        owner.bound = true;
        owner.available = true;
        owner.radio = radio;
        owner.index = index;
        owner.operation = operation;
        owner.epoch = epoch;
    }
    bsp_request_unlock(key);
    return result;
}
BspStatus ecw_bsp_wifi_net_revoke(uint64_t operation, uint64_t epoch)
{
    if (!operation || !epoch)
    {
        return BSP_INVALID_ARG;
    }
    uintptr_t key = bsp_request_lock();
    BspStatus result = owner.bound && owner.operation == operation && owner.epoch == epoch
                           ? BSP_OK
                           : BSP_INVALID_STATE;
    if (result == BSP_OK)
    {
        owner.available = false;
    }
    bsp_request_unlock(key);
    return result;
}
BspStatus ecw_bsp_wifi_net_retire(uint64_t operation, uint64_t epoch)
{
    if (!operation || !epoch)
    {
        return BSP_INVALID_ARG;
    }
    uintptr_t key = bsp_request_lock();
    BspStatus result = operation != owner.operation || epoch != owner.epoch ? BSP_INVALID_STATE
                       : !owner.bound                                       ? BSP_OK
                       : owner.available                                    ? BSP_BUSY
                                                                            : BSP_OK;
    if (result == BSP_OK && owner.bound)
    {
        for (unsigned i = 0; i < LEASES; ++i)
        {
            if (owner.holders[i])
            {
                result = BSP_BUSY;
                break;
            }
        }
        if (result == BSP_OK)
        {
            owner.bound = false;
            owner.index = 0;
        }
    }
    bsp_request_unlock(key);
    return result;
}
BspStatus ecw_bsp_wifi_net_withdraw(void)
{
    uintptr_t key = bsp_request_lock();
    for (unsigned i = 0; i < LEASES; ++i)
    {
        if (owner.holders[i])
        {
            bsp_request_unlock(key);
            return BSP_BUSY;
        }
    }
    owner.available = false;
    owner.bound = false;
    owner.index = 0;
    bsp_request_unlock(key);
    return BSP_OK;
}
BspStatus ecw_bsp_wifi_net_acquire(BspRadio radio, EcwBspWifiNetLease *lease)
{
    if (!lease)
    {
        return BSP_INVALID_ARG;
    }
    uintptr_t key = bsp_request_lock();
    BspStatus result = lease->active                      ? BSP_INVALID_STATE
                       : !owner.bound || !owner.available ? BSP_NOT_READY
                       : radio != owner.radio             ? BSP_INVALID_ARG
                                                          : BSP_OK;
    unsigned i = 0;
    if (result == BSP_OK)
    {
        while (i < LEASES && owner.holders[i])
        {
            ++i;
        }
        if (i == LEASES || owner.next_ticket == UINT64_MAX)
        {
            result = BSP_NO_RESOURCE;
        }
    }
    if (result == BSP_OK)
    {
        owner.holders[i] = lease;
        owner.tickets[i] = ++owner.next_ticket;
        *lease = (EcwBspWifiNetLease){.netif_index = owner.index,
                                      .operation = owner.operation,
                                      .epoch = owner.epoch,
                                      .ticket = owner.tickets[i],
                                      .active = true};
    }
    bsp_request_unlock(key);
    return result;
}
BspStatus ecw_bsp_wifi_net_available(const EcwBspWifiNetLease *lease)
{
    if (!lease)
    {
        return BSP_INVALID_ARG;
    }
    uintptr_t key = bsp_request_lock();
    BspStatus result = slot(lease) == LEASES ? BSP_INVALID_STATE
                       : owner.available     ? BSP_OK
                                             : BSP_NOT_READY;
    bsp_request_unlock(key);
    return result;
}
BspStatus ecw_bsp_wifi_net_release(EcwBspWifiNetLease *lease)
{
    if (!lease)
    {
        return BSP_INVALID_ARG;
    }
    uintptr_t key = bsp_request_lock();
    unsigned i = slot(lease);
    BspStatus result = i == LEASES ? BSP_INVALID_STATE : BSP_OK;
    if (result == BSP_OK)
    {
        owner.holders[i] = NULL;
        owner.tickets[i] = 0;
        *lease = (EcwBspWifiNetLease){0};
    }
    bsp_request_unlock(key);
    return result;
}

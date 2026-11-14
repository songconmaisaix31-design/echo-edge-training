#include "ecw_ble_gatt.h"
#include "ecw_ble_adv_guard.h"
#include "bsp_request_internal.h"
#include "host/ble_att.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
#include "os/os_mbuf.h"
#include <string.h>
#ifdef BSP_ECW_NATIVE_GATT
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#endif

static void uuid_copy(ble_uuid128_t *out, const BspBleUuid *in)
{
    out->u.type = BLE_UUID_TYPE_128;
    for (size_t i = 0; i < 16; ++i)
    {
        out->value[i] = in->bytes[15u - i];
    }
}
static int access_value(uint16_t connection, uint16_t handle, struct ble_gatt_access_ctxt *ctxt,
                        void *arg)
{
    EcwBspBleAttribute *a = arg;
    (void)handle;
    if (!a || !ctxt || !ctxt->om)
    {
        return BLE_ATT_ERR_UNLIKELY;
    }
    uint8_t data[BSP_BLE_EVENT_DATA_MAX];
    uint16_t length = 0;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR)
    {
        uintptr_t key = bsp_request_lock();
        if (!(a->properties & BSP_BLE_READ))
        {
            bsp_request_unlock(key);
            return BLE_ATT_ERR_READ_NOT_PERMITTED;
        }
        length = a->length;
        memcpy(data, a->value, length);
        bsp_request_unlock(key);
        return os_mbuf_append(ctxt->om, data, length) ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
    }
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR)
    {
        return BLE_ATT_ERR_UNLIKELY;
    }
    /* No ownership transfer: NimBLE still releases the incoming mbuf. */
    int rc = ble_hs_mbuf_to_flat(ctxt->om, data, sizeof(data), &length);
    uintptr_t key = bsp_request_lock();
    if (rc)
    {
        a->owner->rejected(a->owner->context);
        bsp_request_unlock(key);
        return rc == BLE_HS_EMSGSIZE ? BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN : BLE_ATT_ERR_UNLIKELY;
    }
    if (!(a->properties & BSP_BLE_WRITE))
    {
        a->owner->rejected(a->owner->context);
        bsp_request_unlock(key);
        return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
    }
    if (length > a->max_length)
    {
        a->owner->rejected(a->owner->context);
        bsp_request_unlock(key);
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    if (!a->owner->write(a->owner->context, connection, a->id, data, length))
    {
        bsp_request_unlock(key);
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    memcpy(a->value, data, length);
    a->length = length;
    bsp_request_unlock(key);
    return 0;
}
BspStatus ecw_bsp_ble_gatt_prepare(EcwBspBleGatt *g, const BspBleService *services, size_t count,
                                   EcwBspBleWriteSink write, EcwBspBleWriteRejected rejected,
                                   void *context)
{
    if (!g || !services || !count || !write || !rejected)
    {
        return BSP_INVALID_ARG;
    }
    if (g->prepared || g->registration_attempted)
    {
        return BSP_BUSY;
    }
    if (count > ECW_BSP_BLE_SERVICES)
    {
        return BSP_NO_RESOURCE;
    }
    size_t total = 0;
    uint16_t ids[ECW_BSP_BLE_CHARACTERISTICS];
    for (size_t s = 0; s < count; ++s)
    {
        if (!services[s].items || !services[s].count)
        {
            return BSP_INVALID_ARG;
        }
        if (services[s].count > ECW_BSP_BLE_CHARACTERISTICS - total)
        {
            return BSP_NO_RESOURCE;
        }
        for (size_t c = 0; c < services[s].count; ++c)
        {
            const BspBleCharacteristic *a = &services[s].items[c];
            if (!a->properties ||
                (a->properties &
                 ~(uint32_t)(BSP_BLE_READ | BSP_BLE_WRITE | BSP_BLE_NOTIFY | BSP_BLE_INDICATE)) ||
                !a->max_length)
            {
                return BSP_INVALID_ARG;
            }
            if (a->max_length > BSP_BLE_EVENT_DATA_MAX)
            {
                return BSP_NO_RESOURCE;
            }
            for (size_t i = 0; i < total; ++i)
            {
                if (ids[i] == a->id)
                {
                    return BSP_INVALID_ARG;
                }
            }
            ids[total++] = a->id;
        }
    }
    /* All validation precedes mutation. Lifecycle owner excludes callbacks here. */
    memset(g, 0, sizeof(*g));
    g->write = write;
    g->rejected = rejected;
    g->context = context;
    g->count = total;
    total = 0;
    for (size_t s = 0; s < count; ++s)
    {
        uuid_copy(&g->uuid[s], &services[s].uuid);
        g->services[s] = (struct ble_gatt_svc_def){.type = BLE_GATT_SVC_TYPE_PRIMARY,
                                                   .uuid = &g->uuid[s].u,
                                                   .characteristics = g->characteristics[s]};
        for (size_t c = 0; c < services[s].count; ++c)
        {
            const BspBleCharacteristic *in = &services[s].items[c];
            EcwBspBleAttribute *a = &g->attributes[total++];
            a->owner = g;
            a->id = in->id;
            a->max_length = in->max_length;
            a->properties = in->properties;
            uuid_copy(&a->uuid, &in->uuid);
            ble_gatt_chr_flags flags = 0;
            if (in->properties & BSP_BLE_READ)
            {
                flags |= BLE_GATT_CHR_F_READ;
            }
            if (in->properties & BSP_BLE_WRITE)
            {
                flags |= BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP;
            }
            if (in->properties & BSP_BLE_NOTIFY)
            {
                flags |= BLE_GATT_CHR_F_NOTIFY;
            }
            if (in->properties & BSP_BLE_INDICATE)
            {
                flags |= BLE_GATT_CHR_F_INDICATE;
            }
            g->characteristics[s][c] = (struct ble_gatt_chr_def){.uuid = &a->uuid.u,
                                                                 .access_cb = access_value,
                                                                 .arg = a,
                                                                 .flags = flags,
                                                                 .val_handle = &a->handle};
        }
    }
    g->prepared = true;
    return BSP_OK;
}
int ecw_bsp_ble_gatt_register(EcwBspBleGatt *g)
{
    if (!g || !g->prepared)
    {
        return BLE_HS_EINVAL;
    }
    if (g->registration_attempted || g->native_detached)
    {
        return BLE_HS_EBUSY;
    }
    g->registration_attempted = true;
    int rc = ble_gatts_count_cfg(g->services);
    if (!rc)
    {
        rc = ble_gatts_add_svcs(g->services);
    }
    g->registered = rc == 0;
    return rc;
}
int ecw_bsp_ble_gatt_activate_on_host(EcwBspBleGatt *g)
{
    if (!g || !g->prepared)
    {
        return BLE_HS_EINVAL;
    }
    if (g->registration_attempted || g->activation_attempted || g->native_detached)
    {
        return BLE_HS_EBUSY;
    }
    if (!ble_hs_synced())
    {
        return BLE_HS_ENOTSYNCED;
    }
    if (ecw_ble_adv_active_from(0))
    {
        return BLE_HS_EBUSY;
    }
    /* Native start's error epilogue can free service memory even for BUSY.
     * Check mutability via reset BEFORE registering, on the sole Host owner;
     * neither GAP admission nor another native GATT owner may race this step. */
    int result = ble_gatts_reset();
    if (result)
    {
        return result;
    }
    g->activation_attempted = true;
    /* reset removes ATT entries but retains the old definition pointer list.
     * Exclusive owner must remove that list before registering a fresh table. */
    ble_gatts_stop();
#ifdef BSP_ECW_NATIVE_GATT
    const struct ble_gatt_svc_def *defaults[] = {ble_svc_gap_definitions(),
                                                 ble_svc_gatt_definitions()};
    for (size_t i = 0; i < sizeof(defaults) / sizeof(defaults[0]); ++i)
    {
        if (!defaults[i])
        {
            return BLE_HS_ENOTSUP;
        }
        result = ble_gatts_count_cfg(defaults[i]);
        if (!result)
        {
            result = ble_gatts_add_svcs(defaults[i]);
        }
        if (result)
        {
            return result; /* Retain partial default registration for explicit stop. */
        }
    }
#endif
    result = ecw_bsp_ble_gatt_register(g);
    if (!result)
    {
        result = ble_gatts_start();
    }
    g->active = result == 0;
    return result;
}
BspStatus ecw_bsp_ble_gatt_set(EcwBspBleGatt *g, uint16_t id, const void *data, size_t n)
{
    if (!g || (!data && n))
    {
        return BSP_INVALID_ARG;
    }
    uintptr_t key = bsp_request_lock();
    BspStatus result = g->prepared ? BSP_INVALID_ARG : BSP_NOT_READY;
    for (size_t i = 0; g->prepared && i < g->count; ++i)
    {
        EcwBspBleAttribute *a = &g->attributes[i];
        if (a->id != id)
        {
            continue;
        }
        if (n > a->max_length)
        {
            result = BSP_INVALID_ARG;
        }
        else
        {
            if (n)
            {
                memcpy(a->value, data, n);
            }
            a->length = (uint16_t)n;
            result = BSP_OK;
        }
        break;
    }
    bsp_request_unlock(key);
    return result;
}
int ecw_bsp_ble_gatt_detach_on_host(EcwBspBleGatt *g)
{
    if (!g || !g->prepared)
    {
        return BLE_HS_EINVAL;
    }
    if (g->native_detached)
    {
        return 0;
    }
    if (g->registration_attempted || g->activation_attempted)
    {
        if (ecw_ble_adv_active_from(0))
        {
            return BLE_HS_EBUSY;
        }
        /* P16 reset checks only advertising slot 0; the guard above checks all.
         * Native reset also refuses active scan/connect procedures/connections.
         * Reset alone retains the service-definition pointer array. stop frees
         * that array and ATT allocation. Caller serializes the two operations
         * with native Host/GAP work; no global BSP IRQ lock spans SDK calls. */
        int rc = ble_gatts_reset();
        if (rc)
        {
            return rc;
        }
        ble_gatts_stop();
    }
    g->registered = false;
    g->active = false;
    g->native_detached = true;
    return 0;
}
void ecw_bsp_ble_gatt_forget_quiescent(EcwBspBleGatt *g)
{
    if (g)
    {
        memset(g, 0, sizeof(*g));
    }
}

/* Managed CS / display plans use the same finite-DMA + SPI END engine.
 * No worker, timer, software SPI, allocation, or progress polling. */
#include "ecw_spi_async.h"
#include "ecw_internal.h"
#include "ecw_sdk.h"
#include "bsp_request_internal.h"
#include <string.h>

typedef struct
{
    const Ecw6700Port *ports[3];
    BspDisplayBinding binding;
    BspDisplaySegment segments[BSP_PLAN_MAX_SEGMENTS];
    BspRequest *request;
    uint64_t generation;
    uint8_t *tx, *rx;
    size_t capacity, segment_count, index, offset, chunk, tx_done, rx_done;
    EcwIoResult outcomes[3];
    BspStatus event_status;
    uint32_t event_native;
    bool advancing, next, in_flight, finished, started;
} SpiPlan;
static SpiPlan plan;

static bool overlaps(const void *a, size_t an, const void *b, size_t bn)
{
    if (!a || !b || !an || !bn)
    {
        return false;
    }
    uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
    return x <= y ? y - x < an : x - y < bn;
}
static void release_ports(const Ecw6700Port **ports, const EcwIoResult *outcomes)
{
    for (size_t i = 0; i < 3; ++i)
    {
        if (ports[i])
        {
            (void)ecw_finish_basic(ports[i]->id, outcomes[i]);
        }
    }
}
static void retire(void *owner)
{
    SpiPlan *p = owner;
    release_ports(p->ports, p->outcomes);
    *p = (SpiPlan){0};
}
static int drive(unsigned index, bool high)
{
    if (!plan.ports[index])
    {
        return 0;
    }
    uintptr_t key = ecw_sdk_lock();
    int raw = ecw_sdk_gpio_write(plan.ports[index]->pins[0], high);
    if (raw)
    {
        plan.outcomes[index] = (EcwIoResult){BSP_IO, raw, true};
        ecw_record_async(plan.ports[index]->id, plan.outcomes[index]);
    }
    ecw_sdk_unlock(key);
    return raw;
}
/* safe is only established by no native start, or pure DMA TC + SPI END.
 * A failed deassert cannot silently release the pin plan. */
static void finish(BspStatus status, int32_t raw, bool safe, BspKind origin)
{
    int cleanup = safe ? drive(1, !plan.binding.cs_active_high) : 0;
    if (cleanup)
    {
        safe = false;
    }
    uintptr_t key = ecw_sdk_lock();
    if (status == BSP_OK && cleanup)
    {
        status = BSP_IO;
        raw = cleanup;
        origin = BSP_GPIO;
    }
    plan.outcomes[0] =
        (EcwIoResult){status, raw, !safe || status == BSP_IO || status == BSP_NOT_READY};
    BspResult result = {
        .error = {.code = status,
                  .native_valid = raw != 0,
                  .native_domain = origin == BSP_GPIO ? 0x45435747u : 0x45430000u | BSP_SPI,
                  .native_code = raw},
        .cleanup_error = {.code = safe      ? BSP_OK
                                  : cleanup ? BSP_IO
                                            : BSP_UNSUPPORTED,
                          .native_valid = cleanup != 0,
                          .native_domain = 0x45435747u,
                          .native_code = cleanup},
        .tx_done = plan.tx_done,
        .rx_done = plan.rx_done,
        .tx_count_valid = status == BSP_OK,
        .rx_count_valid = status == BSP_OK,
        /* GPIO selection may already have occurred before native DMA admission.
         * Do not describe that accepted path as having no hardware effects. */
        .hardware_started = plan.started ? BSP_YES : BSP_UNKNOWN,
        .wire_complete = status == BSP_OK ? BSP_YES : BSP_UNKNOWN,
        .buffers_returned = safe};
    plan.finished = true;
    plan.advancing = false;
    plan.next = false;
    ecw_record_async(plan.ports[0]->id, plan.outcomes[0]);
    BspRequest *request = plan.request;
    uint64_t generation = plan.generation;
    ecw_sdk_unlock(key);
    /* No plan access after publication: caller may reset after notification. */
    (void)bsp_request_publish(request, generation, &result);
}
static void advance(void);
static void completed(void *context, BspStatus status, uint32_t raw)
{
    (void)context;
    uintptr_t key = ecw_sdk_lock();
    if (!plan.request || !plan.in_flight || plan.finished)
    {
        ecw_sdk_unlock(key);
        return;
    }
    plan.started = true;
    plan.in_flight = false;
    plan.event_status = status;
    plan.event_native = raw;
    if (status == BSP_OK)
    {
        BspDisplaySegment *segment = &plan.segments[plan.index];
        if (segment->receive)
        {
            memcpy(segment->receive + plan.offset, plan.rx, plan.chunk);
            plan.rx_done += plan.chunk;
        }
        plan.tx_done += plan.chunk;
        plan.offset += plan.chunk;
        if (plan.offset == segment->length)
        {
            ++plan.index;
            plan.offset = 0;
        }
    }
    plan.next = true;
    ecw_sdk_unlock(key);
    advance();
}
static void advance(void)
{
    uintptr_t key = ecw_sdk_lock();
    if (plan.advancing || plan.finished)
    {
        ecw_sdk_unlock(key);
        return;
    }
    plan.advancing = true;
    ecw_sdk_unlock(key);
    for (;;)
    {
        key = ecw_sdk_lock();
        if (!plan.next)
        {
            plan.advancing = false;
            ecw_sdk_unlock(key);
            return;
        }
        plan.next = false;
        BspStatus event = plan.event_status;
        int32_t raw = (int32_t)plan.event_native;
        bool end = plan.index == plan.segment_count;
        ecw_sdk_unlock(key);
        if (event != BSP_OK)
        {
            finish(event, raw, false, BSP_SPI);
            return;
        }
        if (end)
        {
            finish(BSP_OK, 0, true, BSP_SPI);
            return;
        }
        BspDisplaySegment *segment = &plan.segments[plan.index];
        if (!plan.offset)
        {
            int error = drive(2, segment->data_mode);
            if (error)
            {
                finish(BSP_IO, error, true, BSP_GPIO);
                return;
            }
        }
        plan.chunk = segment->length - plan.offset;
        if (plan.chunk > plan.capacity)
        {
            plan.chunk = plan.capacity;
        }
        memcpy(plan.tx, segment->data + plan.offset, plan.chunk);
        size_t padded = (plan.chunk + 3u) & ~(size_t)3u;
        memset(plan.tx + plan.chunk, 0, padded - plan.chunk);
        key = ecw_sdk_lock();
        plan.in_flight = true;
        ecw_sdk_unlock(key);
        int32_t native = 0;
        BspStatus status = ecw_spi_dma_start(plan.tx, segment->receive ? plan.rx : NULL, plan.chunk,
                                             completed, NULL, &native);
        if (status != BSP_OK)
        {
            plan.in_flight = false;
            /* Busy/stale FIFO/config errors lack a wire-stop proof. OOM happens
             * before either allocated channel starts in the reviewed engine. */
            bool safe =
                status == BSP_NO_RESOURCE || status == BSP_UNSUPPORTED || status == BSP_INVALID_ARG;
            finish(status, native, safe, BSP_SPI);
            return;
        }
        plan.started = true;
        /* An early callback sets next; iterate without recursive submissions.
         * Otherwise return immediately and continue only on the next IRQ. */
    }
}
static BspStatus submit(const BspDisplayBinding *binding, const BspDisplaySegment *segments,
                        size_t count, BspRequest *request, bool display)
{
    if (!ecw_sdk_thread_ready())
    {
        return BSP_INVALID_STATE;
    }
    if (!binding || !segments || !request || !count || count > BSP_PLAN_MAX_SEGMENTS ||
        binding->cs == binding->dc || binding->cs == BSP_PORT_NONE)
    {
        return BSP_INVALID_ARG;
    }
    for (size_t i = 0; i < count; ++i)
    {
        const BspDisplaySegment *s = &segments[i];
        if (!s->data || !s->length || s->length > UINT16_MAX)
        {
            return BSP_INVALID_ARG;
        }
        if (s->receive != s->data && overlaps(s->data, s->length, s->receive, s->length))
        {
            return BSP_UNSUPPORTED;
        }
        if (overlaps(s->data, s->length, request, sizeof(*request)) ||
            overlaps(s->receive, s->length, request, sizeof(*request)))
        {
            return BSP_INVALID_ARG;
        }
    }
    const Ecw6700Port *ports[3] = {0};
    BspStatus status = ecw_claim_spi_plan(binding, display, ports);
    if (status != BSP_OK)
    {
        return status;
    }
    uint8_t *tx, *rx;
    size_t capacity;
    ecw_spi_workspace(&tx, &rx, &capacity);
    if (!tx || !capacity)
    {
        status = BSP_UNSUPPORTED;
    }
    for (size_t i = 0; status == BSP_OK && i < count; ++i)
    {
        if (segments[i].receive && !rx)
        {
            status = BSP_UNSUPPORTED;
            break;
        }
        if (ecw_spi_storage_overlaps(segments[i].data, segments[i].length) ||
            ecw_spi_storage_overlaps(segments[i].receive, segments[i].length))
        {
            status = BSP_INVALID_ARG;
        }
    }
    if (ecw_spi_storage_overlaps(request, sizeof(*request)) ||
        ecw_spi_storage_overlaps(binding, sizeof(*binding)) ||
        ecw_spi_storage_overlaps(segments, count * sizeof(*segments)))
    {
        status = BSP_INVALID_ARG;
    }
    if (status == BSP_OK)
    {
        plan = (SpiPlan){.binding = *binding,
                         .tx = tx,
                         .rx = rx,
                         .capacity = capacity,
                         .segment_count = count,
                         .next = true};
        for (size_t i = 0; i < 3; ++i)
        {
            plan.ports[i] = ports[i];
        }
        memcpy(plan.segments, segments, count * sizeof(*segments));
        status = bsp_request_begin(request, &plan, retire, &plan.generation);
    }
    if (status != BSP_OK)
    {
        ecw_unclaim_spi_plan(ports);
        return status;
    }
    plan.request = request;
    /* Once accepted, all later failures are observable through the request.
     * Rejection before this point has no GPIO/DMA/buffer side effects. */
    int raw = drive(2, segments[0].data_mode);
    if (!raw)
    {
        raw = drive(1, binding->cs_active_high);
    }
    if (raw)
    {
        finish(BSP_IO, raw, true, BSP_GPIO);
    }
    else
    {
        advance();
    }
    bsp_request_end_submit(request);
    return BSP_OK;
}
BspStatus bsp_display_submit(const BspDisplayBinding *binding, const BspDisplaySegment *segments,
                             size_t count, BspRequest *request)
{
    return submit(binding, segments, count, request, true);
}
BspStatus bsp_spi_device_transfer_async(const BspSpiDevice *device, const uint8_t *tx, uint8_t *rx,
                                        size_t count, BspRequest *request)
{
    if (!device)
    {
        return BSP_INVALID_ARG;
    }
    BspDisplayBinding binding = {device->spi, device->cs, BSP_PORT_NONE, device->cs_active_high};
    BspDisplaySegment segment = {false, tx, count, rx};
    return submit(&binding, &segment, 1, request, false);
}
BspStatus ecw_spi_plan_dispatch(size_t *count)
{
    uintptr_t key = ecw_sdk_lock();
    BspRequest *request = plan.request;
    EcwSpiDmaState state = ecw_spi_dma_state();
    bool fault = request && state.faulted && !plan.finished && !plan.advancing;
    ecw_sdk_unlock(key);
    if (fault)
    {
        finish(BSP_IO, (int32_t)state.dma_status, false, BSP_SPI);
    }
    key = ecw_sdk_lock();
    bool deliver = request && request->callback_pending && !request->callback_running;
    if (deliver)
    {
        request->callback_pending = false;
        request->callback_running = true;
    }
    ecw_sdk_unlock(key);
    if (deliver)
    {
        request->notify.done(request, request->notify.user);
        key = ecw_sdk_lock();
        request->callback_running = false;
        ecw_sdk_unlock(key);
        ++*count;
    }
    return BSP_OK;
}

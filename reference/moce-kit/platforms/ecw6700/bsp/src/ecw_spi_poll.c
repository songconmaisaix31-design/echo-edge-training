#include "ecw_spi_poll.h"
/* B02P03/P16 spi.h: transCtrl/cmd/data/ctrl/status/intrEn/intrSt/mux_config. */
enum
{
    TRANS = 0x20,
    CMD = 0x24,
    DATA = 0x2c,
    CTRL = 0x30,
    STATUS = 0x34,
    IRQ_EN = 0x38,
    IRQ_STATUS = 0x3c,
    MUX = 0x78
};
enum
{
    BUSY = 1,
    TX_FULL = 1u << 23,
    RX_COUNT = 31u << 8,
    TX_COUNT = 31u << 16,
    FIFO_RESET = 6,
    RESET = 1,
    END = 1u << 4,
    FIFO_ERROR = 3
};
uint32_t ecw_spi_mmio_read(void *base, unsigned off)
{
    return *(volatile uint32_t *)((volatile uint8_t *)base + off);
}
void ecw_spi_mmio_write(void *base, unsigned off, uint32_t v)
{
    *(volatile uint32_t *)((volatile uint8_t *)base + off) = v;
}
static bool expired(const EcwSpiPollIo *io, uint32_t start, uint32_t budget)
{
    return (uint32_t)(io->time_ms(io->context) - start) >= budget;
}
static EcwSpiPollResult failed(const EcwSpiPollIo *io, BspStatus status, uint32_t native)
{
    /* No DMA/IRQ or queued work owns user memory. Reset is deliberately not
     * reported as successful recovery until a later explicit init proves it.
     * Do not invent a cleanup wait budget or spin beyond the caller's budget. */
    io->write(io->context, IRQ_EN, 0);
    io->write(io->context, CTRL, RESET);
    return (EcwSpiPollResult){status, native, true};
}
EcwSpiPollResult ecw_spi_poll_transfer(const EcwSpiPollIo *io, const uint8_t *tx, uint8_t *rx,
                                       size_t n, uint32_t timeout_ms)
{
    EcwSpiPollResult result = {BSP_INVALID_ARG, 0, false};
    if (!io || !io->read || !io->write || !io->time_ms || !tx || !n || timeout_ms == UINT32_MAX)
    {
        return result;
    }
    if (n > 512)
    {
        result.status = BSP_UNSUPPORTED;
        return result;
    }
    /* Overlap other than identical in-place is not supported: future TX must
     * not be overwritten by earlier RX. Reject before touching hardware. */
    if (rx && rx != tx)
    {
        uintptr_t a = (uintptr_t)tx, b = (uintptr_t)rx;
        if ((a < b ? b - a : a - b) < n)
        {
            result.status = BSP_UNSUPPORTED;
            return result;
        }
    }
    uint32_t start = io->time_ms(io->context);
    if (!timeout_ms)
    {
        result.status = BSP_TIMEOUT;
        return result;
    }
    while (io->read(io->context, STATUS) & BUSY)
    {
        if (expired(io, start, timeout_ms))
        {
            return failed(io, BSP_TIMEOUT, BUSY);
        }
    }
    io->write(io->context, IRQ_EN, 0);
    io->write(io->context, CTRL, FIFO_RESET); /* also clears DMA enable bits */
    while (io->read(io->context, CTRL) & FIFO_RESET)
    {
        if (expired(io, start, timeout_ms))
        {
            return failed(io, BSP_TIMEOUT, FIFO_RESET);
        }
    }
    if (expired(io, start, timeout_ms))
    {
        return failed(io, BSP_TIMEOUT, 0);
    }
    io->write(io->context, IRQ_STATUS, 0xffffffffu); /* W1C stale status */
    io->write(io->context, MUX, 0);                  /* <=512, no extended counters/DMA */
    /* Always clock simultaneous W/R. With rx==NULL discard the received data:
     * RX progress proves clocks occurred even if END is absent with IRQ_EN=0.
     * The owned backend already configures all three SPI pins. No additional
     * pin, DMA, interrupt, task or persistent caller buffer is acquired. */
    io->write(io->context, TRANS, ((uint32_t)(n - 1) << 12) | (uint32_t)(n - 1));
    io->write(io->context, CMD, 0); /* no command/address phase; starts transfer */
    size_t sent = 0, received = 0;
    for (;;)
    {
        if (expired(io, start, timeout_ms))
        {
            return failed(io, BSP_TIMEOUT, 0);
        }
        uint32_t flags = io->read(io->context, IRQ_STATUS);
        if (flags & FIFO_ERROR)
        {
            return failed(io, BSP_IO, flags);
        }
        uint32_t status = io->read(io->context, STATUS);
        /* Interleave FIFO write/read, including during long transfers. P16 SDK
         * writes all TX before RX, which cannot safely sustain full duplex. */
        if (sent < n && !(status & TX_FULL))
        {
            uint32_t word = 0;
            size_t count = n - sent;
            if (count > 4)
            {
                count = 4;
            }
            for (size_t i = 0; i < count; ++i)
            {
                word |= (uint32_t)tx[sent + i] << (8 * i);
            }
            io->write(io->context, DATA, word);
            sent += count;
        }
        status = io->read(io->context, STATUS);
        if (status & RX_COUNT)
        {
            if (received >= n || received >= sent)
            {
                return failed(io, BSP_IO, status);
            }
            uint32_t word = io->read(io->context, DATA);
            size_t count = n - received;
            if (count > 4)
            {
                count = 4;
            }
            for (size_t i = 0; i < count; ++i)
            {
                if (rx)
                {
                    rx[received + i] = (uint8_t)(word >> (8 * i));
                }
            }
            received += count;
        }
        /* Do not require the IRQ END latch in an IRQ-disabled CPU transfer.
         * All RX data must belong to this transaction after its FIFO reset.
         * Also wait for controller idle and both FIFOs empty; !BUSY alone is
         * insufficient before the first clock or while queued TX remains. */
        flags = io->read(io->context, IRQ_STATUS);
        if (flags & FIFO_ERROR)
        {
            return failed(io, BSP_IO, flags);
        }
        status = io->read(io->context, STATUS);
        if (!(status & (BUSY | RX_COUNT | TX_COUNT)))
        {
            if (sent == n && received == n)
            {
                io->write(io->context, IRQ_STATUS, END);
                return (EcwSpiPollResult){BSP_OK, 0, false};
            }
            if (flags & END)
            {
                return failed(io, BSP_IO, flags);
            }
        }
    }
}

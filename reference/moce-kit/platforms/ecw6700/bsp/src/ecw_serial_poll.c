#include "ecw_clock_profile.h"
/* Owned CPU engines. Offsets/bits verified against fixed P16 register headers.
 * No allocation, IRQ/DMA submissions or retained caller pointers. */
#include "ecw_serial_poll.h"
static bool timeout(const EcwRegIo *io, uint32_t start, uint32_t ms)
{
    return (uint32_t)(io->time_ms(io->context) - start) >= ms;
}
static EcwIoResult i2c_fail(const EcwRegIo *io, BspStatus s, uint32_t raw)
{
    io->write(io->context, 0x14, 0); /* IRQ off */
    io->write(io->context, 0x2c, 0); /* disable controller, not physical bus-clear */
    io->write(io->context, 0x28, 5); /* reset; explicit recover must confirm cleanup */
    return (EcwIoResult){s, raw, true};
}
EcwIoResult ecw_i2c_poll(const EcwRegIo *io, uint8_t address, const uint8_t *prefix, size_t np,
                         const uint8_t *tx, size_t nt, uint8_t *rx, size_t nr, uint32_t ms)
{
    if (!io || !io->read || !io->write || !io->time_ms || address > 127 || np > 2 ||
        (!prefix && np) || (!tx && nt) || (!rx && nr) || (!np && !nt && !nr) || nt > 65535 ||
        nr > 65535 || ms == UINT32_MAX)
    {
        return (EcwIoResult){BSP_INVALID_ARG, 0, false};
    }
    /* Auto-NACK occurs at the last receive byte. Do not invent continuation
  * across a 256-byte RX command; reject before hardware. TX can continue. */
    if (nr > 256)
    {
        return (EcwIoResult){BSP_UNSUPPORTED, 0, false};
    }
    if (!ms)
    {
        return (EcwIoResult){BSP_TIMEOUT, 0, false};
    }
    uint32_t start = io->time_ms(io->context), s;
    while ((s = io->read(io->context, 0x18)) & 0x800)
    {
        if (timeout(io, start, ms))
        {
            return (EcwIoResult){BSP_TIMEOUT, s, false};
        }
    }
    io->write(io->context, 0x14, 0);
    io->write(io->context, 0x28, 4);       /* clear FIFO before the transaction */
    io->write(io->context, 0x1c, address); /* unshifted 7-bit address */
    size_t total = np + nt;
    for (unsigned phase = 0; phase < 2; ++phase)
    {
        bool reading = phase == 1;
        size_t length = reading ? nr : total, offset = 0;
        while (offset < length)
        {
            size_t chunk = length - offset;
            if (chunk > 256)
            {
                chunk = 256;
            }
            bool first = offset == 0, stop = (offset + chunk == length) && (reading || !nr);
            uint32_t ctrl = 0x400u | (reading ? 0x100u : 0) | (first ? 0x1800u : 0) |
                            (stop ? 0x200u : 0) | ((uint32_t)chunk & 255u);
            if (timeout(io, start, ms))
            {
                return i2c_fail(io, BSP_TIMEOUT, 0);
            }
            io->write(io->context, 0x18, 0x3f8); /* only W1C flags */
            io->write(io->context, 0x24, ctrl);
            io->write(io->context, 0x28, 1);
            size_t done = 0;
            bool hit = !first;
            for (;;)
            {
                if (timeout(io, start, ms))
                {
                    return i2c_fail(io, BSP_TIMEOUT, 0);
                }
                s = io->read(io->context, 0x18);
                if (s & 0x10)
                {
                    return i2c_fail(io, BSP_IO, s); /* arbitration loss */
                }
                if (s & 8)
                {
                    hit = true;
                }
                if (reading && !(s & 1) && done < chunk)
                {
                    rx[offset + done] = (uint8_t)io->read(io->context, 0x20);
                    ++done;
                    continue;
                }
                if (s & 0x200)
                {
                    if (!hit || done != chunk || (io->read(io->context, 0x24) & 255u) ||
                        (!reading && !(s & 0x400)))
                    {
                        return i2c_fail(io, BSP_IO, s);
                    }
                    if (stop)
                    {
                        while ((s = io->read(io->context, 0x18)) & 0x800)
                        {
                            if (timeout(io, start, ms))
                            {
                                return i2c_fail(io, BSP_TIMEOUT, s);
                            }
                        }
                    }
                    break;
                }
                if (!reading && hit && !(s & 2) && done < chunk)
                {
                    size_t index = offset + done;
                    io->write(io->context, 0x20, index < np ? prefix[index] : tx[index - np]);
                    ++done;
                }
            }
            offset += chunk;
        }
    }
    return (EcwIoResult){BSP_OK, 0, false};
}
EcwIoResult ecw_uart_poll(const EcwRegIo *io, const uint8_t *tx, uint8_t *rx, size_t n, uint32_t ms)
{
    if (!io || !io->read || !io->write || !io->time_ms || (!tx && !rx) || (tx && rx) || !n ||
        n > 65535 || ms == UINT32_MAX)
    {
        return (EcwIoResult){BSP_INVALID_ARG, 0, false};
    }
    if (!ms)
    {
        return (EcwIoResult){BSP_TIMEOUT, 0, false};
    }
    uint32_t start = io->time_ms(io->context), s = 0;
    size_t done = 0;
    for (;;)
    {
        s = io->read(io->context, 0x34);
        if (s & 0x1e)
        {
            return (EcwIoResult){BSP_IO, s, true}; /* OE/PE/FE/BI */
        }
        if (done == n && (!tx || (s & 0x40)))
        {
            return (EcwIoResult){BSP_OK, 0, false};
        }
        if (timeout(io, start, ms))
        {
            return (EcwIoResult){BSP_TIMEOUT, s,
                                 tx != NULL || done != 0}; /* Empty RX is not a fault. */
        }
        if (done < n)
        {
            if (tx && (s & 0x20))
            {
                io->write(io->context, 0x20, tx[done]);
                ++done;
            }
            else if (rx && (s & 1))
            {
                rx[done] = (uint8_t)io->read(io->context, 0x20);
                ++done;
            }
        }
    }
}
BspStatus ecw_pwm_counts(uint32_t rate, uint16_t duty, uint32_t *reload)
{
    if (!reload || !rate || duty > 10000)
    {
        return BSP_INVALID_ARG;
    }
    if (rate > ECW_PIT_INPUT_HZ)
    {
        return BSP_UNSUPPORTED;
    }
    uint32_t period = ECW_PIT_INPUT_HZ / rate;
    if (period > 131072u)
    {
        return BSP_UNSUPPORTED;
    }
    if (!duty || duty == 10000)
    {
        *reload = 0;
        return BSP_OK;
    }
    uint32_t high = (uint32_t)((uint64_t)period * duty / 10000u), low = period - high;
    if (!high || !low || high > 65536u || low > 65536u)
    {
        return BSP_UNSUPPORTED;
    }
    *reload = ((high - 1u) << 16) | (low - 1u);
    return BSP_OK;
}

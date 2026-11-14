/* Interrupt-driven hardware I2C transactions. Each IRQ handles ready data;
 * no loop waits for a status transition and no thread polls to progress IO.
 * Register/flag definitions are pinned to P16 i2c_reg_struct.h.
 */
#include "ecw_i2c_irq.h"
#include "ecw_sdk.h"
#include "i2c_reg_struct.h"
_Static_assert(offsetof(T_I2C_REG_MAP, IntEn) == 0x14 &&
               offsetof(T_I2C_REG_MAP, Status) == 0x18 &&
               offsetof(T_I2C_REG_MAP, Setup) == 0x2c, "Pinned P16 I2C register layout");
enum
{
    INT_EN = 0x14,
    STATUS = 0x18,
    ADDRESS = 0x1c,
    DATA = 0x20,
    CONTROL = 0x24,
    COMMAND = 0x28,
    SETUP = 0x2c,
    W1C_FLAGS = 0x3f8
};
typedef struct
{
    bool active, receive, address_hit, complete, stop, tx_valid;
    const uint8_t *tx;
    uint8_t *rx;
    size_t nt, nr, sent, received, chunk_end;
    EcwI2cIrqDone done;
    void *context;
} I2cTransfer;
static I2cTransfer transfer;

static void issue(bool receive)
{
    transfer.receive = receive;
    transfer.complete = false;
    transfer.stop = false;
    bool first = receive || transfer.sent == 0;
    transfer.address_hit = !first;
    size_t length = receive ? transfer.nr : transfer.nt - transfer.sent;
    if (length > 256)
    {
        length = 256;
    }
    transfer.chunk_end = transfer.sent + length;
    uint32_t control = I2C_CTRL_DATA | ((uint32_t)length & 255u) |
                       (first ? I2C_CTRL_START | I2C_CTRL_ADDR : 0) |
                       (receive ? I2C_CTRL_DIR_RX | I2C_CTRL_STOP : I2C_CTRL_DIR_TX);
    uint32_t interrupts = I2C_IT_ArbLose | I2C_IT_Cmpl | I2C_IT_Stop |
                          (first ? I2C_IT_AddrHit : I2C_IT_FIFOEmpty) |
                          (receive ? I2C_IT_ByteRecv : 0);
    ecw_i2c_irq_write(STATUS, W1C_FLAGS);
    ecw_i2c_irq_write(CONTROL, control);
    ecw_i2c_irq_write(INT_EN, interrupts);
    ecw_i2c_irq_write(COMMAND, I2C_CMD_IssueDataTrans);
}

/* Called with IRQ state locked. Masking + erasing pointers ends all CPU access;
 * this is not a claim that the physical bus is idle after an error.
 */
static I2cTransfer finish(bool failed)
{
    ecw_i2c_irq_write(INT_EN, 0);
    ecw_i2c_irq_mask(true);
    if (failed)
    {
        ecw_i2c_irq_write(SETUP, 0);
    }
    I2cTransfer finished = transfer;
    transfer = (I2cTransfer){0};
    return finished;
}

bool ecw_i2c_irq_active(void)
{
    uintptr_t key = ecw_sdk_lock();
    bool active = transfer.active;
    ecw_sdk_unlock(key);
    return active;
}

BspStatus ecw_i2c_irq_start(uint8_t address, const uint8_t *tx, size_t nt, uint8_t *rx, size_t nr,
                            EcwI2cIrqDone done, void *context)
{
    if (address > 127 || !tx || !rx || !nt || !nr || nt > UINT16_MAX || nr > UINT16_MAX || !done)
    {
        return BSP_INVALID_ARG;
    }
    if (nr > 256)
    {
        return BSP_UNSUPPORTED;
    }
    uintptr_t key = ecw_sdk_lock();
    if (transfer.active || (ecw_i2c_irq_read(STATUS) & I2C_FLAG_BusBusy))
    {
        ecw_sdk_unlock(key);
        return BSP_BUSY;
    }
    if ((ecw_i2c_irq_read(SETUP) & (I2C_SETUP_EN | I2C_SETUP_MASTER)) !=
        (I2C_SETUP_EN | I2C_SETUP_MASTER))
    {
        ecw_sdk_unlock(key);
        return BSP_NOT_READY;
    }
    ecw_i2c_irq_mask(true);
    ecw_i2c_irq_write(INT_EN, 0);
    ecw_i2c_irq_install(ecw_i2c_irq_handler);
    ecw_i2c_irq_write(COMMAND, I2C_CMD_ClearFIFO);
    ecw_i2c_irq_write(ADDRESS, address);
    transfer = (I2cTransfer){
        .active = true, .tx = tx, .rx = rx, .nt = nt, .nr = nr, .done = done, .context = context};
    issue(false);
    ecw_i2c_irq_mask(false);
    ecw_sdk_unlock(key);
    return BSP_OK;
}

void ecw_i2c_irq_handler(void)
{
    uintptr_t key = ecw_sdk_lock();
    if (!transfer.active)
    {
        ecw_sdk_unlock(key);
        return;
    }
    uint32_t status = ecw_i2c_irq_read(STATUS);
    /* Clear only captured edge events before processing FIFO. Events arriving
     * later remain pending; FIFO level sources are not W1C flags.
     */
    ecw_i2c_irq_write(STATUS, status & W1C_FLAGS);
    bool failed = (status & I2C_FLAG_ArbLose) != 0;
    transfer.address_hit |= (status & I2C_FLAG_AddrHit) != 0;
    transfer.complete |= (status & I2C_FLAG_Cmpl) != 0;
    transfer.stop |= (status & I2C_FLAG_Stop) != 0;
    if (!failed && transfer.receive && transfer.address_hit)
    {
        /* Drain only bytes already present. Bound work by requested length;
         * never wait for the next byte or poll an empty FIFO.
         */
        while (transfer.received < transfer.nr && !(ecw_i2c_irq_read(STATUS) & I2C_FLAG_FIFOEmpty))
        {
            transfer.rx[transfer.received++] = (uint8_t)ecw_i2c_irq_read(DATA);
        }
    }
    if (!transfer.receive)
    {
        failed |= transfer.stop; /* No STOP is allowed between TX and RX. */
        if (transfer.complete)
        {
            failed |= !transfer.address_hit || transfer.sent != transfer.chunk_end ||
                      (ecw_i2c_irq_read(CONTROL) & 255u) != 0 || !(status & I2C_FLAG_ACK);
            if (!failed)
            {
                transfer.tx_valid = transfer.sent == transfer.nt;
                issue(transfer.tx_valid); /* Continuation or repeated START, never STOP. */
                ecw_sdk_unlock(key);
                return;
            }
        }
        else if (!failed && transfer.address_hit && transfer.sent < transfer.chunk_end)
        {
            /* One available FIFO write per IRQ, hardware owns the wire timing.
             * FIFOEmpty is enabled only after the address response.
             */
            if (!(status & I2C_FLAG_FIFOFull))
            {
                ecw_i2c_irq_write(DATA, transfer.tx[transfer.sent++]);
            }
            uint32_t interrupts = I2C_IT_ArbLose | I2C_IT_Cmpl | I2C_IT_Stop;
            if (transfer.sent < transfer.chunk_end)
            {
                interrupts |= I2C_IT_FIFOEmpty;
            }
            ecw_i2c_irq_write(INT_EN, interrupts);
        }
    }
    else if (transfer.complete || transfer.stop)
    {
        failed |= !transfer.address_hit;
        if (transfer.complete)
        {
            failed |= transfer.received != transfer.nr || (ecw_i2c_irq_read(CONTROL) & 255u) != 0;
        }
        if (transfer.stop && transfer.received != transfer.nr)
        {
            failed = true;
        }
    }
    bool success = transfer.receive && transfer.complete && transfer.stop && !failed;
    if (success && (ecw_i2c_irq_read(STATUS) & I2C_FLAG_BusBusy))
    {
        failed = true; /* Contradictory completion evidence: never report success. */
        success = false;
    }
    if (failed || success)
    {
        I2cTransfer finished = finish(failed);
        ecw_sdk_unlock(key);
        finished.done(finished.context, failed ? BSP_IO : BSP_OK, failed ? status : 0,
                      finished.tx_valid ? finished.nt : 0, finished.tx_valid, finished.received);
        return;
    }
    ecw_sdk_unlock(key);
}

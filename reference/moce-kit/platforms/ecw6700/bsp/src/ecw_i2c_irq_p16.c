#include "ecw_i2c_irq.h"
#include "chip_memmap.h"
#include "arch_irq.h"
#include "i2c_common.h"

uint32_t ecw_i2c_irq_read(unsigned offset)
{
    switch (offset)
    {
    case 0x18:
        return (uint32_t)drv_i2c_reg_read_status(MEM_BASE_I2C);
    case 0x20:
        return drv_i2c_reg_read_data(MEM_BASE_I2C);
    case 0x24:
        return (uint32_t)drv_i2c_reg_read_ctrl(MEM_BASE_I2C);
    case 0x2c:
        return ((T_I2C_REG_MAP *)(uintptr_t)MEM_BASE_I2C)->Setup;
    default:
        return 0;
    }
}
void ecw_i2c_irq_write(unsigned offset, uint32_t value)
{
    switch (offset)
    {
    case 0x14:
        drv_i2c_reg_write_intEn(MEM_BASE_I2C, value);
        break;
    case 0x18:
        drv_i2c_reg_clear_status(MEM_BASE_I2C, value);
        break;
    case 0x1c:
        drv_i2c_reg_write_addr(MEM_BASE_I2C, (unsigned short)value);
        break;
    case 0x20:
        drv_i2c_reg_write_data(MEM_BASE_I2C, (unsigned char)value);
        break;
    case 0x24:
        drv_i2c_reg_write_ctrl(MEM_BASE_I2C, value);
        break;
    case 0x28:
        drv_i2c_reg_write_cmd(MEM_BASE_I2C, value);
        break;
    case 0x2c:
        drv_i2c_reg_write_setup(MEM_BASE_I2C, value);
        break;
    default:
        break;
    }
}
void ecw_i2c_irq_mask(bool masked)
{
    if (masked)
    {
        arch_irq_mask(VECTOR_NUM_I2C);
    }
    else
    {
        arch_irq_unmask(VECTOR_NUM_I2C);
    }
}
void ecw_i2c_irq_install(void (*handler)(void))
{
    arch_irq_register(VECTOR_NUM_I2C, handler);
}

/* ECW6700F B02P12 SDK platform startup; no module business code. */
#include "config.h"
#include "FreeRTOS.h"
#include "task.h"
#include "oshal.h"
#include "cli.h"
#include "flash.h"
#include "easyflash.h"
#include "PS_common_extern.h"
#include "rf_cal_config.h"
#include "psm_intf_hooks.h"
#include "psm_system.h"
#include "ble_thread.h"
#include "wifi_config.h"
#include "rtc.h"

#if !defined(CONFIG_ECW6700F) || !defined(CONFIG_CLI_UART_1)
#error "This startup requires ECW6700F and the Board UART1 console"
#endif
#if !defined(CONFIG_CPU_CLK_SRC_160M) || !defined(CONFIG_CPU_CLK_FREQ_DIV_0)
#error "Platform startup requires the reviewed CPU160/AHB80/APB40 profile"
#endif
#if defined(CONFIG_CMD_WIFI) || defined(CONFIG_CMD_BLE) || \
    defined(CONFIG_CMD_WIFI_AMT_VERSION) || defined(CONFIG_CMD_BLE_RATE_DEBUG) || \
    defined(CONFIG_BLE_EXAMPLES_NETCFG_SLAVE) || defined(CONFIG_OTA_LOCAL) || \
    defined(CONFIG_OTA_SERVICE) || defined(CONFIG_CMD_PSM) || defined(CONFIG_CMD_SMARTCONFIG)
#error "Disable competing SDK applications in the B02P12 project configuration"
#endif
#if !defined(CONFIG_SYSTEM_WIFI_CTRL) || !CONFIG_BT_NIMBLE_ENABLED
#error "This startup retains the native B02P12 Wi-Fi system and NimBLE Host"
#endif

/* Fixed SDK standalone main uses these entry points; RF/ROM code stays native. */
extern uint8_t ecr_nimble_init(void);
extern void wifi_version_mode_set(uint8_t mode);
extern void rf_calibration_all(void);
extern void project_run(void);

volatile BspStatus project_startup_status = BSP_NOT_READY;

static void project_task(void *argument)
{
    (void)argument;
    project_run();
    /* No automatic teardown of partially initialized hardware or retry. */
    vTaskDelete(NULL);
}

int main(void)
{
    psm_wifi_init_hooks();
    if (component_cli_init(E_UART_SELECT_BY_KCONFIG) != 0)
    {
        return -1;
    }
#if defined(CONFIG_FLASH)
    if (drv_spiflash_init() == 0)
    {
        return -2;
    }
#if defined(CONFIG_NV)
    if (partion_init() != 0 || easyflash_init() != 0)
    {
        return -3;
    }
#endif
#endif
    rf_platform_int();
    ke_init();
    ke_rom_func_init();
    co_rom_func_init();
    rf_cal_config_init();
    ble_main();
    if (ecr_nimble_init() != 0)
    {
        return -4;
    }
    wifi_version_mode_set(1);
    wifi_main();
    rf_calibration_all();
#if defined(CFG_WLAN_COEX)
    rf_pta_config();
#endif
#if defined(CONFIG_PSM_SURPORT)
    psm_wifi_ble_init();
#endif
#if defined(CONFIG_RTC)
    start_calculate_rtc();
#endif
    /* The SDK returns -1 on failure, otherwise a task handle cast to int.
     * Valid RAM handles have bit 31 set; a negative value is not failure.
     * Stack size is bytes; no wifi_conn_main, BLE demo or OTA app. */
    if (os_task_create("moce_app", PROJECT_APP_PRIORITY, PROJECT_APP_STACK_BYTES,
                       project_task, NULL) == -1)
    {
        return -5;
    }
    vTaskStartScheduler();
    return -6;
}

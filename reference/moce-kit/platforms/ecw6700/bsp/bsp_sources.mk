# Include in the application's SDK build scope, before sources are finalized.
# BSP_ECW_ROOT: absolute POSIX path in the SDK build shell (no spaces or drive colon).
# Include in the application's Boards Make.defs scope before CFLAGS/VPATH expansion.
CSRCS += ecw_critical.c
ifndef BSP_ECW_ROOT
$(error BSP_ECW_ROOT must point to the selected ECW BSP source directory)
endif
# MOCE-SYNC: fixed SDK selection | integration/SDK_COMPATIBILITY.md
BSP_ECW_SDK_BASELINE ?= B02P12
ifeq ($(BSP_ECW_SDK_BASELINE),B02P12)
CFLAGS += -DBSP_ECW_SDK_B02P12=1
else ifeq ($(BSP_ECW_SDK_BASELINE),B02P03)
CFLAGS += -DBSP_ECW_SDK_B02P12=0
else
$(error Unknown BSP_ECW_SDK_BASELINE; select B02P03 or B02P12)
endif
# MOCE-SYNC: optional dependencies | BACKENDS.md
BSP_ECW_ENABLE_I2S ?= 1
BSP_ECW_ENABLE_DMA ?= 1
BSP_ECW_ENABLE_RADIO ?= 1
ifneq ($(filter-out 0 1,$(BSP_ECW_ENABLE_I2S) $(BSP_ECW_ENABLE_DMA) $(BSP_ECW_ENABLE_RADIO)),)
$(error BSP_ECW_ENABLE_I2S/DMA/RADIO must be 0 or 1)
endif
ifeq ($(BSP_ECW_ENABLE_I2S)$(BSP_ECW_ENABLE_DMA),10)
$(error I2S requires BSP_ECW_ENABLE_DMA=1; disable I2S too)
endif
ifeq ($(BSP_ECW_GATT_EXCLUSIVE_DATABASE)$(BSP_ECW_ENABLE_RADIO),10)
$(error GATT ownership requires BSP_ECW_ENABLE_RADIO=1)
endif
CFLAGS += -DBSP_ECW_ENABLE_I2S=$(BSP_ECW_ENABLE_I2S) -DBSP_ECW_ENABLE_DMA=$(BSP_ECW_ENABLE_DMA)
# Optional task-context PWM diagnostics; never enable software waveform generation.
BSP_ECW_PWM_DIAGNOSTICS ?= 0
CFLAGS += -DBSP_ECW_PWM_DIAGNOSTICS=$(BSP_ECW_PWM_DIAGNOSTICS)
ifeq ($(BSP_ECW_ENABLE_RADIO),1)
CFLAGS += -DBSP_ECW_NATIVE_WIFI=1 -DBSP_ECW_NATIVE_NET=1 -DBSP_ECW_NATIVE_BLE=1
CSRCS += ecw_ble_service.c ecw_ble_scan.c ecw_ble_advertising.c
# Explicit project grant: BSP owns the entire native GATT database, not the Host task.
BSP_ECW_GATT_EXCLUSIVE_DATABASE ?= 0
ifeq ($(BSP_ECW_GATT_EXCLUSIVE_DATABASE),1)
CFLAGS += -DBSP_ECW_NATIVE_GATT=1
CSRCS += ecw_ble_gatt.c ecw_ble_retire.c ecw_ble_peripheral.c
endif
endif
CSRCS += ecw_uart_rx.c ecw_uart_rx_native.c ecw_i2c_async.c ecw_i2c_irq.c ecw_i2c_irq_p16.c ecw_spi_async.c ecw_spi_plan.c ecw_uart_async.c bsp_spp_unavailable.c
ifeq ($(BSP_ECW_ENABLE_RADIO),1)
CSRCS += ecw_wifi_net.c ecw_net.c ecw_net_job.c ecw_net_socket.c ecw_net_dns.c
CSRCS += ecw_wifi_scan_native.c ecw_wifi_service.c ecw_wifi_ap_p16.c ecw_wifi_control.c ecw_wifi_credentials.c ecw_netif_p16.c
endif
CSRCS += ecw_adc.c ecw_adc_native.c
ifeq ($(BSP_ECW_ENABLE_DMA),1)
CSRCS += ecw_dma_memory.c ecw_spi_dma_p16.c ecw_uart_dma_p16.c
else
CSRCS += ecw_dma_disabled.c
endif
ifeq ($(BSP_ECW_ENABLE_I2S),1)
CSRCS += ecw_audio.c ecw_audio_p16.c
else
CSRCS += ecw_audio_disabled.c
endif
CSRCS += bsp_io_config.c bsp_ecw6700_io_config.c
CSRCS += bsp_radio_unavailable.c ecw_gpio_irq_p16.c bsp_gpio_event_queue.c bsp_wifi_state.c bsp_timer.c bsp_timer_dma_gpio.c ecw_timer_binding.c ecw_timer_p16.c ecw_core.c ecw_io.c ecw_serial_poll.c ecw_basic_p16.c ecw_pwm_ccu6.c ecw_sdk_p16.c ecw_spi_p16.c ecw_spi_poll.c ecw_unsupported.c bsp_request.c bsp_touch.c
VPATH += :$(BSP_ECW_ROOT)/src
INCPATHS += $(BSP_ECW_ROOT)/include $(BSP_ECW_ROOT)/src
# Board board.c is selected once by application composition, never by BSP.

# Defaults to the prepared SDK matching the selected baseline shipped beside this BSP.
# Legacy variable remains accepted so old project integration need not rename it.
ifndef BSP_ECW_SDK_ROOT
ifdef BSP_ECW_P16_ROOT
BSP_ECW_SDK_ROOT := $(BSP_ECW_P16_ROOT)
else ifeq ($(BSP_ECW_SDK_BASELINE),B02P12)
BSP_ECW_SDK_ROOT := $(abspath $(BSP_ECW_ROOT)/../../../sdk/ECW6700FSDK_V1.0.0B02P12/code)
else
BSP_ECW_SDK_ROOT := $(abspath $(BSP_ECW_ROOT)/../../../sdk/ECW6700FSDK_V1.0.0B02P03/code)
endif
endif
BSP_ECW_P16_ROOT ?= $(BSP_ECW_SDK_ROOT)
ifneq ($(abspath $(BSP_ECW_P16_ROOT)),$(abspath $(BSP_ECW_SDK_ROOT)))
$(error BSP_ECW_SDK_ROOT and legacy BSP_ECW_P16_ROOT must select the same reviewed SDK)
endif
ifeq ($(BSP_ECW_SDK_BASELINE),B02P12)
INCPATHS += $(BSP_ECW_SDK_ROOT)/drivers/spi
endif
ifeq ($(BSP_ECW_ENABLE_I2S),1)
ifeq ($(wildcard $(BSP_ECW_P16_ROOT)/drivers/i2s/i2s_ll.h),)
$(error Set BSP_ECW_SDK_ROOT to the reviewed $(BSP_ECW_SDK_BASELINE) code directory; run integration/check_sdk.py --baseline $(BSP_ECW_SDK_BASELINE) before building)
endif
INCPATHS += $(BSP_ECW_P16_ROOT)/drivers/i2s $(BSP_ECW_P16_ROOT)/include/drivers/i2s
# SDK drivers/Make.defs must compile the prepared dma.c exactly once. The
# original DMA header/library lacks drv_dma_event_register; do not mix versions.
# Set to 1 only if the application already links this exact LL source.
ifneq ($(BSP_ECW_I2S_LL_PROVIDED),1)
CSRCS += i2s_ll.c
VPATH += :$(BSP_ECW_P16_ROOT)/drivers/i2s
endif

endif
ifeq ($(BSP_ECW_ENABLE_DMA),1)
INCPATHS += $(BSP_ECW_P16_ROOT)/include/drivers/dma
endif
INCPATHS += $(BSP_ECW_P16_ROOT)/drivers/gpio
INCPATHS += $(BSP_ECW_P16_ROOT)/drivers/i2c $(BSP_ECW_P16_ROOT)/drivers/uart $(BSP_ECW_P16_ROOT)/drivers/pit

INCPATHS += $(BSP_ECW_P16_ROOT)/include/drivers/ccu6
INCPATHS += $(BSP_ECW_P16_ROOT)/include/drivers/pit
# P16 drivers/Make.defs already includes pit_timer.c and pit_common.c; link once.

ifeq ($(BSP_ECW_ENABLE_RADIO),1)
INCPATHS += $(BSP_ECW_P16_ROOT)/include/components/wifi_crtl $(BSP_ECW_P16_ROOT)/components/wpa/wpa_supplicant $(BSP_ECW_P16_ROOT)/components/wpa/src/utils

INCPATHS += $(BSP_ECW_P16_ROOT)/components/ble/nimble/nimble/host/include $(BSP_ECW_P16_ROOT)/components/ble/nimble/nimble/host/src $(BSP_ECW_P16_ROOT)/components/ble/nimble/nimble/include $(BSP_ECW_P16_ROOT)/components/ble/nimble/porting/nimble/include $(BSP_ECW_P16_ROOT)/components/ble/port $(BSP_ECW_P16_ROOT)/include/PS/ble
INCPATHS += $(BSP_ECW_P16_ROOT)/components/ble/nimble/porting/npl/freertos/include

INCPATHS += $(BSP_ECW_P16_ROOT)/components/wpa/src

ifeq ($(BSP_ECW_GATT_EXCLUSIVE_DATABASE),1)
INCPATHS += $(BSP_ECW_SDK_ROOT)/components/ble/nimble/nimble/host/services/gap/include
INCPATHS += $(BSP_ECW_SDK_ROOT)/components/ble/nimble/nimble/host/services/gatt/include
endif

endif
# Bounded status synchronization after actual SPI END + DMA TC; not a time unit.
BSP_ECW_SPI_END_STATUS_READ_LIMIT ?= 64
CFLAGS += -DBSP_ECW_SPI_END_STATUS_READ_LIMIT=$(BSP_ECW_SPI_END_STATUS_READ_LIMIT)

# Explicit project opt-in; old projects do not require the new SDK extension.
BSP_ECW_ADC_CHECKED ?= 0
CFLAGS += -DBSP_ECW_ADC_CHECKED=$(BSP_ECW_ADC_CHECKED)
INCPATHS += $(BSP_ECW_SDK_ROOT)/include/drivers/adc

# Native system facade uses the real generated RTC/PSM configuration; never
# manufacture CONFIG_RTC or take ownership of the SDK's IRQ/calibration service.
BSP_ECW_SYSTEM_EXTENSIONS ?= 0
ifeq ($(BSP_ECW_SYSTEM_EXTENSIONS),1)
CFLAGS += -DBSP_ECW_SYSTEM_EXTENSIONS=1
CSRCS += ecw_system_native.c
INCPATHS += $(BSP_ECW_SDK_ROOT)/include/drivers/rtc $(BSP_ECW_SDK_ROOT)/include/psm
INCPATHS += $(BSP_ECW_SDK_ROOT)/Boards/ecw6700f/common/include
endif

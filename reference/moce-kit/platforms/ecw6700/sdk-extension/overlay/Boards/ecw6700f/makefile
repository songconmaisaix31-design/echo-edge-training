.PHONY:all FORCE

include $(TOPDIR)/Make.defs


###########################################
#
#			extra objs and libs 
#
###########################################

LIBPATHS				:= -L$(OUTPUTDIR)/libs -L$(BOARD_DIR)/library -L$(BOARD_COMMON_DIR)/library/whole_archive
LINK_LIBS				:= $(notdir $(wildcard $(OUTPUTDIR)/libs/*.a))
BOARD_LIBS				:= $(notdir $(wildcard $(BOARD_DIR)/library/*.a))
BOARD_COMMON_WA_LIBS	:= $(notdir $(wildcard $(BOARD_COMMON_DIR)/library/whole_archive/*.a))
ifneq ($(CUSTOM_LIBPATH),)
LIBPATHS                += -L$(CUSTOM_LIBPATH)
LINK_LIBS               += $(notdir $(wildcard $(CUSTOM_LIBPATH)/*.a))
endif
LINK_LIBGROUPS			:= $(patsubst lib%.a,-l%,$(LINK_LIBS)) $(patsubst lib%.a,-l%,$(BOARD_LIBS)) $(patsubst lib%.a,-l%,$(BOARD_COMMON_WA_LIBS))

EXTRA_LIBPATHS			?= $(BOARD_COMMON_DIR)/library
EXTRA_LIBS				:= $(notdir $(wildcard $(EXTRA_LIBPATHS)/*.a))
EXTRA_LINK_LIBGROUPS	:= $(patsubst lib%.a,-l%,$(EXTRA_LIBS))

# dboot/sboot 优先从 ref_bin 查找，找到则只用 ref_bin，找不到回退到 common_bin
ifneq ($(wildcard $(BOARD_DIR)/ref_bin/*dboot*.bin $(BOARD_DIR)/ref_bin/*sboot*.bin),)
BOOT_COMMON_DIR := $(BOARD_DIR)
else
BOOT_COMMON_DIR := $(BOARD_COMMON_DIR)
endif


###########################################
#
#		OTA
#
###########################################

ifdef CONFIG_OTA_MODE_STRING
	OTA_CONFIG_TYPE := $(patsubst "%",%,$(CONFIG_OTA_MODE_STRING))
else
	OTA_CONFIG_TYPE := "OTA_COMPRESSION_UPDATE"
endif


###########################################
#
#		generate ld script
#
###########################################
ifeq ($(CONFIG_LD_USE_TCM_IRAM_FLASH),y)
	LD_SCRIPTS_FILE:=$(BOARD_COMMON_DIR)/ld_ram_flashXIP.script.S
else ifeq ($(CONFIG_LD_USE_TCM_IRAM),y)
	LD_SCRIPTS_FILE:=$(BOARD_COMMON_DIR)/ld_ram.script.S
else ifeq ($(CONFIG_LD_USE_TCM_IRAM_FLASH_PSRAM),y)
	LD_SCRIPTS_FILE:=$(BOARD_COMMON_DIR)/ld_ram_psramXIP.script.S
else
	LD_SCRIPTS_FILE:=$(LD_SCRIPTS)
endif

LD_SCRIPT_CPPFLAGS := $(AFLAGS)
ifneq ($(wildcard $(BOARD_DIR)/include),)
	LD_SCRIPT_CPPFLAGS := -I"$(BOARD_DIR)/include" $(LD_SCRIPT_CPPFLAGS)
endif

$(LDSCRIPT):$(LD_SCRIPTS_FILE) FORCE
	$(Q) echo --CC_LD--:$< to $@
ifeq ($(OS_TYPE_CFG),Windows)
	$(file >$(LDSCRIPT).rsp,$(call win_rsp,$(LD_SCRIPT_CPPFLAGS)))
	$(Q) $(CC) -E -nostdinc -P @$(LDSCRIPT).rsp $< -o $@
else
	$(Q) $(CC) -E -nostdinc -P $(LD_SCRIPT_CPPFLAGS) $< -o $@
endif


###########################################
#
#		Safety Related
#
###########################################

BIN_TARGE:=$(OUTPUTDIR)/ECW6700F_$(APP_NAME)_cpu_$(XIP_ADDR_HEX).bin

include $(TOPDIR)/scripts/iram_check.mk


###########################################
#
#		generate_results: link objs and libs
#
###########################################

TARGET				:= $(OUTPUTDIR)/$(APP_NAME)
OUTPUT_EXT			:= .elf
TRANSPORT_OUTBIN	:= $(OUTPUTDIR)/ECW6700F_$(APP_NAME)_connect_data.bin
PARTITION_CPU_ADDR  := $(shell printf "0x%x" $$(($(XIP_ADDR_HEX) - $(TZSYS_SIZE))))
	
$(TARGET)$(OUTPUT_EXT): $(LDSCRIPT) FORCE
	$(LD) $(LIBPATHS) -L$(EXTRA_LIBPATHS) $(LDSTARTGROUP) $(LINK_LIBGROUPS) $(LDENDGROUP) --start-group $(EXTRA_LINK_LIBGROUPS) --end-group $(ARCHSCRIPT) $(LDFLAGS) -o $@ -Map=$(TARGET).map
	$(Q) $(OBJDUMP) -d -S $@ > $(TARGET).lst
#	$(Q) $(NM) -S --demangle --size-sort -s -r $@ > $(TARGET).nm
	$(Q) $(NM) -n $@ > $(TARGET).nm
ifeq ($(IRAM_CHECK),1)
	$(Q) python3 $(TOPDIR)/scripts/check_iram_calls.py $@ \
		--objdump $(OBJDUMP) \
		--lst $(TARGET).lst \
		--iram-section $(IRAM_CHECK_SECTIONS) \
		--entry $(IRAM_CHECK_ENTRIES)
endif
	$(Q) $(READELF) -e -g -t $@ > $(TARGET).readelf
	$(Q) $(OBJCOPY) -O binary -j .xip $@ $(TARGET).flash

ifeq ($(CONFIG_OUTPUT_VERSION_TRANSPORT_ELF),y)
	$(Q) $(OBJCOPY) -O binary -j .text -j .data $@ $(TARGET).bin
	$(Q) $(OBJCOPY) -O binary -j .text_iram $@ $(TARGET)_iram.bin
	$(Q) ls -l $(TARGET).bin | awk '{printf "%08d", $$5}' > $(TARGET)_single.bin
	$(Q) cat $(TARGET).bin >> $(TARGET)_single.bin
	$(Q) ls -l $(TARGET)_iram.bin | awk '{printf "%08d", $$5}' >> $(TARGET)_single.bin
	$(Q) cat $(TARGET)_iram.bin >> $(TARGET)_single.bin
endif #CONFIG_OUTPUT_VERSION_TRANSPORT_ELF

ifdef CONFIG_ATE
	pwd
	$(Q) echo "################ generate $(OUT_PATH)$(TARGET) ################"
	python3	 $(TOPPATH)/scripts/bin/dboot_crc32_tool.py $(TARGET).bin
	$(Q) $(TOPPATH)/scripts/bin/bootrom_bin2HEX/bin2hex_new.pl 32 $(TARGET)_crc.bin $(TARGET)_crc.rom_hex
	rm -rf $(TARGET).bin
endif
	
ifeq ($(CONFIG_OUTPUT_VERSION_STANDALONE_ELF),y)
	$(Q) echo "################ generate ECW6700F_$(APP_NAME)_cpu_$(XIP_ADDR_HEX).bin ################"
	$(Q) $(TOPDIR)/scripts/bin/eswin_tool/eswin_tool$(HOSTEXEEXT) elf2image $@ -c $(CONFIG_DATA_CHECK_AT_STARTUP) -t xip -k $(TOPDIR)/scripts/bin/eswin_tool/ecdsa_private.key -o $(OTA_CONFIG_TYPE) -v $(REL_V) -p $(XIP_ADDR) -b $(BOARD_DIR) $(BIN_TARGE)
	rm -rf $(TARGET).bin
endif

ifeq ($(CONFIG_TZ_INFAC),y)
	$(Q) $(TOPDIR)/scripts/bin/eswin_tool/eswin_tool$(HOSTEXEEXT) marge_s_ns -a $(filter-out %.aes.cbc.bin,$(wildcard $(BOARD_DIR)/*_tzsys_*.bin $(BOARD_DIR)/*/*_tzsys_$(PARTITION_CPU_ADDR).bin)) -c $(BIN_TARGE) -k $(TOPDIR)/scripts/bin/eswin_tool/ecdsa_private.key -z $(TZSYS_SIZE) $(OUTPUTDIR)/ECW6700F_$(APP_NAME)_tzsys_cpu_$(PARTITION_CPU_ADDR).bin
	$(Q) rm -rf $(BIN_TARGE)
endif #CONFIG_TZ_INFAC

ifneq ($(wildcard $(OUTPUTDIR)/libs/libeswin_sdk_version.a),)
	$(Q) $(TOPDIR)/scripts/bin/eswin_tool/eswin_tool$(HOSTEXEEXT) makeone_partition -m $(BOOT_COMMON_DIR) -b $(BOARD_DIR) -o $(OUTPUTDIR) -a SDK$(shell grep -az ECW6700F_ $(OUTPUTDIR)/libs/libeswin_sdk_version.a | sed 's/ECW6700F_//')_REL$(REL_V) -d 0x80048000 -c ECW6700F
else
	$(Q) $(TOPDIR)/scripts/bin/eswin_tool/eswin_tool$(HOSTEXEEXT) makeone_partition -m $(BOOT_COMMON_DIR) -b $(BOARD_DIR) -o $(OUTPUTDIR) -a SDK$(shell grep -az ECW6700F_ $(BOARD_COMMON_DIR)/library/libeswin_sdk_version.a | sed 's/ECW6700F_//')_REL$(REL_V) -d 0x80048000 -c ECW6700F
endif

ifeq ($(OS_TYPE),Linux)
ifeq ("$(OTA_CONFIG_TYPE)","OTA_COMPRESSION_UPDATE")
	$(Q) echo "################ generate $(OUTPUTDIR)/ECW6700F_$(APP_NAME)_Compress_ota_package.bin ################"
	$(Q) $(TOPDIR)/scripts/bin/ota_tool/otatool$(HOSTEXEEXT) -m cmz -p $(BOARD_DIR)/ref_bin/*Partition*.bin --new $(OUTPUTDIR)/ECW6700F_$(APP_NAME)_*cpu_*.bin -o $(OUTPUTDIR)/ECW6700F_$(APP_NAME)_Compress_ota_package.bin
endif # OTA
endif
	
all: $(TARGET)$(OUTPUT_EXT) FORCE
	$(Q) echo "################ generate $(TARGET)$(OUTPUT_EXT) ################"

.PHONY:all FORCE

include $(TOPDIR)/Make.defs


###########################################
#
#			directory path
#
###########################################

CURRENT_MAKEFILE			:= $(CURDIR)
CURRENT_MAKEFILE_SUB_TOP	:= $(patsubst $(TOPDIR)/%,%,$(CURRENT_MAKEFILE))
CURRENT_LIBA_NAME			:= $(subst /,_,$(CURRENT_MAKEFILE_SUB_TOP))
OUTPUTDIR_OBJS				:= $(OUTPUTDIR)/objs/$(CURRENT_MAKEFILE_SUB_TOP)
OUTPUTDIR_LIB				:= $(OUTPUTDIR)/libs

LIBTARGET := lib$(CURRENT_LIBA_NAME).a

LOG_MOD_HEADER = $(TOPDIR)/include/components/cmd/auto_generate_log_common.h
# 短名优先策略：先尝试最后一级目录名，找不到再用全路径名
MODULE_SHORT_NAME = $(shell basename $(CURRENT_MAKEFILE))
MODULE_FULL_NAME = $(shell echo $(CURRENT_LIBA_NAME) | tr 'a-z' 'A-Z' | sed 's/[^A-Z0-9]/_/g')
MODULE_SHORT_ENUM = LM_$(shell echo $(MODULE_SHORT_NAME) | tr 'a-z' 'A-Z' | sed 's/[^A-Z0-9]/_/g')
MODULE_FULL_ENUM = LM_$(MODULE_FULL_NAME)
# 优先匹配短名，找不到则回退到全名
CURRENT_LIB_MODULE_ID := $(shell \
    id=$$(grep -E "$(MODULE_SHORT_ENUM)[[:space:]]*=[[:space:]]*[0-9]+" $(LOG_MOD_HEADER) 2>/dev/null | sed -E 's/.*=[[:space:]]*([0-9]+).*/\1/'); \
    if [ -z "$$id" ]; then \
        id=$$(grep -E "$(MODULE_FULL_ENUM)[[:space:]]*=[[:space:]]*[0-9]+" $(LOG_MOD_HEADER) 2>/dev/null | sed -E 's/.*=[[:space:]]*([0-9]+).*/\1/'); \
    fi; \
    echo $${id:-0})

# 提取模块编译等级: 先检查是否使用全局等级，否则使用模块独立等级
MODULE_LOG_PREFIX = LOG_$(shell echo $(CURRENT_LIBA_NAME) | tr 'a-z' 'A-Z' | sed 's/[^A-Z0-9]/_/g')
CURRENT_LIB_COMPILE_LEVEL := $(shell \
    if grep -qE "CONFIG_$(MODULE_LOG_PREFIX)_USE_GLOBAL[[:space:]]+1" $(KCONFIG_AUTOHEADER) 2>/dev/null || \
       ! grep -qE "CONFIG_$(MODULE_LOG_PREFIX)_USE_GLOBAL" $(KCONFIG_AUTOHEADER) 2>/dev/null; then \
        grep -E "^\#define CONFIG_LOG_GLOBAL_COMPILE_LEVEL[[:space:]]+" $(KCONFIG_AUTOHEADER) | awk '{print $$3}' || echo 4; \
    else \
        grep -E "^\#define CONFIG_$(MODULE_LOG_PREFIX)_COMPILE_LEVEL[[:space:]]+" $(KCONFIG_AUTOHEADER) | awk '{print $$3}' || echo 4; \
    fi)


###########################################
#
#		generate lib version
#
###########################################

LIBVERSION = libeswin_sdk_version.a
version_builtin = $(shell if [ -f $(BOARD_COMMON_DIR)/eswin_sdk_version.c ]; then echo "y"; else echo "n"; fi;)

dir_git = $(if $(wildcard $(TOPDIR)/.git),y,)
ifeq ($(dir_git),y)
version_git = $(shell git log --oneline -1 | cut -d' ' -f1 2>/dev/null)
version_git_full = $(version_git)$(shell if [ -n "`git diff-index --name-only HEAD | head -1`" ]; then echo "-dirty"; fi;)
$(BOARD_COMMON_DIR)/eswin_sdk_version.c_CFLAGS = -DGIT_TAG='"($(version_git_full))"'
else
$(BOARD_COMMON_DIR)/eswin_sdk_version.c_CFLAGS = -DGIT_TAG=''
endif

ifeq ($(version_builtin), y)
$(OUTPUTDIR_LIB)/$(LIBVERSION): $(BOARD_COMMON_DIR)/eswin_sdk_version.c FORCE
	$(Q) echo "Compile eswin_sdk_version.c"
	$(Q) -mkdir -p $(dir $@)
	$(Q) -mkdir -p $(OUTPUTDIR_OBJS)
	$(call COMPILE, $<, $(OUTPUTDIR_OBJS)/eswin_sdk_version.o)
	$(call ARCHIVE, $@, $(OUTPUTDIR_OBJS)/eswin_sdk_version.o)
else
$(OUTPUTDIR_LIB)/$(LIBVERSION):
	$(Q) echo "No eswin_sdk_version.c"
endif


###########################################
#
#	compile flag
#
###########################################

include $(BOARD_DIR)/Make.defs
include $(BOARD_COMMON_DIR)/Make.defs

COMMON_COMPILE_FLAGS := -DLIB_A_NAME='"$(CURRENT_LIBA_NAME)"' -DLIB_A_MODULE_ID=$(CURRENT_LIB_MODULE_ID) -DLIB_A_COMPILE_LEVEL=$(CURRENT_LIB_COMPILE_LEVEL)
AFLAGS	:= $(COMMON_COMPILE_FLAGS) $(foreach incpath, $(INCPATHS), -I "$(incpath)") \
			$(foreach incpath, $(TOP_INCPATHS), -I "$(incpath)") $(AFLAGS)
CFLAGS	:= $(COMMON_COMPILE_FLAGS) $(foreach incpath, $(INCPATHS), -I "$(incpath)") \
			$(foreach incpath, $(TOP_INCPATHS), -I "$(incpath)") $(CFLAGS)
CPPFLAGS:= $(COMMON_COMPILE_FLAGS) $(foreach incpath, $(INCPATHS), -I "$(incpath)") \
			$(foreach incpath, $(TOP_INCPATHS), -I "$(incpath)") $(CPPFLAGS)

ABSOLUTE_ASRCS			:= $(filter /%, $(ASRCS))
RELATIVE_ASRCS			:= $(filter-out /%, $(ASRCS))
FOUND_RELATIVE_ASRCS	:= $(foreach dir, $(subst :, ,$(VPATH)), \
							$(foreach file, $(RELATIVE_ASRCS), $(wildcard $(dir)/$(file))))
ASRCS					:= $(patsubst $(CURRENT_MAKEFILE)/%,%,$(ABSOLUTE_ASRCS)) $(FOUND_RELATIVE_ASRCS)

ABSOLUTE_CSRCS			:= $(filter /%, $(CSRCS))
RELATIVE_CSRCS			:= $(filter-out /%, $(CSRCS))
FOUND_RELATIVE_CSRCS	:= $(foreach dir, $(subst :, ,$(VPATH)), \
							$(foreach file, $(RELATIVE_CSRCS), $(wildcard $(dir)/$(file))))
CSRCS					:= $(patsubst $(CURRENT_MAKEFILE)/%,%,$(ABSOLUTE_CSRCS)) $(FOUND_RELATIVE_CSRCS)

ABSOLUTE_CPPSRCS		:= $(filter /%, $(CPPSRCS))
RELATIVE_CPPSRCS		:= $(filter-out /%, $(CPPSRCS))
FOUND_RELATIVE_CPPSRCS	:= $(foreach dir, $(subst :, ,$(VPATH)), \
							$(foreach file, $(RELATIVE_CPPSRCS), $(wildcard $(dir)/$(file))))
CPPSRCS					:= $(patsubst $(CURRENT_MAKEFILE)/%,%,$(ABSOLUTE_CPPSRCS)) $(FOUND_RELATIVE_CPPSRCS)

# Opt-in build-only fix; default keeps legacy object paths/rules unchanged.
ifeq ($(BSP_ECW_ISOLATE_BOARD_OBJECTS),1)
ifneq ($(findstring __moce_up__/,$(ASRCS) $(CSRCS) $(CPPSRCS)),)
$(error __moce_up__ is reserved for isolated Board object paths)
endif
# Encode parent components instead of traversing outside the project output.
moce_board_object = $(OUTPUTDIR_OBJS)/$(subst ../,__moce_up__/,$(basename $(1))).o
AOBJS := $(foreach source,$(ASRCS),$(call moce_board_object,$(source)))
COBJS := $(foreach source,$(CSRCS),$(call moce_board_object,$(source)))
CPPOBJS := $(foreach source,$(CPPSRCS),$(call moce_board_object,$(source)))
else
AOBJS	:= $(addprefix $(OUTPUTDIR_OBJS)/,$(ASRCS:.S=.o))
COBJS	:= $(addprefix $(OUTPUTDIR_OBJS)/,$(CSRCS:.c=.o))
CPPOBJS := $(addprefix $(OUTPUTDIR_OBJS)/,$(CPPSRCS:.cpp=.o))
endif

OBJS	:= $(AOBJS) $(COBJS) $(CPPOBJS)


###########################################
#
#		generate lib
#
###########################################

$(OUTPUTDIR_LIB)/$(LIBTARGET): $(OBJS)
#	$(Q) echo "MAKEDEFS:"$(MAKEDEFS)
	$(Q) -mkdir -p $(dir $@)
	$(call ARCHIVE, $@, $(OBJS))


###########################################
#
#		generate objs
#
###########################################

ifeq ($(BSP_ECW_ISOLATE_BOARD_OBJECTS),1)
# Archives identify members by basename, so reject collisions explicitly.
ifneq ($(words $(OBJS)),$(words $(sort $(notdir $(OBJS)))))
$(error Duplicate Board object basename; use distinct source names)
endif
# Keep each source as its explicit prerequisite while changing only output paths.
define moce_board_rule
$(call moce_board_object,$(1)): $(1) $(BOARD_DIR)/Make.defs $(KCONFIG_AUTOHEADER)
	$$(Q) -mkdir -p $$(dir $$@)
	$$(call $(2),$$<,$$@)
endef
$(foreach source,$(ASRCS),$(eval $(call moce_board_rule,$(source),ASSEMBLE)))
$(foreach source,$(CSRCS),$(eval $(call moce_board_rule,$(source),COMPILE)))
$(foreach source,$(CPPSRCS),$(eval $(call moce_board_rule,$(source),COMPILE_CPP)))
-include $(OBJS:.o=.d)
else
$(AOBJS): $(OUTPUTDIR_OBJS)/%.o: %.S
	$(Q) -mkdir -p $(dir $@)
	$(call ASSEMBLE, $<, $@)

$(COBJS): $(OUTPUTDIR_OBJS)/%.o: %.c
	$(Q) -mkdir -p $(dir $@)
	$(call COMPILE, $<, $@)

$(CPPOBJS): $(OUTPUTDIR_OBJS)/%.o: %.cpp
	$(Q) -mkdir -p $(dir $@)
	$(call COMPILE_CPP, $<, $@)

endif


all: $(OUTPUTDIR_LIB)/$(LIBTARGET) $(OUTPUTDIR_LIB)/$(LIBVERSION)


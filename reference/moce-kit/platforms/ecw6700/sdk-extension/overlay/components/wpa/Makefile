.PHONY:all

include $(TOPDIR)/Make.defs


###########################################
#
#			directory path
#
###########################################

# WAPI and KVR generate standalone sealed libraries.
SUBDIRS := $(filter-out $(CURDIR)/src/wapi $(CURDIR)/kvr,$(call find_dirs,$(CURDIR)))

CURRENT_MAKEFILE			:= $(realpath $(shell echo $(CURDIR) | sed -e 's/ /\\ /g'))
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
#	include Make.defs, get MAKEDEFS
#
###########################################

ASRCS	:=
CSRCS	:=
CPPSRCS :=
VPATH	:=
INCPATHS:=
MAKEDEFS:=

$(foreach dir, $(SUBDIRS), $(eval $(call MAKEDEFS_template,$(dir))))

export WPA_SUPPLICANT_ABI_CFLAGS := $(filter-out -D,$(filter -D%,$(CFLAGS)))

VPATH	+= :.


###########################################
#
#	compile flag
#
###########################################
COMMON_COMPILE_FLAGS := -DLIB_A_NAME='"$(CURRENT_LIBA_NAME)"' -DLIB_A_MODULE_ID=$(CURRENT_LIB_MODULE_ID) -DLIB_A_COMPILE_LEVEL=$(CURRENT_LIB_COMPILE_LEVEL)
AFLAGS	:= $(COMMON_COMPILE_FLAGS) $(foreach incpath, $(INCPATHS), -I "$(incpath)") \
			$(foreach incpath, $(TOP_INCPATHS), -I "$(incpath)") $(AFLAGS)
CFLAGS	:= $(COMMON_COMPILE_FLAGS) $(foreach incpath, $(INCPATHS), -I "$(incpath)") \
			$(foreach incpath, $(TOP_INCPATHS), -I "$(incpath)") $(CFLAGS)
CPPFLAGS:= $(COMMON_COMPILE_FLAGS) $(foreach incpath, $(INCPATHS), -I "$(incpath)") \
			$(foreach incpath, $(TOP_INCPATHS), -I "$(incpath)") $(CPPFLAGS)

ABSOLUTE_ASRCS			:= $(filter /%, $(ASRCS))
RELATIVE_ASRCS			:= $(filter-out /%, $(ASRCS))
FOUND_RELATIVE_ASRCS	:= $(foreach dir, $(subst :, ,$(subst $(CURRENT_MAKEFILE),.,$(VPATH))), \
							$(foreach file, $(RELATIVE_ASRCS), $(wildcard $(dir)/$(file))))
ASRCS					:= $(patsubst $(CURRENT_MAKEFILE)/%,%,$(ABSOLUTE_ASRCS)) $(FOUND_RELATIVE_ASRCS)

ABSOLUTE_CSRCS			:= $(filter /%, $(CSRCS))
RELATIVE_CSRCS			:= $(filter-out /%, $(CSRCS))
FOUND_RELATIVE_CSRCS	:= $(foreach dir, $(subst :, ,$(subst $(CURRENT_MAKEFILE),.,$(VPATH))), \
							$(foreach file, $(RELATIVE_CSRCS), $(wildcard $(dir)/$(file))))
CSRCS					:= $(patsubst $(CURRENT_MAKEFILE)/%,%,$(ABSOLUTE_CSRCS)) $(FOUND_RELATIVE_CSRCS)

ABSOLUTE_CPPSRCS		:= $(filter /%, $(CPPSRCS))
RELATIVE_CPPSRCS		:= $(filter-out /%, $(CPPSRCS))
FOUND_RELATIVE_CPPSRCS	:= $(foreach dir, $(subst :, ,$(subst $(CURRENT_MAKEFILE),.,$(VPATH))), \
							$(foreach file, $(RELATIVE_CPPSRCS), $(wildcard $(dir)/$(file))))
CPPSRCS					:= $(patsubst $(CURRENT_MAKEFILE)/%,%,$(ABSOLUTE_CPPSRCS)) $(FOUND_RELATIVE_CPPSRCS)

AOBJS	:= $(addprefix $(OUTPUTDIR_OBJS)/,$(ASRCS:.S=.o))
COBJS	:= $(addprefix $(OUTPUTDIR_OBJS)/,$(CSRCS:.c=.o))
CPPOBJS := $(addprefix $(OUTPUTDIR_OBJS)/,$(CPPSRCS:.cpp=.o))

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

$(AOBJS): $(OUTPUTDIR_OBJS)/%.o: %.S
	$(Q) -mkdir -p $(dir $@)
	$(call ASSEMBLE, $<, $@)

$(COBJS): $(OUTPUTDIR_OBJS)/%.o: %.c
	$(Q) -mkdir -p $(dir $@)
	$(call COMPILE, $<, $@)

$(CPPOBJS): $(OUTPUTDIR_OBJS)/%.o: %.cpp
	$(Q) -mkdir -p $(dir $@)
	$(call COMPILE_CPP, $<, $@)



all: $(OUTPUTDIR_LIB)/$(LIBTARGET)
	+$(Q) if [ -f $(CURDIR)/src/wapi/makefile ]; then $(MAKE) -C $(CURDIR)/src/wapi all; fi
	+$(Q) if [ -f $(CURDIR)/src/lib.mk ]; then $(MAKE) -C $(CURDIR)/src -f $(CURDIR)/src/lib.mk all; fi
	+$(Q) if [ -f $(CURDIR)/kvr/lib.mk ]; then $(MAKE) -C $(CURDIR)/kvr -f $(CURDIR)/kvr/lib.mk all; fi

# Nintendo Dev Agent — 3DS build (devkitPro). Produces nintendo-dev-agent.3dsx + .smdh
# Build:  export DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM && make
.SUFFIXES:

ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITARM)/3ds_rules

TARGET      := nintendo-dev-agent
BUILD       := build
# platform code + the platform-neutral core + the shared BSD-socket server
SOURCES     := source ../common/src ../posix/src
INCLUDES    := source ../common/include ../posix/include

# The version comes from the single source of truth (shown in the Homebrew Launcher description).
AGENT_VERSION   := $(shell sed -n 's/^\#define NDP_AGENT_VERSION "\(.*\)"$$/\1/p' $(TOPDIR)/../common/include/ndp/ndp_defs.h)
APP_TITLE       := Nintendo Dev Agent
APP_DESCRIPTION := v$(AGENT_VERSION) - remote dev agent
APP_AUTHOR      := BC Estrategias

ARCH    := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

CFLAGS  := -g -Wall -Wextra -O2 -mword-relocations -ffunction-sections -std=gnu99 $(ARCH)
CFLAGS  += $(INCLUDE) -D__3DS__

ASFLAGS := -g $(ARCH)
LDFLAGS  = -specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

LIBS    := -lctru -lm
LIBDIRS := $(CTRULIB)

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT   := $(CURDIR)/$(TARGET)
export TOPDIR   := $(CURDIR)
export VPATH    := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR  := $(CURDIR)/$(BUILD)

CFILES  := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
export LD := $(CC)
export OFILES := $(CFILES:.c=.o)
export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                  $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                  -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

ifeq ($(strip $(ICON)),)
	export APP_ICON := $(CTRULIB)/default_icon.png
else
	export APP_ICON := $(TOPDIR)/$(ICON)
endif
export _3DSXFLAGS += --smdh=$(CURDIR)/$(TARGET).smdh
export _3DSXDEPS := $(OUTPUT).smdh

.PHONY: all clean

all: $(BUILD)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

$(BUILD):
	@mkdir -p $@

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).3dsx $(OUTPUT).smdh $(TARGET).elf

else

$(OUTPUT).3dsx : $(OUTPUT).elf $(_3DSXDEPS)
$(OUTPUT).elf  : $(OFILES)

-include $(DEPSDIR)/*.d

endif

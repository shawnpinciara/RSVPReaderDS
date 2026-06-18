.SUFFIXES:

ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

include $(DEVKITARM)/ds_rules

BUILD    := build
SOURCES  := source
INCLUDES := include

WOOPSI   ?= $(HOME)/devkitpro-libs/woopsi/Woopsi/libwoopsi

ARCH     := -march=armv5te -mtune=arm946e-s -mthumb

CFLAGS   := -g -Wall -O2 -ffunction-sections -fdata-sections $(ARCH) $(INCLUDE) -DARM9
CXXFLAGS := $(CFLAGS) -fno-rtti -fno-exceptions
ASFLAGS  := -g $(ARCH)
LDFLAGS   = -specs=ds_arm9.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

LIBS    := -lwoopsi -lfat -lz -lnds9
LIBDIRS := $(LIBNDS) $(WOOPSI) /opt/devkitpro/portlibs/nds

ifneq ($(BUILD),$(notdir $(CURDIR)))

export ARM9ELF := $(CURDIR)/$(TARGET).elf
export VPATH   := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)

CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
CFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
SFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))

ifeq ($(strip $(CPPFILES)),)
  export LD := $(CC)
else
  export LD := $(CXX)
endif

export OFILES  := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                  -I$(TOPDIR)/shared \
                  $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                  -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

.PHONY: $(BUILD) clean

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean arm9...
	@rm -fr $(BUILD) $(TARGET).elf

else

DEPENDS := $(OFILES:.o=.d)
$(ARM9ELF): $(OFILES)
-include $(DEPENDS)

endif

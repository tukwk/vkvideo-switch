ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

TARGET   := vkvideo
BUILD    := build
SOURCES  := src src/vendor
DATA     := data
INCLUDES := src

APP_TITLE   := VK Video
APP_AUTHOR  := vkvideo-switch
APP_VERSION := 0.4.1

ARCH := -march=armv8-a+crc+crypto -mtune=cortex-a57 -fPIC

CFLAGS   := -g -Wall -Wextra -O2 -ffunction-sections -fdata-sections \
            $(ARCH) $(DEFINES) $(INCLUDE) -D__SWITCH__
CXXFLAGS := $(CFLAGS) -std=gnu++17 -fno-rtti -fno-exceptions

LDFLAGS := -specs=$(DEVKITPRO)/libnx/switch.specs -specs=sysbase.specs -g $(ARCH) \
           -Wl,-Map,$(notdir $*.map)

LIBS    := -Wl,--start-group -lavformat -lavcodec -lswscale -lswresample \
           -lavutil -Wl,--end-group -lmbedtls -lmbedx509 -lmbedcrypto -lnx -lm
LIBDIRS := $(PORTLIBS) $(LIBNX)

# The project directory may contain spaces, so the recursive build uses
# relative paths and an explicit VK_BUILD marker instead of CURDIR tricks.

#---------------------------------------------------------------------------------
ifneq ($(VK_BUILD),1)
#---------------------------------------------------------------------------------

export OUTPUT := $(TARGET)

export VPATH := $(foreach dir,$(SOURCES),../$(dir)) \
                $(foreach dir,$(DATA),../$(dir))

export DEPSDIR := .

CFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES := $(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

ifeq ($(strip $(CPPFILES)),)
export LD := $(CC)
else
export LD := $(CXX)
endif

export OFILES_BIN := $(addsuffix .o,$(BINFILES))
export OFILES_SRC := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES     := $(OFILES_BIN) $(OFILES_SRC)
export HFILES_BIN := $(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE  := -I. -I../$(firstword $(INCLUDES)) \
                   $(foreach dir,$(LIBDIRS),-I$(dir)/include)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

.PHONY: all clean test $(BUILD)

all: $(BUILD)
	@cp -f "$(BUILD)/$(TARGET).nro" "$(TARGET).nro"
	@echo "built ... $(TARGET).nro"

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C "$(BUILD)" -f "$(CURDIR)/Makefile" VK_BUILD=1

test:
	@./tools/test-host.sh

clean:
	@rm -rf "$(BUILD)" "$(TARGET).nro" "$(TARGET).nsp" "$(TARGET).nso" \
	        "$(TARGET).elf" "$(TARGET).map"

#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------

DEPENDS := $(OFILES:.o=.d)

.DEFAULT_GOAL := $(OUTPUT).nro

# switch_rules' %.nacp depends on $(MAKEFILE_LIST), which breaks when the
# project path contains spaces. This target is equivalent but path-safe.
$(OUTPUT).nacp:
	@nacptool --create "$(APP_TITLE)" "$(APP_AUTHOR)" "$(APP_VERSION)" $@

$(OFILES_SRC): $(HFILES_BIN)

$(OUTPUT).nro: $(OUTPUT).elf $(OUTPUT).nacp

$(OUTPUT).elf: $(OFILES)

-include $(DEPENDS)

endif

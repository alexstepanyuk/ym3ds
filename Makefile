.SUFFIXES:
ifeq ($(strip $(DEVKITARM)),)
$(error Укажите DEVKITARM; установите devkitPro 3ds-dev, 3ds-curl, 3ds-mbedtls и 3ds-zlib)
endif
include $(DEVKITARM)/3ds_rules

TARGET := ym3ds
BUILD := build
SOURCES := source vendor/cjson
INCLUDES := include vendor/cjson vendor/minimp3 vendor/stb
ARCH := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
CFLAGS := -O2 -g -Wall -Wextra -std=gnu11 -mword-relocations -ffunction-sections $(ARCH)
CFLAGS += $(INCLUDE) -D__3DS__
LDFLAGS = -specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)
LIBS := -lcitro2d -lcitro3d -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lz -lctru -lm
LIBDIRS := $(CTRULIB) $(DEVKITPRO)/portlibs/3ds
APP_TITLE := YM3DS
APP_DESCRIPTION := Неофициальный прототип Яндекс Музыки
APP_AUTHOR := YM3DS contributors

ifneq ($(BUILD),$(notdir $(CURDIR)))
export OUTPUT := $(CURDIR)/$(TARGET)
export TOPDIR := $(CURDIR)
export VPATH := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)
export LD := $(CC)
export OFILES := $(foreach dir,$(SOURCES),$(notdir $(patsubst %.c,%.o,$(wildcard $(dir)/*.c))))
export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) $(foreach dir,$(LIBDIRS),-I$(dir)/include) -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)
export _3DSXDEPS := $(OUTPUT).smdh
export _3DSXFLAGS := --smdh=$(OUTPUT).smdh

.PHONY: all clean
all: $(BUILD)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile
$(BUILD):
	@mkdir -p $@
clean:
	@rm -rf $(BUILD) $(TARGET).3dsx $(TARGET).smdh $(TARGET).elf
else
$(OUTPUT).3dsx: $(OUTPUT).elf $(_3DSXDEPS)
$(OUTPUT).elf: $(OFILES)
-include $(DEPSDIR)/*.d
endif

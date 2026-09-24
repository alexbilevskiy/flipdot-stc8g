# Flip-dot quad STC8G firmware
#
# Toolchain: sdcc + packihx (sdcc 4.5.0)
# Library:   ../FwLib_STC8 (IOsetting/FwLib_STC8)
# Target:    STC8G1K08 TSSOP20
#
# Build:  make            -> build/quad.hex (copied to $(DEPLOY_DIR) if set)
# Flash:  make flash PORT=/dev/ttyX   (stcgal + pyserial) or directly:
#   stcgal -p PORT -P stc8g flipdot-stc8g-quad.hex
# stcgal polls the header; power-cycle the module while it polls
# (power off -> start stcgal -> power on).

FWLIB     := $(abspath ../FwLib_STC8)
PROJECT   := quad
BUILD_DIR := build
DEPLOY_DIR ?=

SDCC      ?= sdcc
PACKIHX   ?= packihx
STCGAL    ?= python3 ../stcgal/stcgal.py

MCU_IRAM       := 256
MCU_XRAM       := 1024
MCU_CODE_SIZE  := 8192

# STC8G1K08, power-on default IRC ~24 MHz.
# No SYS_SetClock()/IRC re-trim: the trim constants are per-chip, we run on
# the boot default. 24 MHz -> 115200 baud error ~0.2%.
LIB_FLAGS := __CONF_FOSC=24000000UL \
             __CONF_MCU_MODEL=MCU_MODEL_STC8G1K08

CFLAGS := -mmcs51 --model-large --opt-code-size --std-sdcc99 \
          $(addprefix -D,$(LIB_FLAGS)) -I$(FWLIB)/include -Isrc

# Only the modules actually used — the full library does not fit in 8 KB
# together with the app.
LIB_SRCS := $(FWLIB)/src/fw_sys.c $(FWLIB)/src/fw_uart.c $(FWLIB)/src/fw_util.c
APP_SRCS := $(wildcard src/*.c)
OBJS     := $(addprefix $(BUILD_DIR)/,$(notdir $(LIB_SRCS:.c=.rel) $(APP_SRCS:.c=.rel)))

.PHONY: all clean flash

all: $(BUILD_DIR)/$(PROJECT).hex
ifneq ($(DEPLOY_DIR),)
	cp $< $(DEPLOY_DIR)/flipdot-stc8g-$(PROJECT).hex
endif

$(BUILD_DIR)/fw_%.rel: $(FWLIB)/src/fw_%.c
	@mkdir -p $(BUILD_DIR)
	$(SDCC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.rel: src/%.c
	@mkdir -p $(BUILD_DIR)
	$(SDCC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/$(PROJECT).ihx: $(OBJS)
	$(SDCC) $(CFLAGS) $^ \
		--iram-size $(MCU_IRAM) --xram-size $(MCU_XRAM) --code-size $(MCU_CODE_SIZE) \
		--out-fmt-ihx -o $@

$(BUILD_DIR)/$(PROJECT).hex: $(BUILD_DIR)/$(PROJECT).ihx
	$(PACKIHX) $< > $@

clean:
	rm -rf $(BUILD_DIR)

flash: $(BUILD_DIR)/$(PROJECT).hex
	$(STCGAL) -p $(PORT) -P stc8g $<

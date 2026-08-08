# Point this at your libDaisy checkout.
LIBDAISY_DIR ?= $(HOME)/dev/libDaisy

TARGET       = verb_seed
CPP_SOURCES  = main.cpp

# core/ is header-only and platform-free — just add it to the include path.
C_INCLUDES  += -I../core

# The core uses `if constexpr`. libDaisy defaults to gnu++14; it will not build
# without this line.
CPP_STANDARD = -std=gnu++17

# Kills the in-loop shelf asserts. They are a desktop-build safety net; on the
# Seed they drag in newlib's assert machinery for no benefit.
C_DEFS      += -DNDEBUG

OPT          = -O3

SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile

# Variant: same firmware, every buffer in SDRAM.
sdram:
	$(MAKE) C_DEFS="$(C_DEFS) -DVERB_HOT_SRAM=0" all
.PHONY: sdram

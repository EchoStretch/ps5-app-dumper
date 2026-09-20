# Build rules for a web homebrew that stands on webhb/. Set, then include:
#
#   WHB_ROOT     where the repository's root is, seen from the app's Makefile
#   WHB_APP      base name of the ELF                       (klogview)
#   WHB_PAGE     the app's page, with its @WHB_CSS@ / @WHB_JS@ / @WHB:name@ lines
#   WHB_ICON     512x512 PNG: favicon, home-screen icon, tile
#   WHB_SOURCES  the app's own C files
#   WHB_CFLAGS   more flags, if any
#
# Targets: all (the payload; needs PS5_PAYLOAD_SDK), sim (the same program for
# this machine, console business stubbed - see webhb/host/stubs.c), run, clean.
#
# The payload is built twice: a payload sent over the network has no file of
# its own to hand to a payload manager, so stage 1 is embedded into the final
# ELF, which the web UI can then store in pldmgr.

.DEFAULT_GOAL := all

WHB      := $(WHB_ROOT)/webhb
WHB_OUT  ?= build
WHB_PORT ?= 8199
WHB_INC  := -I$(WHB)/include

WHB_CORE := $(wildcard $(WHB)/*.c)
# what only makes sense on the console; webhb/host/stubs.c stands in for it
WHB_CONSOLE_ONLY := %/tile.c %/instance.c %/drives.c %/start.c

WHB_GEN  := $(WHB_OUT)/web_assets.c $(WHB_OUT)/icon_assets.c
WHB_ID   := $(shell git rev-parse --short HEAD 2>/dev/null || echo nogit)-$(shell date +%m%d-%H%M)

$(WHB_OUT)/web_assets.c: $(WHB_PAGE) $(wildcard $(WHB)/web/*) $(WHB_ROOT)/tools/inline.sh $(WHB_ROOT)/tools/bin2c.sh
	@mkdir -p $(WHB_OUT)
	$(WHB_ROOT)/tools/inline.sh $(WHB_PAGE) $(WHB)/web | sed "s/@BUILD@/$(WHB_ID)/g" > $(WHB_OUT)/index.html
	$(WHB_ROOT)/tools/bin2c.sh $(WHB_OUT)/index.html web_index_html > $@

$(WHB_OUT)/icon_assets.c: $(WHB_ICON) $(WHB_ROOT)/tools/bin2c.sh
	@mkdir -p $(WHB_OUT)
	$(WHB_ROOT)/tools/bin2c.sh $(WHB_ICON) app_icon_png > $@

$(WHB_OUT)/self_none.c:
	@mkdir -p $(WHB_OUT)
	printf '#include <stddef.h>\nconst unsigned char self_elf[] = { 0 };\nconst size_t self_elf_len = 0;\n' > $@

# ---- the payload
ifdef PS5_PAYLOAD_SDK
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

WHB_ALL := $(WHB_SOURCES) $(WHB_CORE) $(WHB_GEN)

all: $(WHB_APP).elf

$(WHB_OUT)/stage1.elf: $(WHB_ALL) $(WHB_OUT)/self_none.c
	$(CC) -Werror -pthread -O2 -Wall $(WHB_INC) $(WHB_CFLAGS) -o $@ $^

$(WHB_OUT)/self_elf.c: $(WHB_OUT)/stage1.elf
	$(WHB_ROOT)/tools/bin2c.sh $< self_elf > $@

$(WHB_APP).elf: $(WHB_ALL) $(WHB_OUT)/self_elf.c
	$(CC) -Werror -pthread -O2 -Wall $(WHB_INC) $(WHB_CFLAGS) -o $@ $^
	strip $@
else
all:
	@echo "PS5_PAYLOAD_SDK is undefined - only 'make sim' works without it" >&2; false
endif

# ---- the same program on this machine
HOSTCC ?= cc

sim: $(WHB_OUT)/sim

$(WHB_OUT)/sim: $(WHB_SOURCES) $(filter-out $(WHB_CONSOLE_ONLY),$(WHB_CORE)) $(WHB)/host/stubs.c $(WHB_GEN)
	$(HOSTCC) -O1 -Wall -pthread $(WHB_INC) $(WHB_CFLAGS) -DWHB_HOST \
	    -DINTERNAL_ROOT='"data"' -DSTORE_ROOT='"$(abspath $(WHB_OUT))/run/pstore/"' -o $@ $^

# serves http://127.0.0.1:$(WHB_PORT)/ until interrupted
run: $(WHB_OUT)/sim
	@mkdir -p $(WHB_OUT)/run
	cd $(WHB_OUT)/run && WHB_PORT=$(WHB_PORT) ../sim

clean:
	rm -rf $(WHB_OUT) $(WHB_APP).elf

.PHONY: all sim run clean

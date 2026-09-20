PS5_HOST ?= ps5
PS5_PORT ?= 9021

ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined)
endif

ELF := ps5-app-dumper.elf

CFLAGS := -Werror -pthread -O2 -Wall -Iinclude -Iwebhb/include


# The web UI is compiled into the payload so it works no matter how the
# ELF was delivered to the console.
WEB_PAGE := web/index.html
WEB_GEN  := source/web_assets.c

# The home-screen tile's icon travels inside the payload as well. Its
# param.json is written at install time because it carries the live port.
TILE_ICON := assets/tile/icon0.png
TILE_GEN  := source/tile_assets.c

# A payload sent over the network has no file of its own to hand to a
# payload manager. So the ELF is built twice: once as it is (stage 1), and
# once more with stage 1 embedded, which the web UI can store in pldmgr.
SELF_GEN  := source/self_elf.c
SELF_NONE := source/self_elf_none.c
STAGE1    := ps5-app-dumper.stage1.elf

all: $(ELF)

# webhb/ is the part that is not about dumping: the web server and, step by
# step, what else a web homebrew needs. See docs/webhb-plan.md.
CFILES := $(filter-out $(WEB_GEN) $(TILE_GEN) $(SELF_GEN) $(SELF_NONE),$(wildcard source/*.c)) \
          $(wildcard webhb/*.c) $(WEB_GEN) $(TILE_GEN)

# The page carries a stamp of when it was embedded, so a look at the footer
# tells which version a browser - or its cache - is showing.
BUILD_ID := $(shell git rev-parse --short HEAD 2>/dev/null || echo nogit)-$(shell date +%m%d-%H%M)

# The client kit (webhb/web) goes into the page first: what is served stays
# one file, which is what lets the browser cache it as a whole.
WHB_KIT := webhb/web

$(WEB_GEN): $(WEB_PAGE) $(wildcard $(WHB_KIT)/*) tools/inline.sh tools/bin2c.sh
	./tools/inline.sh $(WEB_PAGE) $(WHB_KIT) | sed "s/@BUILD@/$(BUILD_ID)/g" > $(WEB_GEN).html
	./tools/bin2c.sh $(WEB_GEN).html web_index_html > $@
	rm -f $(WEB_GEN).html

$(TILE_GEN): $(TILE_ICON) tools/bin2c.sh
	./tools/bin2c.sh $(TILE_ICON) tile_icon0_png > $@

$(STAGE1): $(CFILES) $(SELF_NONE)
	$(CC) $(CFLAGS) -o $@ $^

$(SELF_GEN): $(STAGE1) tools/bin2c.sh
	./tools/bin2c.sh $(STAGE1) self_elf > $@

$(ELF): $(CFILES) $(SELF_GEN)
	$(CC) $(CFLAGS) -o $@ $^
	strip $@

clean:
	rm -f $(ELF) $(STAGE1) $(WEB_GEN) $(WEB_GEN).html $(TILE_GEN) $(SELF_GEN)

test: $(ELF)
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $^

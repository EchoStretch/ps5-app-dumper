PS5_HOST ?= ps5
PS5_PORT ?= 9021

ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined)
endif

ELF := ps5-app-dumper.elf

CFLAGS := -Werror -pthread -O2 -Wall -Iinclude


# The web UI is compiled into the payload so it works no matter how the
# ELF was delivered to the console.
WEB_PAGE := web/index.html
WEB_GEN  := source/web_assets.c

# The home-screen tile's icon travels inside the payload as well. Its
# param.json is written at install time because it carries the live port.
TILE_ICON := assets/tile/icon0.png
TILE_GEN  := source/tile_assets.c

all: $(ELF)

CFILES := $(filter-out $(WEB_GEN) $(TILE_GEN),$(wildcard source/*.c)) $(WEB_GEN) $(TILE_GEN)

# The page carries a stamp of when it was embedded, so a look at the footer
# tells which version a browser - or its cache - is showing.
BUILD_ID := $(shell git rev-parse --short HEAD 2>/dev/null || echo nogit)-$(shell date +%m%d-%H%M)

$(WEB_GEN): $(WEB_PAGE) tools/bin2c.sh
	sed "s/@BUILD@/$(BUILD_ID)/g" $(WEB_PAGE) > $(WEB_GEN).html
	./tools/bin2c.sh $(WEB_GEN).html web_index_html > $@
	rm -f $(WEB_GEN).html

$(TILE_GEN): $(TILE_ICON) tools/bin2c.sh
	./tools/bin2c.sh $(TILE_ICON) tile_icon0_png > $@

$(ELF): $(CFILES)
	$(CC) $(CFLAGS) -o $@ $^
	strip $@

clean:
	rm -f $(ELF) $(WEB_GEN) $(WEB_GEN).html $(TILE_GEN)

test: $(ELF)
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $^

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

all: $(ELF)

CFILES := $(filter-out $(WEB_GEN),$(wildcard source/*.c)) $(WEB_GEN)

$(WEB_GEN): $(WEB_PAGE) tools/bin2c.sh
	./tools/bin2c.sh $(WEB_PAGE) web_index_html > $@

$(ELF): $(CFILES)
	$(CC) $(CFLAGS) -o $@ $^
	strip $@

clean:
	rm -f $(ELF) $(WEB_GEN)

test: $(ELF)
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $^

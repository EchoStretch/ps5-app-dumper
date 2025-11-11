PS5_HOST ?= ps5
PS5_PORT ?= 9021

ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined)
endif

ELF := ps5-app-dumper.elf

CFLAGS := -Werror -pthread -O2 -Wall -Iinclude

all: $(ELF)

CFILES := $(wildcard source/*.c)

$(ELF): $(CFILES)
	$(CC) $(CFLAGS) -o $@ $^
	strip $@

clean:
	rm -f $(ELF)

test: $(ELF)
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $^

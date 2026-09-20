PS5_HOST ?= ps5
PS5_PORT ?= 9021

# The dumper is a web homebrew like any other: the web server, the page kit,
# the build stamp, the icon and the two-stage build that lets the web UI store
# the payload in pldmgr are webhb's - a project of its own, installed as the
# release webhb.lock pins (tools/webhb-dir.mk says where it is looked for).
# What is written here is what the dumper is made of.
include tools/webhb-dir.mk
WHB_ROOT    := $(WEBHB_DIR)
WHB_APP     := ps5-app-dumper
WHB_PAGE    := web/index.html
WHB_ICON    := assets/tile/icon0.png
WHB_CFLAGS  := -Iinclude
# files earlier Makefiles generated into source/ may still lie around
WHB_SOURCES := $(filter-out source/web_assets.c source/tile_assets.c source/self_elf.c source/self_elf_none.c, \
                            $(wildcard source/*.c))
# the dumper's pretend console is harness/, which fakes titles and drives too
WHB_NO_SIM  := 1

include $(WHB_ROOT)/webhb/webhb.mk

clean: clean-legacy
clean-legacy:
	rm -f ps5-app-dumper.stage1.elf source/web_assets.c source/web_assets.c.html source/tile_assets.c source/self_elf.c

test: $(WHB_APP).elf
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $^

.PHONY: clean-legacy test

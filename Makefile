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
# A build for testers - a day, a firmware, a list of consoles - is webhb's.
# The values live in test-build.mk (not in git; test-build.mk.example shows
# the lines) and are read for `make test-build` only, which puts the ELF into
# dist/ under a name that says what it is. Or by hand, without the file:
# make TEST_EXPIRES=2026-09-27 TEST_FIRMWARE=12.00 TEST_CONSOLES=id1,id2
ifneq ($(filter test-build,$(MAKECMDGOALS)),)
-include test-build.mk
endif
# files earlier Makefiles generated into source/ may still lie around
WHB_SOURCES := $(filter-out source/web_assets.c source/tile_assets.c source/self_elf.c source/self_elf_none.c, \
                            $(wildcard source/*.c))
# the dumper's pretend console is harness/, which fakes titles and drives too
WHB_NO_SIM  := 1

include $(WHB_ROOT)/webhb/webhb.mk

clean: clean-legacy
clean-legacy:
	rm -f ps5-app-dumper.stage1.elf source/web_assets.c source/web_assets.c.html source/tile_assets.c source/self_elf.c

comma := ,
TEST_ELF := dist/$(WHB_APP)_v$(shell sed -n 's/.*DUMPER_VERSION "\(.*\)"/\1/p' include/version.h)-test$(if $(TEST_EXPIRES),-until-$(TEST_EXPIRES))$(if $(TEST_FIRMWARE),-fw$(subst $(comma),+,$(TEST_FIRMWARE)))$(if $(TEST_CONSOLES),-$(subst $(comma),+,$(TEST_CONSOLES))).elf
test-build:
	@test -n "$(TEST_EXPIRES)$(TEST_FIRMWARE)$(TEST_CONSOLES)" || { echo "test-build: nothing set - copy test-build.mk.example to test-build.mk" >&2; false; }
	$(MAKE) clean
	$(MAKE) TEST_EXPIRES=$(TEST_EXPIRES) TEST_FIRMWARE=$(TEST_FIRMWARE) TEST_CONSOLES=$(TEST_CONSOLES)
	@mkdir -p dist && cp $(WHB_APP).elf $(TEST_ELF) && echo "test-build: $(TEST_ELF)"

test: $(WHB_APP).elf
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $^

.PHONY: clean-legacy test test-build

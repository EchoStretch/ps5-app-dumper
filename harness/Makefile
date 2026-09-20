# Host harness: the payload's web server, job, queue and dump store, built for
# this machine against a pretend console. Needs a C compiler and nothing else -
# no PS5 SDK. See README.md.

ROOT := ..
OUT  := out
RUN  := $(OUT)/run
PORT ?= 8099
CC   ?= cc

# the payload's own sources that run unchanged on the host; the tile, the
# process list, the start-up and the drives are PS5 business and faked in mock_console.c
REAL := $(filter-out %/tile.c %/instance.c %/drives.c %/start.c,$(wildcard $(ROOT)/webhb/*.c)) \
        $(ROOT)/source/routes_dumper.c $(ROOT)/source/dumper_app.c $(ROOT)/source/dumper_config.c \
        $(ROOT)/source/dump_job.c $(ROOT)/source/dump_queue.c $(ROOT)/source/dump_store.c $(ROOT)/source/dump_library.c \
        $(ROOT)/source/utils.c

GEN  := $(OUT)/web_assets.c $(OUT)/icon_assets.c

# short timeouts keep a queue run watchable; the stored-file check is pointed
# at a folder the fake Payload Manager really writes to
CFLAGS := -O1 -Wall -pthread -I$(ROOT)/include -I$(ROOT)/webhb/include \
          -DMOUNT_TIMEOUT=15 -DINTERNAL_ROOT='"data"' -DSTORE_ROOT='"$(abspath $(RUN))/pstore/"'

.PHONY: sim run pldmgr snapshot render styles clean

sim: $(OUT)/sim

$(OUT)/web_assets.c: $(ROOT)/web/index.html $(wildcard $(ROOT)/webhb/web/*) $(ROOT)/tools/inline.sh $(ROOT)/tools/bin2c.sh
	@mkdir -p $(OUT)
	$(ROOT)/tools/inline.sh $< $(ROOT)/webhb/web > $(OUT)/index.html
	$(ROOT)/tools/bin2c.sh $(OUT)/index.html web_index_html > $@

$(OUT)/icon_assets.c: $(ROOT)/assets/tile/icon0.png $(ROOT)/tools/bin2c.sh
	@mkdir -p $(OUT)
	$(ROOT)/tools/bin2c.sh $< app_icon_png > $@

$(OUT)/sim: mock_console.c self_blob.c $(REAL) $(GEN)
	$(CC) $(CFLAGS) -o $@ $^

# serves http://127.0.0.1:$(PORT)/ until interrupted
run: $(OUT)/sim
	@mkdir -p $(RUN)/usb0
	cd $(RUN) && HARNESS_ART=$(abspath $(ROOT)/assets/tile/icon0.png) ../sim usb0 $(PORT)

# a stand-in for pldmgr on port 8084: payload list, upload, launch
pldmgr:
	@mkdir -p $(RUN)/pstore/ps5-app-dumper
	python3 fake_pldmgr.py $(abspath $(RUN)) $(abspath $(OUT)/sim)

# every route's answer, normalised, into out/snapshot.txt - diff two of them
snapshot: $(OUT)/sim
	./regress.sh $(abspath $(OUT)/sim) $(abspath $(OUT))/snapshot.txt

# what a browser makes of the page: run `make run` in another terminal first
render:
	./domcheck.sh http://127.0.0.1:$(PORT)/

# the computed style of every element in every view - see README.md
styles:
	node --experimental-websocket stylecheck.js http://127.0.0.1:$(PORT)/ $(OUT)/styles.json

clean:
	rm -rf $(OUT)

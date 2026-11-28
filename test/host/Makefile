CORE  := ../../core
SRC   := ../../src
BSP   := ../../board/bsp
PROTO := ../../external/clu-esp32/components/clu_proto
PUFF  := ../../external/puff
# Everything built (test binaries, gcov data, coverage reports) goes here.
OUT := out
$(shell mkdir -p $(OUT))

# make COVERAGE=1 ...: instrument the code under test (gcov); `make coverage` does the whole run.
COVERAGE ?= 0
COV := $(if $(filter 1,$(COVERAGE)),--coverage)
# Coverage builds run unoptimised: -O1 merges lines and turns switches into tables gcov cannot attribute.
OPT := $(if $(COV),-O0,-O1)
CFLAGS := -std=gnu11 -Wall -Wextra -Werror $(OPT) -g -fsanitize=address,undefined $(COV)
# Firmware sources (src/) compile against the Pico SDK / FatFS stand-ins in stubs/ (declarations
# only: each test defines the fakes it needs).
FW_INC := -Istubs -I$(BSP) -I$(CORE) -I$(SRC)
STUBS := $(wildcard stubs/*.h stubs/*/*.h)
HDRS := $(wildcard $(CORE)/*.h $(SRC)/*.h $(BSP)/*.h)

TESTS := $(addprefix $(OUT)/,test_panel test_png test_main test_icons test_scanout test_bsp)

$(OUT)/test_panel: test_panel.c $(CORE)/panel_core.c $(PROTO)/tfbus_frame.c $(HDRS)
	$(CC) $(CFLAGS) -I$(CORE) -I$(PROTO)/include -o $@ $(filter %.c,$^)

# PNG fixtures: python3 tools/gen_png_fixtures.py (committed in fixtures/)
$(OUT)/test_png: test_png.c $(CORE)/png_icon.c $(PUFF)/puff.c $(HDRS)
	$(CC) $(CFLAGS) -I$(CORE) -I$(PUFF) -o $@ $(filter %.c,$^)

# src/main.c is included by the test (static functions); the core and the UI are the real ones.
$(OUT)/test_main: test_main.c $(SRC)/main.c $(CORE)/panel_core.c $(SRC)/ui.c $(STUBS) $(HDRS)
	$(CC) $(CFLAGS) $(FW_INC) -o $@ test_main.c $(CORE)/panel_core.c $(SRC)/ui.c -lm

$(OUT)/test_icons: test_icons.c $(SRC)/icons.c $(CORE)/png_icon.c $(PUFF)/puff.c $(STUBS) $(HDRS)
	$(CC) $(CFLAGS) $(FW_INC) -I$(PUFF) -o $@ test_icons.c $(CORE)/png_icon.c $(PUFF)/puff.c

$(OUT)/test_scanout: test_scanout.c $(SRC)/scanout.c $(SRC)/ui.c $(STUBS) $(HDRS)
	$(CC) $(CFLAGS) $(FW_INC) -o $@ test_scanout.c $(SRC)/ui.c -lm

# Board drivers (included). Vendor code: its own warnings are not ours to fail on.
BSP_VENDOR_FLAGS := -Wno-unused-parameter -Wno-incompatible-pointer-types
$(OUT)/test_bsp: test_bsp.c $(BSP)/bsp_i2c.c $(BSP)/bsp_gt911.c $(BSP)/bsp_qmi8658.c $(STUBS) $(HDRS)
	$(CC) $(CFLAGS) $(BSP_VENDOR_FLAGS) -Istubs -I$(BSP) -o $@ test_bsp.c -lm

# The tests read fixtures/ relative to this directory (make -C test/host).
.PHONY: test clean coverage
test: $(TESTS)
	$(OUT)/test_panel
	$(if $(CAPTURE),$(OUT)/test_panel "$(CAPTURE)")
	$(OUT)/test_png
	$(OUT)/test_main
	$(OUT)/test_icons
	$(OUT)/test_scanout
	$(OUT)/test_bsp

clean:
	rm -rf $(OUT) && mkdir -p $(OUT) # recreated: `make clean test` reuses the parse-time mkdir

# Line/branch coverage of the firmware sources (core/, src/, board/): summary on stdout,
# $(OUT)/coverage/summary.md (Markdown table) and $(OUT)/coverage/coverage.json. Needs gcc/gcov,
# python3. FAIL_UNDER=<pct> fails the run when total line coverage is lower; VERBOSE=1 lists the
# uncovered lines.
coverage:
	$(MAKE) clean
	$(MAKE) COVERAGE=1 test
	python3 coverage.py $(if $(FAIL_UNDER),--fail-under $(FAIL_UNDER)) $(if $(VERBOSE),--verbose)

preview: preview.c $(SRC)/ui.c $(CORE)/panel_core.c $(CORE)/png_icon.c $(PUFF)/puff.c
	$(CC) -std=gnu11 -O2 -Wall -Wextra -I$(CORE) -I$(SRC) -I$(PUFF) -fsanitize=address,undefined -o $(OUT)/preview $^ -lm
	$(OUT)/preview
	python3 -c "from PIL import Image; [Image.open(f+'.ppm').save(f+'.png') for f in ('preview_online','preview_waiting')]"
	rm -f preview_online.ppm preview_waiting.ppm

CORE  := ../../core
PROTO := ../../external/clu-esp32/components/clu_proto
PUFF  := ../../external/puff
CFLAGS := -std=gnu11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -I$(CORE) -I$(PROTO)/include -I$(PUFF)

test_panel: test_panel.c $(CORE)/panel_core.c $(PROTO)/tfbus_frame.c
	$(CC) $(CFLAGS) -o $@ $^

# PNG fixtures: python3 tools/gen_png_fixtures.py (committed in fixtures/)
test_png: test_png.c $(CORE)/png_icon.c $(PUFF)/puff.c
	$(CC) $(CFLAGS) -o $@ $^

.PHONY: test clean
test: test_panel test_png
	./test_panel
	$(if $(CAPTURE),./test_panel "$(CAPTURE)")
	./test_png

clean:
	rm -f test_panel test_png

preview: preview.c ../../src/ui.c $(CORE)/panel_core.c $(CORE)/png_icon.c $(PUFF)/puff.c
	$(CC) -std=gnu11 -O2 -Wall -Wextra -I$(CORE) -I../../src -I$(PUFF) -fsanitize=address,undefined -o preview_bin $^ -lm
	./preview_bin
	python3 -c "from PIL import Image; [Image.open(f+'.ppm').save(f+'.png') for f in ('preview_online','preview_waiting')]"
	rm -f preview_online.ppm preview_waiting.ppm preview_bin

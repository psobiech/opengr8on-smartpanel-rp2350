CORE  := ../../core
PROTO := ../../external/clu-esp32/components/clu_proto
CFLAGS := -std=gnu11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -I$(CORE) -I$(PROTO)/include

test_panel: test_panel.c $(CORE)/panel_core.c $(PROTO)/tfbus_frame.c
	$(CC) $(CFLAGS) -o $@ $^

.PHONY: test clean
test: test_panel
	./test_panel
	$(if $(CAPTURE),./test_panel "$(CAPTURE)")

clean:
	rm -f test_panel

preview: preview.c ../../src/ui.c $(CORE)/panel_core.c
	$(CC) -std=gnu11 -O2 -Wall -Wextra -I$(CORE) -I../../src -fsanitize=address,undefined -o preview_bin $^ -lm
	./preview_bin
	python3 -c "from PIL import Image; [Image.open(f+'.ppm').save(f+'.png') for f in ('preview_online','preview_waiting')]"
	rm -f preview_online.ppm preview_waiting.ppm preview_bin

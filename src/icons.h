// Button icons (PANEL_BUTTON IconA / IconB) from the SD card: /icons/<name>.png, decoded
// (core/png_icon.c) into a small RAM cache. The card is mounted once at boot; without a card the
// buttons show their labels only. A cached icon stays until its slot is needed for another one,
// so a changed file shows after a restart.
#pragma once

#include "png_icon.h"

// After the display init: the SD driver claims free DMA channels (the display claims channel 0).
void icons_init(void);
// Cached icon for a CLU icon name ("~name": the negative). NULL for "", while it is still loading,
// and if the card, the file or its decoding failed. A name not seen before is queued for
// icons_poll. Main loop only.
const png_icon_t *icons_find(const char *name);
// Load one queued icon (blocks while the card is read: a few ms). Main loop only.
void icons_poll(void);

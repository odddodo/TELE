#pragma once

#define W             64
#define H             64
#define CW            32   // coarse noise grid
#define CH            32
#define BITPLANES      6
#define PALETTE_COUNT 10

void        graphicsInit(int paletteIdx);
void        buildPalette(int idx);
void        renderFrame(float soft, float scAX, float scAY, float scBX, float scBY);
void        pushToPanel();
void        graphicsTick(float dtG, float dtQ);   // advance gtime / qtime
const char *paletteName(int idx);

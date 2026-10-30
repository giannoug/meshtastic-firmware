// Host bodies for the T5 panel driver: the real setupNicheGraphics() constructs an ED047TC1, and its
// device .cpp compiles empty here, so update() keeps the frame instead of driving FastEPD.
#ifdef T5_SCREENS_HOST
#include "ScreenCapture.h"

#include "graphics/niche/Drivers/EInk/ED047TC1.h"

#include <cstdio>
#include <cstring>
#include <vector>

using namespace NicheGraphics::Drivers;

static std::vector<uint8_t> lastFrame;
static uint16_t frameW = 0, frameH = 0;
static uint32_t frameCount = 0;

void ED047TC1::begin(SPIClass *, uint8_t, uint8_t, uint8_t, uint8_t)
{
    frameW = width;
    frameH = height;
}

void ED047TC1::update(uint8_t *imageData, UpdateTypes)
{
    lastFrame.assign(imageData, imageData + ((frameW + 7) / 8) * frameH);
    frameCount++;
}

uint32_t captureFrameCount()
{
    return frameCount;
}

// Black pixels in the last frame (1 = white in the InkHUD buffer)
uint32_t captureBlackPixels()
{
    uint32_t n = 0;
    for (uint8_t b : lastFrame)
        n += 8 - __builtin_popcount(b);
    return n;
}

// Panel-orientation 928x508 P4 PBM (1 = black). The tooling rotates it by the recorded mode.
bool captureSavePbm(const char *path)
{
    if (lastFrame.empty())
        return false;
    FILE *f = fopen(path, "wb");
    if (!f)
        return false;
    fprintf(f, "P4\n%u %u\n", frameW, frameH);
    for (uint8_t b : lastFrame) {
        uint8_t inv = ~b;
        fwrite(&inv, 1, 1, f);
    }
    fclose(f);
    return true;
}
#endif

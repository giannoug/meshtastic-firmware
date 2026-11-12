#pragma once
#include <cstdint>

uint32_t captureFrameCount();
uint32_t captureBlackPixels();
bool captureSavePbm(const char *path);

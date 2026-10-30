#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct DrawRec {
    enum Kind : uint8_t { TEXT, RECT, FILL, LINE } kind = TEXT;
    std::string owner; // Applet name
    std::string text;  // TEXT only: bytes as drawn (Win-1253)
    int16_t x = 0, y = 0, w = 0, h = 0;
    uint16_t color = 0;
};

void drawLogStart();
void drawLogStop();
const std::vector<DrawRec> &drawLog();
bool drawLogDump(const char *path);

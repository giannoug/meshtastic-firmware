// Host stand-in for the Arduino-ESP32 SD object T5Settings.cpp reads (card present, 32 GB, 1.2 GB used).
#pragma once
#include <cstdint>

#define CARD_NONE 0
#define CARD_SDHC 3

struct T5HostSD {
    uint8_t cardType() { return CARD_SDHC; }
    uint64_t totalBytes() { return 32ULL * 1000 * 1000 * 1000; }
    uint64_t usedBytes() { return 1200ULL * 1000 * 1000; }
};
static T5HostSD SD;

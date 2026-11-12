// Host stand-in for the ESP-IDF bits T5Settings.cpp reads on its developer page.
#pragma once
#include <cstdint>

struct T5HostEsp {
    uint32_t getFreeHeap() { return 182 * 1024; }
    uint32_t getMinFreeHeap() { return 141 * 1024; }
    uint32_t getFreePsram() { return 7 * 1024 * 1024; }
};
static T5HostEsp ESP;
inline int esp_reset_reason()
{
    return 1;
}

#pragma once
#include "MeshTypes.h"
#include <cstdint>

struct FixtureNode {
    NodeNum num;
    const char *longName; // Null: a node we have never had a NodeInfo from
    const char *shortName;
    int32_t meters; // < 0: no position
    float bearing;  // Degrees from north, from our position
    float snr;
    int8_t hops;      // < 0: unknown
    uint32_t agoSecs; // Last heard
    int16_t battery;  // < 0: no telemetry; 101 = powered
    bool favorite;
    uint8_t channel = 0;
};

uint32_t fixtureNow();
void fixtureNode(const FixtureNode &n);
void fixtureText(NodeNum from, NodeNum to, uint8_t channel, uint32_t agoSecs, const char *text);
void seedCanonical();
void seedMessy();

#pragma once
#include "ProtobufModule.h"
#include "concurrency/OSThread.h"
#include "graphics/Screen.h"
#include "graphics/SharedUIDisplay.h"
#include "input/InputBroker.h"
#if HAS_SCREEN
#include "OLEDDisplayUi.h"
#endif
#include <vector>

#define ROUTE_SIZE sizeof(((meshtastic_RouteDiscovery *)0)->route) / sizeof(((meshtastic_RouteDiscovery *)0)->route[0])

/*
  One decoded RouteDiscovery reply, kept with the identity of the request it answers.

  The route arrays are stored as they look after alterReceivedProtobuf() has run, so they already
  carry insertUnknownHops()' placeholders and this node's own appended SNR - the same data the
  text summary is built from, without a caller having to parse that text back apart.
*/
struct TraceRouteResult {
    uint32_t requestId = 0;    // decoded.request_id this reply answered
    NodeNum from = 0;          // node that replied
    uint32_t receivedAtMs = 0; // millis() when it was decoded
    meshtastic_RouteDiscovery route = meshtastic_RouteDiscovery_init_zero;
};

/**
 * A module that traces the route to a certain destination node
 */
enum TraceRouteRunState { TRACEROUTE_STATE_IDLE, TRACEROUTE_STATE_TRACKING, TRACEROUTE_STATE_RESULT, TRACEROUTE_STATE_COOLDOWN };

class TraceRouteModule : public ProtobufModule<meshtastic_RouteDiscovery>,
                         public Observable<const UIFrameEvent *>,
                         private concurrency::OSThread
{
  public:
    TraceRouteModule();

    bool startTraceRoute(NodeNum node);
    void launch(NodeNum node);
    void handleTraceRouteResult(const String &result);
    bool shouldDraw();
#if HAS_SCREEN
    virtual void drawFrame(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y) override;
#endif

    const char *getNodeName(NodeNum node);

    /* Read-only views of state this module already creates. None of them sends a packet, takes a
       measurement or advances a timer; each is safe to call at any time, including while idle. */

    /// Packet id of the most recent request sent by startTraceRoute(), or 0 if none has been sent.
    /// A fresh id per request, so a caller can tell a retrace to the same node from the one before it.
    uint32_t getLastRequestId() const { return lastRequestId; }
    /// Node the most recent request was addressed to, or 0 if none has been sent.
    NodeNum getLastRequestDest() const { return lastRequestDest; }

    /// Copies the most recently decoded reply into `out`. False when none has been decoded yet, in
    /// which case `out` is untouched. The reply keeps its own requestId, so a caller compares that
    /// against the id it started with rather than trusting the destination alone.
    bool getLastResult(TraceRouteResult &out) const;

    /// millis() at which a new request stops being refused by the cooldown, or 0 when nothing is
    /// pending. Reports the existing cooldown; it does not impose one.
    uint32_t getCooldownEndMs() const;

    virtual bool wantUIFrame() override { return shouldDraw(); }
    virtual Observable<const UIFrameEvent *> *getUIFrameObservable() override { return this; }

    void processUpgradedPacket(const meshtastic_MeshPacket &mp);

  protected:
    bool handleReceivedProtobuf(const meshtastic_MeshPacket &mp, meshtastic_RouteDiscovery *r) override;

    virtual meshtastic_MeshPacket *allocReply() override;

    /* Called before rebroadcasting a RouteDiscovery payload in order to update
       the route array containing the IDs of nodes this packet went through */
    void alterReceivedProtobuf(meshtastic_MeshPacket &p, meshtastic_RouteDiscovery *r) override;

    virtual int32_t runOnce() override;

  private:
    void setResultText(const String &text);
    void clearResultLines();
#if HAS_SCREEN
    void rebuildResultLines(OLEDDisplay *display);
#endif
    // Call to add unknown hops (e.g. when a node couldn't decrypt it) to the route based on hopStart and current hopLimit
    void insertUnknownHops(meshtastic_MeshPacket &p, meshtastic_RouteDiscovery *r, bool isTowardsDestination);

    // Call to add your ID to the route array of a RouteDiscovery message
    void appendMyIDandSNR(meshtastic_RouteDiscovery *r, float snr, bool isTowardsDestination, bool SNRonly);

    // Update next-hops in the routing table based on the returned route
    void updateNextHops(const meshtastic_MeshPacket &p, meshtastic_RouteDiscovery *r);

    // Helper to update next-hop for a single node
    void maybeSetNextHop(NodeNum target, uint8_t nextHopByte);

    /* Call to print the route array of a RouteDiscovery message.
       Set origin to where the request came from.
       Set dest to the ID of its destination, or NODENUM_BROADCAST if it has not yet arrived there. */
    void printRoute(meshtastic_RouteDiscovery *r, uint32_t origin, uint32_t dest, bool isTowardsDestination);

    TraceRouteRunState runState = TRACEROUTE_STATE_IDLE;
    unsigned long lastTraceRouteTime = 0;
    unsigned long resultShowTime = 0;
    unsigned long cooldownMs = 30000;
    unsigned long resultDisplayMs = 10000;
    unsigned long trackingTimeoutMs = 10000;
    String bannerText;
    String resultText;
    std::vector<String> resultLines;
    bool resultLinesDirty = false;
    NodeNum tracingNode = 0;
    bool initialized = false;

    // Identity of the last request sent, captured where it is generated, and the last reply
    // decoded, captured where it is decoded. Retained so a UI can redraw a route it has already
    // been shown; nothing in this module reads them back, so routing, cooldown and the tracking
    // timeout behave exactly as they did before.
    uint32_t lastRequestId = 0;
    NodeNum lastRequestDest = 0;
    bool hasResult = false;
    TraceRouteResult result;
};

extern TraceRouteModule *traceRouteModule;

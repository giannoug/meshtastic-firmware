#if defined(T5_S3_EPAPER_PRO) && defined(MESHTASTIC_INCLUDE_INKHUD)

/*

Trace Route: the frozen states T1-T8, plus K3 / K4 in Console (Apps 09).

The module sends the request, enforces its own cooldown, decodes the reply, and now keeps both the
id it sent and the route it decoded. What lives here is the part the frozen screen promises and the
module does not: a 60 second reply window.

The module gives up at 10 seconds - it clears its own target and stops recognising a reply as a
result. Ours is matched on request id instead, which the module retains, so a reply arriving after
that is still ours and still lands on the right target. That is what makes the frozen "a late reply
will still appear here" true rather than decorative.

A late reply never brings this screen forward. It updates state; the snapshot rules decide whether
anything is drawn (Apps 12, 26).

*/

#include "./T5AppsApplet.h"
#include "./T5Text.h"
#include "./T5TracePickTask.h"

#include "UptimeClock.h"
#include "gps/RTC.h"
#include "mesh/NodeDB.h"
#include "mesh/Throttle.h"

using namespace NicheGraphics;

namespace
{
using std::to_string;

// Frozen: TRACING... after send, final NO REPLY at 60 s. Measured from millis(), which keeps running
// through a display timeout, so this is elapsed real time and not time awake (Apps 14)
constexpr uint32_t REPLY_WINDOW_MS = 60000;
constexpr int16_t SUBJECT_W = 340; // Console: subject pane, then two equal route panes (Apps K3)

// Quarter-dB int8, with INT8_MIN meaning the node reported none (Apps 09). The frozen copy uses a
// typographic minus; the fonts have no glyph for it, so a hyphen stands in
std::string snrText(const int8_t *list, pb_size_t count, pb_size_t index)
{
    if (index >= count || list[index] == INT8_MIN)
        return InkHUD::T5Applet::DASH;
    char out[16];
    snprintf(out, sizeof(out), "%+.1f dB", (double)list[index] / 4.0);
    return out;
}

// hh:mm:ss of a wall-clock second, or empty without a clock
std::string clockSecs(uint32_t when)
{
    if (!when)
        return "";
    const uint32_t hms = when % SEC_PER_DAY;
    char out[12];
    snprintf(out, sizeof(out), "%02u:%02u:%02u", (unsigned)(hms / SEC_PER_HOUR), (unsigned)((hms % SEC_PER_HOUR) / SEC_PER_MIN),
             (unsigned)(hms % SEC_PER_MIN));
    return out;
}
} // namespace

uint32_t InkHUD::T5AppsApplet::traceDeadlineMs() const
{
    return REPLY_WINDOW_MS;
}

// ---------------------------------------------------------------- the picker's answers

void InkHUD::T5AppsApplet::targetPicked(NodeNum num)
{
    T5AppsApplet *self = get();
    if (!self || !num)
        return;
    // A new target drops the previous result rather than showing it under the wrong name
    if (num != self->traceTarget) {
        self->haveResult = false;
        self->traceResult = {};
        self->traceReplyMs = 0;
        self->traceRequestId = 0; // Stop waiting for a reply that belongs to the old target
    }
    self->traceTarget = num;
    // Choosing a target does not start the trace: the frozen flow puts START TRACE on T3
    if (self->traceState != Trace::TRACING)
        self->traceState = self->traceBlocked() ? Trace::BLOCKED : Trace::READY;
    self->mode = Mode::TRACE;
    self->requestUpdate();
}

void InkHUD::T5AppsApplet::pickCancelled()
{
    if (T5AppsApplet *self = get())
        self->requestUpdate(); // The tool is unchanged; the screen still has to come back
}

// ---------------------------------------------------------------- sending

void InkHUD::T5AppsApplet::startTrace()
{
    if (!traceRouteModule || !traceTarget)
        return;
    if (traceBlocked()) {
        traceState = Trace::BLOCKED;
        requestUpdate();
        return;
    }

    // False means the module refused: a cooldown still running, or a trace already in flight. Either
    // way the frozen screen keeps what it was showing and says when it can run again
    if (!traceRouteModule->startTraceRoute(traceTarget)) {
        requestUpdate();
        return;
    }

    traceRequestId = traceRouteModule->getLastRequestId();
    traceSentAtMs = Time::stampMillis();
    traceSentAtTime = getValidTime(RTCQuality::RTCQualityDevice, true);
    traceState = Trace::TRACING;
    haveResult = false; // A fresh request: the previous route is no longer what we are showing
    traceResult = {};
    traceReplyMs = 0;
    setIntervalFromNow(1000); // Watch for the reply and for the deadline
    requestUpdate();
}

// Take the module's retained result, but only if it answers the request we are waiting for. This is
// the whole identity rule: not "a reply from this node", which a stale reply to an earlier request
// to the same node would also satisfy (Apps 09)
void InkHUD::T5AppsApplet::adoptResult()
{
    if (!traceRouteModule || !traceRequestId)
        return;

    TraceRouteResult got;
    if (!traceRouteModule->getLastResult(got))
        return;
    if (got.requestId != traceRequestId)
        return; // Someone else's request, or one of ours we are no longer waiting for
    if (haveResult && traceResult.requestId == got.requestId)
        return; // Already taken: nothing new, so nothing to redraw

    traceResult = got;
    haveResult = true;
    traceReplyMs = got.receivedAtMs - traceSentAtMs;
    traceResultAtTime = getValidTime(RTCQuality::RTCQualityDevice, true);
    // RESULT even if we had already given up: the frozen T6 promises a late reply still appears
    traceState = Trace::RESULT;

    // Deliberately no requestFocus() and no showApplet(): a reply changes what this screen would
    // draw, never which screen is in front (Apps 12)
    if (isForeground() && mode == Mode::TRACE)
        requestUpdate();
}

// The one-line form the launcher card and T8's LAST RESULT both use
std::string InkHUD::T5AppsApplet::traceSummary() const
{
    if (!haveResult)
        return "No trace to this node yet this session.";
    char out[96];
    snprintf(out, sizeof(out), "%u hops towards %s %u hops back %s reply in %.1f s",
             (unsigned)(traceResult.route.route_count + 1), SEP, (unsigned)(traceResult.route.route_back_count + 1), SEP,
             (double)traceReplyMs / 1000.0);
    return out;
}

// ---------------------------------------------------------------- route rendering

// Towards is us, the relays the request passed, then the target. Back is the target, the relays the
// reply passed, then us. Shown separately and never merged: the paths can differ each way (Apps T5)
void InkHUD::T5AppsApplet::drawRouteSections(int16_t &y, bool console)
{
    const meshtastic_RouteDiscovery &r = traceResult.route;
    const NodeNum me = nodeDB->getNodeNum();

    auto drawLeg = [&](int16_t &ly, bool towards) {
        const uint32_t *route = towards ? r.route : r.route_back;
        const pb_size_t count = towards ? r.route_count : r.route_back_count;
        const int8_t *snr = towards ? r.snr_towards : r.snr_back;
        const pb_size_t snrCount = towards ? r.snr_towards_count : r.snr_back_count;
        const NodeNum last = towards ? traceTarget : me;

        std::string label;
        if (towards)
            label = console ? "TOWARDS " + std::string(SEP) + " " + to_string(count + 1) + " HOPS"
                            : "TOWARDS " + t5NodeTitle(traceTarget) + " " + SEP + " " + to_string(count + 1) + " HOPS";
        else
            label = console ? "BACK " + std::string(SEP) + " " + to_string(count + 1) + " HOPS"
                            : "BACK TO YOU " + std::string(SEP) + " " + to_string(count + 1) + " HOPS";
        rowSection(ly, label);

        // Index 0 is the leg's origin and carries no SNR: nothing heard it before the sender
        const std::string firstSub = towards ? "YOU " + std::string(SEP) + " " + t5NodeTag(me) : t5NodeTag(traceTarget);
        rowHop(ly, 0, towards ? t5NodeTitle(me) : t5NodeTitle(traceTarget),
               console ? (towards ? "YOU" : t5NodeTag(traceTarget)) : firstSub, "", HOP_END);

        for (pb_size_t i = 0; i < count; i++) {
            const bool unknown = route[i] == NODENUM_BROADCAST;
            rowHop(ly, (uint8_t)(i + 1), unknown ? "Unknown relay" : t5NodeTitle(route[i]),
                   unknown ? (console ? "NO KEY" : "could not decode " + std::string(SEP) + " no channel key")
                           : t5NodeTag(route[i]),
                   snrText(snr, snrCount, i), unknown ? HOP_UNK : HOP_MID);
        }

        // The leg's far end: the target on the way out, us on the way back
        const std::string lastSub = towards ? t5NodeTag(traceTarget) + " " + SEP + " TARGET"
                                            : (console ? "YOU" : "YOU " + std::string(SEP) + " " + t5NodeTag(me));
        rowHop(ly, (uint8_t)(count + 1), t5NodeTitle(last), lastSub, snrText(snr, snrCount, count), HOP_END);
    };

    if (!console) {
        drawLeg(y, true);
        drawLeg(y, false);
        return;
    }

    // Console puts the two legs side by side in the panes beyond the subject
    const int16_t paneW = (width() - SUBJECT_W - 2) / 2;
    int16_t ly = headerHeight();
    setPane(SUBJECT_W + 2, paneW);
    drawLeg(ly, true);
    clearPane();
    ly = headerHeight();
    setPane(SUBJECT_W + 2 + paneW, paneW);
    drawLeg(ly, false);
    clearPane();
    drawLine(SUBJECT_W + 2 + paneW, headerHeight(), SUBJECT_W + 2 + paneW, navTop() - 1, BLACK);
}

// RUN AGAIN shows the clock time the cooldown ends rather than counting down: one frame when it
// becomes available, none in between (Apps T5)
void InkHUD::T5AppsApplet::drawResultActions(int16_t &y)
{
    const uint32_t end = traceRouteModule ? traceRouteModule->getCooldownEndMs() : 0;
    const bool cooling = end && (int32_t)(end - Time::stampMillis()) > 0;

    if (cooling) {
        // Wall clock of the moment it clears, derived from how much of the cooldown is left
        const uint32_t leftMs = end - Time::stampMillis();
        const uint32_t now = getValidTime(RTCQuality::RTCQualityDevice, true);
        const std::string when = now ? "FROM " + clockSecs(now + leftMs / 1000) : "COOLING DOWN";
        rowAction(y, Hit::NONE, "RUN AGAIN", false, false, when);
    } else
        rowAction(y, Hit::RUN_AGAIN, "RUN AGAIN", false);
    rowAction(y, Hit::NEW_TARGET, "NEW TARGET", false);
}

// ---------------------------------------------------------------- render

void InkHUD::T5AppsApplet::renderTrace()
{
    // A blocked radio outranks whatever the tool was showing: it cannot send, and T8 says where to
    // fix that. The target and the last result stay on screen and stay usable
    if (traceBlocked())
        traceState = Trace::BLOCKED;
    else if (traceState == Trace::BLOCKED)
        traceState = traceTarget ? (haveResult ? Trace::RESULT : Trace::READY) : Trace::NO_TARGET;

    if (isConsole())
        renderTraceConsole();
    else
        renderTraceCarry();
}

void InkHUD::T5AppsApplet::renderTraceCarry()
{
    std::string context;
    switch (traceState) {
    case Trace::NO_TARGET:
        context = "SEE THE PATH A PACKET TAKES";
        break;
    case Trace::READY:
        context = "READY";
        break;
    case Trace::TRACING:
        context = "SENT " + clockSecs(traceSentAtTime) + " " + SEP + " TRACING" + ELLIPSIS;
        break;
    case Trace::RESULT: {
        char reply[24];
        snprintf(reply, sizeof(reply), "REPLY IN %.1f S", (double)traceReplyMs / 1000.0);
        context = t5NodeTitle(traceTarget) + " " + SEP + " " + reply + " " + SEP + " " + clockSecs(traceSentAtTime);
        break;
    }
    case Trace::NO_REPLY:
        context = "SENT " + clockSecs(traceSentAtTime) + " " + SEP + " NO REPLY";
        break;
    case Trace::BLOCKED:
        context = "CANNOT SEND";
        break;
    }
    drawHeader("TRACE ROUTE", context, "APPS");
    int16_t y = headerHeight();

    switch (traceState) {
    case Trace::NO_TARGET:
        rowBig(y, "Pick a node to trace",
               "Shows every relay between you and the node, in both directions, with the signal each relay heard.", 80);
        rowAction(y, Hit::CHOOSE_NODE, "CHOOSE NODE", false);
        rowNote(y, "Each trace sends one packet over the mesh. One trace every 30 seconds.");
        break;

    case Trace::READY:
        rowTarget(y, true, false);
        rowAction(y, Hit::START_TRACE, "START TRACE", true);
        rowSection(y, "LAST RESULT");
        rowNote(y, traceSummary());
        break;

    case Trace::TRACING:
        rowTarget(y, false, false); // Locked while tracing: no CHANGE
        rowBig(y, std::string("Tracing") + ELLIPSIS,
               "Sent at " + clockSecs(traceSentAtTime) + ". The result or \"no reply\" appears here. You can leave " + DASH +
                   " the trace keeps running.",
               40);
        rowAction(y, Hit::NONE, "START TRACE", false, false, "IN PROGRESS");
        break;

    case Trace::RESULT:
        drawRouteSections(y, false);
        drawResultActions(y);
        break;

    case Trace::NO_REPLY:
        rowTarget(y, true, false);
        rowBig(y, "No reply", "Nothing came back within 60 seconds. A late reply will still appear here.", 30);
        rowBullet(y, "The node may be out of range or switched off.");
        rowBullet(y, "A relay may have dropped the request or the reply.");
        rowBullet(y, "Routers and repeaters answer; some roles may not.");
        rowAction(y, Hit::RUN_AGAIN, "RUN AGAIN", true);
        break;

    case Trace::BLOCKED: {
        const bool noRegion = config.lora.region == meshtastic_Config_LoRaConfig_RegionCode_UNSET;
        rowBanner(y, noRegion ? "REGION NOT SET" : "TRANSMIT IS OFF",
                  noRegion ? "Trace Route sends a packet. Choose a region in MENU " + std::string(FWD) + " RADIO."
                           : "Trace Route sends a packet. Turn transmit on in MENU " + std::string(FWD) + " RADIO.");
        if (traceTarget) {
            rowTarget(y, true, false);
            rowAction(y, Hit::NONE, "START TRACE", false, false, noRegion ? "NO REGION" : "TRANSMIT OFF");
            if (haveResult) {
                rowSection(y, "LAST RESULT " + std::string(SEP) + " " + t5ListTime(traceResultAtTime, false));
                rowNote(y, traceSummary());
            }
        } else {
            rowAction(y, Hit::CHOOSE_NODE, "CHOOSE NODE", false);
        }
        break;
    }
    }
    drawNav();
}

// Console: the subject on the left, the two directions side by side (Apps K3, K4)
void InkHUD::T5AppsApplet::renderTraceConsole()
{
    std::string context;
    switch (traceState) {
    case Trace::NO_TARGET:
        context = "SEE THE PATH A PACKET TAKES";
        break;
    case Trace::READY:
        context = "READY";
        break;
    case Trace::TRACING:
        context = std::string("TRACING") + ELLIPSIS;
        break;
    case Trace::RESULT: {
        char reply[24];
        snprintf(reply, sizeof(reply), "REPLY IN %.1f S", (double)traceReplyMs / 1000.0);
        context = reply;
        break;
    }
    case Trace::NO_REPLY:
        context = "NO REPLY";
        break;
    case Trace::BLOCKED:
        context = "CANNOT SEND";
        break;
    }
    drawHeader("TRACE ROUTE", context, "APPS");

    int16_t y = headerHeight();
    setPane(0, SUBJECT_W);
    if (traceTarget)
        rowTarget(y, traceState != Trace::TRACING, true);
    switch (traceState) {
    case Trace::NO_TARGET:
        rowBig(y, "Pick a node", "Every relay between you and the node, both ways.", 20);
        rowAction(y, Hit::CHOOSE_NODE, "CHOOSE NODE", false);
        break;
    case Trace::READY:
        rowInfo(y, "STATUS", "READY");
        rowAction(y, Hit::START_TRACE, "START TRACE", true);
        break;
    case Trace::TRACING:
        rowInfo(y, "STATUS", std::string("TRACING") + ELLIPSIS);
        rowInfo(y, "SENT", clockSecs(traceSentAtTime));
        rowAction(y, Hit::NONE, "START TRACE", false, false, "IN PROGRESS");
        break;
    case Trace::RESULT: {
        char reply[16];
        snprintf(reply, sizeof(reply), "%.1f S", (double)traceReplyMs / 1000.0);
        rowInfo(y, "STATUS", "REPLIED");
        rowInfo(y, "SENT", clockSecs(traceSentAtTime));
        rowInfo(y, "REPLY TIME", reply);
        drawResultActions(y);
        break;
    }
    case Trace::NO_REPLY:
        rowInfo(y, "STATUS", "NO REPLY");
        rowInfo(y, "SENT", clockSecs(traceSentAtTime));
        rowAction(y, Hit::RUN_AGAIN, "RUN AGAIN", true);
        break;
    case Trace::BLOCKED:
        rowInfo(y, "STATUS", "CANNOT SEND");
        rowAction(y, Hit::NONE, "START TRACE", false, false,
                  config.lora.region == meshtastic_Config_LoRaConfig_RegionCode_UNSET ? "NO REGION" : "TRANSMIT OFF");
        break;
    }
    clearPane();
    drawLine(SUBJECT_W, headerHeight(), SUBJECT_W, navTop() - 1, BLACK);

    if (traceState == Trace::RESULT) {
        int16_t ry = y;
        drawRouteSections(ry, true);
    } else {
        // Nothing known yet, so the two route panes merge into one message area (Apps K4)
        const int16_t left = SUBJECT_W + 2;
        setPane(left, width() - left);
        int16_t my = headerHeight() + 60;
        const bool tracing = traceState == Trace::TRACING;
        rowBig(my, tracing ? std::string("Tracing") + ELLIPSIS : "No route yet",
               tracing
                   ? "The route appears here in both directions. You can leave " + std::string(DASH) + " the trace keeps running."
                   : "The route appears here in both directions, with the signal each relay heard.",
               0);
        clearPane();
    }
    drawNav();
}

#endif

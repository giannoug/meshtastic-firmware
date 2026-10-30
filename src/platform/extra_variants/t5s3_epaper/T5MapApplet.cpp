#if defined(T5_S3_EPAPER_PRO) && defined(MESHTASTIC_INCLUDE_INKHUD)

#include "./T5MapApplet.h"
#include "./T5NodeDetailApplet.h"
#include "./T5Text.h"

#include "GPSStatus.h"
#include "gps/GeoCoord.h"
#include "gps/RTC.h"
#include "mesh/NodeDB.h"
#include "mesh/mesh-pb-constants.h"
#include "meshUtils.h"

#include <algorithm>
#include <cmath>

using namespace NicheGraphics;

namespace
{
constexpr float MI = 1609.344f;      // Metres in a statute mile
constexpr double M_PER_DEG = 111320; // Metres in a degree of latitude

// Radius the sender's reduced precision leaves us, in metres. precision_bits truncates latitude_i to that
// many bits and centres the result in the cell, so the error is half a cell on the latitude axis, which
// stays near-constant worldwide (PositionPrecision.h).
float precisionRadius(uint32_t bits)
{
    if (bits == 0 || bits >= 32)
        return 0;
    return (float)((double)(1UL << (31 - bits)) * 1e-7 * M_PER_DEG);
}

bool imperial()
{
    return config.display.units == meshtastic_Config_DisplayConfig_DisplayUnits_IMPERIAL;
}
} // namespace

// The frozen ladder, named by the distance to the first ring. Ring 2 sits at twice that, and both labels
// are stored rather than derived: every doubling is a round number in both unit systems, and a table
// cannot round one of them wrong (Map 06).
const InkHUD::T5MapApplet::Rung InkHUD::T5MapApplet::LADDER[] = {
    {100, 0.1f * MI, "100 m", "200 m", "0.1 mi", "0.2 mi"}, {250, 0.25f * MI, "250 m", "500 m", "0.25 mi", "0.5 mi"},
    {500, 0.5f * MI, "500 m", "1 km", "0.5 mi", "1 mi"},    {1000, 1 * MI, "1 km", "2 km", "1 mi", "2 mi"},
    {2000, 2 * MI, "2 km", "4 km", "2 mi", "4 mi"},         {5000, 5 * MI, "5 km", "10 km", "5 mi", "10 mi"},
    {10000, 10 * MI, "10 km", "20 km", "10 mi", "20 mi"},   {25000, 25 * MI, "25 km", "50 km", "25 mi", "50 mi"},
    {50000, 50 * MI, "50 km", "100 km", "50 mi", "100 mi"}, {100000, 100 * MI, "100 km", "200 km", "100 mi", "200 mi"},
};

InkHUD::T5MapApplet::T5MapApplet()
    : SinglePortModule("T5MapApplet", meshtastic_PortNum_POSITION_APP), concurrency::OSThread("T5Map")
{
    hideFromAppSwitcher = true; // Reached from the MAP nav cell
}

void InkHUD::T5MapApplet::begin()
{
    const int8_t i = indexOf("Map");
    assert(i >= 0);
    static_cast<T5MapApplet *>(InkHUD::getInstance()->userApplets[i])->setIntervalFromNow(60 * 1000);
}

float InkHUD::T5MapApplet::rungMetres(uint8_t r) const
{
    return imperial() ? LADDER[r].imperial : LADDER[r].metric;
}

const char *InkHUD::T5MapApplet::rungLabel(uint8_t r, bool second) const
{
    const Rung &g = LADDER[r];
    return imperial() ? (second ? g.i2 : g.i1) : (second ? g.m2 : g.m1);
}

// Frozen device pixels for each band, per mode. Not derived from one another, so a change to one cannot
// silently move the rest (Map 02, 10).
InkHUD::T5MapApplet::Layout InkHUD::T5MapApplet::layout() const
{
    Layout l{};
    l.console = width() > height();
    if (l.console) {
        l.mapTop = 64;
        l.mapW = 610;
        l.mapH = 291;
        l.cx = 305;
        l.cy = l.mapTop + 146;
        l.stripTop = 355;
        l.stripW = 610;
        l.stripH = 64;
        l.panelLeft = 612;
        l.panelTop = 64;
        l.panelW = 316;
        l.actionLeft = 612;
        l.actionTop = 355;
        l.actionW = 316;
        l.pxPerStep = 64;
    } else {
        l.mapTop = 100;
        l.mapW = 508;
        l.mapH = 484;
        l.cx = 254;
        l.cy = l.mapTop + 242;
        l.stripTop = 584;
        l.stripW = 508;
        l.stripH = 72;
        l.panelLeft = 0;
        l.panelTop = 656;
        l.panelW = 508;
        l.actionLeft = 0;
        l.actionTop = 775;
        l.actionW = 508;
        l.pxPerStep = 100;
    }
    return l;
}

void InkHUD::T5MapApplet::onForeground()
{
    T5Applet::onForeground();
    // Nav MAP, Home and a first entry all arrive with nothing preset, and all reset the view. A deep link
    // and Node detail's back chip set the view themselves, and clear the flag on the way in (Map 06, 07).
    if (!viewPreset)
        resetView();
    viewPreset = false;
}

void InkHUD::T5MapApplet::resetView()
{
    fit = true;
    anchor = ME;
    anchorNode = 0;
    backNode = 0;
    deepTarget = 0;
    choosing = false;
    chooser.clear();
    centred = false; // settleCentre picks us, or the node centroid, on the next render
    centreIsOwn = false;
    order.clear(); // Re-snapshotted against the model reconcile() is about to read
}

// A position packet is only worth a refresh when it would change what is on screen. The packet's own
// coordinates are compared against the ones the last frame drew, so this does not depend on whether
// nodeDB has already stored them. Never requestAutoshow(): the foreground changes only when the user
// navigates (Map 08).
ProcessMessage InkHUD::T5MapApplet::handleReceived(const meshtastic_MeshPacket &mp)
{
    if (!isForeground() || !isActive())
        return ProcessMessage::CONTINUE;

    meshtastic_Position position = meshtastic_Position_init_default;
    if (mp.which_payload_variant != meshtastic_MeshPacket_decoded_tag ||
        !pb_decode_from_bytes(mp.decoded.payload.bytes, mp.decoded.payload.size, &meshtastic_Position_msg, &position) ||
        !position.has_latitude_i || !position.has_longitude_i || (position.latitude_i == 0 && position.longitude_i == 0)) {
        requestUpdate(); // Not a position we can measure: let the next frame settle it
        return ProcessMessage::CONTINUE;
    }

    const double lat = position.latitude_i * 1e-7, lon = position.longitude_i * 1e-7;

    if (mp.from == nodeDB->getNodeNum()) {
        // Our own: moves under 50 m are ignored, the same threshold PositionsApplet uses
        if (own.valid() && GeoCoord::latLongToMeter(lat, lon, own.lat, own.lon) < OWN_MOVE_M)
            return ProcessMessage::CONTINUE;
        requestUpdate();
        return ProcessMessage::CONTINUE;
    }

    // Another node: skip when it has moved less than 6 px on screen and its freshness is unchanged. A
    // packet always refreshes the panel's age and hop facts for the selection, so that one never skips.
    const Plot *p = find(mp.from);
    if (p && p->num != selected && p->age != AGE_UNKNOWN && p->age < STALE_SECS) {
        const float metresPerPx = rungMetres(rung) / layout().pxPerStep;
        if (GeoCoord::latLongToMeter(lat, lon, p->lat, p->lon) / metresPerPx < MOVED_PX)
            return ProcessMessage::CONTINUE;
    }
    requestUpdate();
    return ProcessMessage::CONTINUE;
}

// One logical cadence for freshness, not a timer per node. Nothing is recomputed here: onRender reads
// every age from the clock, so the tick only has to ask for the frame. While the display timeout has the
// UI gated the request would sit pending anyway, so it is not even made (Map 08).
int32_t InkHUD::T5MapApplet::runOnce()
{
    if (isForeground() && !T5UiGate::snapshotHeld())
        requestUpdate();
    return 60 * 1000;
}

// ---- model -------------------------------------------------------------------------------------------

const InkHUD::T5MapApplet::Plot *InkHUD::T5MapApplet::find(NodeNum num) const
{
    for (const Plot &p : plots)
        if (p.num == num)
            return &p;
    return nullptr;
}

// The ordinary set: everything discovery, FIT, PREV / NEXT and automatic selection may use
bool InkHUD::T5MapApplet::selectable(const Plot &p) const
{
    return !p.old;
}

// Drawn on the map: the ordinary set, plus an explicit SHOW ON MAP target while it stays selected. Such a
// target is never refused, however old its position, and leaves the map once deselected (Map 03, E10).
bool InkHUD::T5MapApplet::drawn(const Plot &p) const
{
    return selectable(p) || (p.num == deepTarget && p.num == selected);
}

int16_t InkHUD::T5MapApplet::selectionIndex() const
{
    for (uint16_t i = 0; i < order.size(); i++)
        if (order[i] == selected)
            return i;
    return -1;
}

// "6 / 10". A deep-linked old node, or one with no position, sits outside the order and reads "\x97 / 10"
std::string InkHUD::T5MapApplet::orderLabel() const
{
    const int16_t i = selectionIndex();
    return (i < 0 ? std::string(DASH) : to_string(i + 1)) + " / " + to_string(order.size());
}

// Our own position, and which of the frozen four states it puts us in (Map 09). Never a fabricated
// coordinate: with nothing usable the map centres on the nodes instead.
void InkHUD::T5MapApplet::readOwn()
{
    own = Own{};
    const NodeNum ourNum = nodeDB->getNodeNum();

    // The live local position first, then the one nodeDB kept from our last fix
    double lat = 0, lon = 0;
    bool have = false;
    if (localPosition.latitude_i != 0 || localPosition.longitude_i != 0) {
        lat = localPosition.latitude_i * 1e-7;
        lon = localPosition.longitude_i * 1e-7;
        have = true;
        own.manual = localPosition.location_source == meshtastic_Position_LocSource_LOC_MANUAL;
        own.phone = localPosition.location_source == meshtastic_Position_LocSource_LOC_EXTERNAL;
        own.fixTime = localPosition.timestamp ? localPosition.timestamp : localPosition.time;
    } else {
        meshtastic_PositionLite stored;
        if (nodeDB->copyNodePosition(ourNum, stored) && (stored.latitude_i != 0 || stored.longitude_i != 0)) {
            lat = stored.latitude_i * 1e-7;
            lon = stored.longitude_i * 1e-7;
            have = true;
            own.manual = stored.location_source == meshtastic_Position_LocSource_LOC_MANUAL;
            own.phone = stored.location_source == meshtastic_Position_LocSource_LOC_EXTERNAL;
            own.fixTime = stored.time;
        }
    }
    if (!have)
        return;

    own.lat = lat;
    own.lon = lon;
    // A fixed or phone position is as live as it gets; a GPS one is only live while the receiver has
    // lock. Without lock the ring turns dashed and distances are prefixed, but the map does not move.
    const bool gpsBacked = !own.manual && !own.phone;
    own.kind =
        (gpsBacked && !gpsStatus->getHasLock() && config.position.gps_mode == meshtastic_Config_PositionConfig_GpsMode_ENABLED)
            ? OWN_LAST_FIX
            : OWN_LIVE;
}

// Rebuilds the model from nodeDB. Age comes from PositionLite.time when it is set and the clock is valid,
// otherwise from last_heard, which the panel marks with "~" (Map 03, 10).
void InkHUD::T5MapApplet::readPlots()
{
    plots.clear();
    noPosition = staleCount = oldCount = beyondCount = 0;

    const NodeNum ourNum = nodeDB->getNodeNum();
    const uint32_t now = getValidTime(RTCQuality::RTCQualityDevice);
    const float beyondM = BEYOND_STEPS * rungMetres(RUNGS - 1);

    for (uint32_t i = 0; i < nodeDB->getNumMeshNodes(); i++) {
        meshtastic_NodeInfoLite *node = nodeDB->getMeshNodeByIndex(i);
        if (!node || node->num == ourNum)
            continue;

        meshtastic_PositionLite pos;
        if (!nodeDB->hasValidPosition(node) || !nodeDB->copyNodePosition(node->num, pos) ||
            (pos.latitude_i == 0 && pos.longitude_i == 0)) {
            noPosition++;
            continue;
        }

        Plot p;
        p.num = node->num;
        p.lat = pos.latitude_i * 1e-7;
        p.lon = pos.longitude_i * 1e-7;
        p.precision = pos.precision_bits == 0 ? 32 : pos.precision_bits;
        p.hops = node->has_hops_away ? (int16_t)node->hops_away : -1;
        p.favorite = nodeInfoLiteIsFavorite(node);
        if (node->last_heard && now)
            p.heard = sinceLastSeen(node);

        // Without a valid clock, freshness is unknown: the node is drawn solid and the panel shows "?"
        // rather than a made-up age (state 14)
        if (now) {
            if (pos.time && pos.time <= now) {
                p.age = now - pos.time;
            } else if (node->last_heard) {
                p.age = p.heard;
                p.approxAge = true;
            }
        }
        if (p.age != AGE_UNKNOWN) {
            p.stale = p.age >= STALE_SECS;
            p.old = p.age > OLD_SECS;
        }

        if (own.valid()) {
            p.distance = GeoCoord::latLongToMeter(p.lat, p.lon, own.lat, own.lon);
            p.degrees = lround(GeoCoord::toDegrees(GeoCoord::bearing(own.lat, own.lon, p.lat, p.lon)) + 360) % 360;
            p.beyond = p.distance > beyondM;
        }

        if (p.old)
            oldCount++;
        else if (p.stale)
            staleCount++;
        if (p.beyond && !p.old)
            beyondCount++;
        plots.push_back(p);
    }
}

// Exactly one node is selected whenever at least one selectable positioned node exists. First match wins:
// deep-link target, nearest fresh, most recently heard fresh, newest stale, none (Map 05, D4).
void InkHUD::T5MapApplet::settleSelection()
{
    // A deep-link target stands whatever its age, and a target with no position at all keeps the panel
    // that explains why it isn't on the map (E4, E10)
    if (deepTarget && selected == deepTarget && nodeDB->getMeshNode(deepTarget))
        return;

    const Plot *held = find(selected);
    if (held && selectable(*held))
        return; // Still a legitimate selection

    selected = 0;
    const Plot *best = nullptr;

    // 2 · nearest fresh, when we know where we are
    for (const Plot &p : plots)
        if (!p.stale && !p.old && p.distance >= 0 && (!best || p.distance < best->distance))
            best = &p;

    // 3 · most recently heard fresh, when distance cannot be computed
    if (!best)
        for (const Plot &p : plots)
            if (!p.stale && !p.old && (!best || p.heard < best->heard))
                best = &p;

    // 4 · newest stale, when no fresh node is left
    if (!best)
        for (const Plot &p : plots)
            if (!p.old && (!best || p.age < best->age))
                best = &p;

    if (best)
        selected = best->num;
}

// Distance ascending from us, snapshotted on entry and on FIT and never re-sorted live, so NEXT never
// jumps because someone moved. Without our own position, the selection fallback order stands in: fresh by
// last heard, then stale by last heard. A new node is appended until the next snapshot (Map 05).
void InkHUD::T5MapApplet::settleOrder()
{
    if (order.empty()) {
        std::vector<const Plot *> sorted;
        for (const Plot &p : plots)
            if (selectable(p))
                sorted.push_back(&p);

        const bool byDistance = own.valid();
        std::sort(sorted.begin(), sorted.end(), [byDistance](const Plot *a, const Plot *b) {
            if (byDistance && a->distance >= 0 && b->distance >= 0)
                return a->distance < b->distance;
            if (a->stale != b->stale)
                return !a->stale; // Fresh before stale
            if (a->heard != b->heard)
                return a->heard < b->heard;
            return a->num < b->num; // Deterministic tie-break, so the order never depends on nodeDB order
        });

        order.clear();
        for (const Plot *p : sorted)
            order.push_back(p->num);
        return;
    }

    // Keep the snapshot, minus anything no longer selectable, plus anything new at the end
    std::vector<NodeNum> kept;
    for (NodeNum num : order) {
        const Plot *p = find(num);
        if (p && selectable(*p))
            kept.push_back(num);
    }
    for (const Plot &p : plots) {
        if (!selectable(p))
            continue;
        if (std::find(kept.begin(), kept.end(), p.num) == kept.end())
            kept.push_back(p.num);
    }
    order = std::move(kept);
}

// Centre = us, or the anchor node. With no own position it falls back to the middle of the positioned
// nodes, as the shared base applet does. While anchored on us the view re-centres only once our marker
// has drifted past a quarter of the view, and moves under 50 m are ignored altogether (Map 06).
void InkHUD::T5MapApplet::settleCentre(const Layout &l)
{
    if (anchor == NODE) {
        const Plot *a = find(anchorNode);
        if (a && drawn(*a)) {
            centreLat = a->lat;
            centreLon = a->lon;
            centred = true;
            centreIsOwn = false;
            return;
        }
        anchor = ME; // The anchor node left the map: fall back to us
    }

    if (own.valid()) {
        // The hysteresis only applies to a centre that is already ours. Arriving from the node centroid,
        // from an anchor node, or from a first-ever fix, the view moves to us at once - otherwise a
        // centroid that happened to sit near us would keep the centre (Map 06, redraw table).
        // Dropping centred is enough to ask for a fresh centre anywhere: no caller has to clear both
        const bool holds = centred && centreIsOwn;
        const float drift =
            holds ? GeoCoord::latLongToMeter(own.lat, own.lon, centreLat, centreLon) / (rungMetres(rung) / l.pxPerStep) : 0;
        if (!holds || drift > RECENTRE_FRACTION * std::min(l.mapW, l.mapH)) {
            centreLat = own.lat;
            centreLon = own.lon;
            centred = true;
            centreIsOwn = true;
        }
        return;
    }

    // No own position: the centroid of everything drawn, then nothing at all if that is empty
    double sumLat = 0, sumLon = 0;
    uint16_t n = 0;
    for (const Plot &p : plots)
        if (drawn(p)) {
            sumLat += p.lat;
            sumLon += p.lon;
            n++;
        }
    centreLat = n ? sumLat / n : 0;
    centreLon = n ? sumLon / n : 0;
    centred = n > 0;
    centreIsOwn = false;
}

// The smallest ladder step where every fresh plotted node fits with a 24 px margin. With no fresh node,
// stale ones count, so the map is not empty. Nodes beyond the top rung are left out of the fit and
// counted in the header instead, so one MQTT-bridged node cannot collapse the scale (Map 06).
void InkHUD::T5MapApplet::applyFit(const Layout &l)
{
    if (!fit)
        return;

    bool anyFresh = false;
    for (const Plot &p : plots)
        if (selectable(p) && !p.stale && !p.beyond)
            anyFresh = true;

    float worst = 0; // Metres from the centre that must be inside the margin
    bool any = false;
    for (const Plot &p : plots) {
        if (!selectable(p) || p.beyond || (anyFresh && p.stale))
            continue;
        const float d = centred ? GeoCoord::latLongToMeter(p.lat, p.lon, centreLat, centreLon) : 0;
        worst = std::max(worst, d);
        any = true;
    }

    if (!any) {
        rung = RUNG_DEFAULT; // Nothing to fit: the 1 km step, and zoom stays live (E3)
        return;
    }

    const float room = std::min(l.mapW, l.mapH) / 2.0f - FIT_MARGIN;
    for (uint8_t r = 0; r < RUNGS; r++) {
        if (worst / (rungMetres(r) / l.pxPerStep) <= room) {
            rung = r;
            return;
        }
    }
    rung = RUNGS - 1;
}

// North-up, metres from the centre, through the same geographic helpers Node detail's relative plot uses
// North-up, metres from the centre, through the same geographic helpers Node detail's relative plot uses.
// The one place a coordinate becomes a pixel, so the markers, US, the rings and the edge tags cannot
// disagree about where something is.
void InkHUD::T5MapApplet::toPixels(const Layout &l, double lat, double lon, int16_t &x, int16_t &y) const
{
    const float metresPerPx = rungMetres(rung) / l.pxPerStep;
    const float d = GeoCoord::latLongToMeter(lat, lon, centreLat, centreLon);
    const float b = GeoCoord::bearing(centreLat, centreLon, lat, lon);
    x = l.cx + lround(d * sinf(b) / metresPerPx);
    y = l.cy - lround(d * cosf(b) / metresPerPx);
}

// Whether a point that size sits wholly inside the map band
bool InkHUD::T5MapApplet::inBand(const Layout &l, int16_t x, int16_t y, int16_t half) const
{
    return x >= half && x < l.mapW - half && y >= l.mapTop + half && y < l.mapTop + l.mapH - half;
}

void InkHUD::T5MapApplet::project(const Layout &l)
{
    for (Plot &p : plots) {
        p.group = -1;
        p.onScreen = false;
        if (!centred)
            continue;
        toPixels(l, p.lat, p.lon, p.x, p.y);
        p.onScreen = inBand(l, p.x, p.y, 7); // Half of the 14 px square
    }
}

// Markers whose centres fall within 24 px merge into one box, recomputed on every scale or anchor change.
// The selected node is never absorbed: it keeps its frame and the rest cluster around it (Map 03).
void InkHUD::T5MapApplet::buildClusters(const Layout &l)
{
    (void)l;
    // Taken in the order snapshot's order, so which marker leads a cluster does not depend on nodeDB's
    // ordering, and a cluster does not re-form around a different member between frames
    std::vector<Plot *> candidates;
    for (Plot &p : plots)
        if (drawn(p) && p.onScreen && p.num != selected) // The selection is never absorbed (Map 03)
            candidates.push_back(&p);
    std::sort(candidates.begin(), candidates.end(), [this](const Plot *a, const Plot *b) {
        const auto ia = std::find(order.begin(), order.end(), a->num);
        const auto ib = std::find(order.begin(), order.end(), b->num);
        if (ia != ib)
            return ia < ib; // Anything outside the order sorts after it, by end() position
        return a->num < b->num;
    });

    std::vector<Plot *> leads;
    for (Plot *p : candidates) {
        for (uint8_t g = 0; g < leads.size(); g++) {
            const int32_t dx = p->x - leads[g]->x, dy = p->y - leads[g]->y;
            if (dx * dx + dy * dy <= CLUSTER_PX * CLUSTER_PX) {
                p->group = g;
                break;
            }
        }
        if (p->group < 0) {
            p->group = leads.size();
            leads.push_back(p);
        }
    }

    // A group of one is not a cluster
    std::vector<uint16_t> members(leads.size(), 0);
    for (const Plot &p : plots)
        if (p.group >= 0)
            members[p.group]++;
    for (Plot &p : plots)
        if (p.group >= 0 && members[p.group] < 2)
            p.group = -1;
}

void InkHUD::T5MapApplet::reconcile(const Layout &l)
{
    readOwn();
    readPlots();
    settleSelection();
    settleOrder();
    settleCentre(l);
    applyFit(l);
    project(l);
    buildClusters(l);

    // The chooser cannot outlive the cluster it was opened on
    if (choosing) {
        std::vector<NodeNum> live;
        for (NodeNum num : chooser) {
            const Plot *p = find(num);
            if (p && drawn(*p))
                live.push_back(num);
        }
        chooser = std::move(live);
        if (chooser.size() < 2)
            choosing = false;
    }
}

// ---- actions -----------------------------------------------------------------------------------------

// Selecting never moves the map. Only CENTER, FIT, zoom and deep links move it (Map 05).
void InkHUD::T5MapApplet::select(NodeNum num)
{
    if (selected == num)
        return;
    selected = num;
    if (num != deepTarget)
        deepTarget = 0; // The >24 h exception belongs to the deep-link target alone
    requestUpdate();
}

void InkHUD::T5MapApplet::step(int8_t delta)
{
    if (order.empty())
        return;
    const int16_t at = selectionIndex();
    // A target outside the order - a deep-linked old node, or one with no position - hands PREV / NEXT
    // back to the ordinary order rather than pulling it along (Map 05, E4, E10)
    const int16_t next =
        at < 0 ? (delta > 0 ? 0 : (int16_t)order.size() - 1) : (at + delta + (int16_t)order.size()) % (int16_t)order.size();
    select(order[next]);
}

void InkHUD::T5MapApplet::zoom(int8_t delta)
{
    const int8_t next = (int8_t)rung + delta;
    if (next < 0 || next >= RUNGS)
        return;
    rung = next;
    fit = false; // One ladder step per tap, leaving FIT (Map 04)
    requestUpdate();
}

void InkHUD::T5MapApplet::toggleAnchor()
{
    if (anchor == NODE) {
        anchor = ME;
        anchorNode = 0;
    } else {
        anchorNode = selected;
        anchor = NODE;
    }
    fit = false;
    centred = false; // settleCentre moves the view to the new anchor
    requestUpdate();
}

void InkHUD::T5MapApplet::openDetail()
{
    if (!selected)
        return;
    backNode = 0; // The map's own back chip is dropped at this point: one return slot, no stack (Map 07)
    T5NodeDetailApplet::open(selected, MAP);
}

// SHOW ON MAP. Accepts a position older than 24 h, and a node with none at all (Map 07).
bool InkHUD::T5MapApplet::focusNode(NodeNum num)
{
    InkHUD *hud = InkHUD::getInstance();
    const int8_t i = indexOf("Map");
    if (i < 0 || !nodeDB->getMeshNode(num))
        return false;
    T5MapApplet *map = static_cast<T5MapApplet *>(hud->userApplets[i]);

    map->resetView();
    map->selected = num;
    map->deepTarget = num;
    map->backNode = num;
    map->order.clear();
    map->choosing = false;

    // Read the model now, so the step and anchor are chosen before the first frame rather than corrected
    // in a second one (Map 08)
    const Layout l = map->layout();
    map->readOwn();
    map->readPlots();
    const Plot *target = map->find(num);

    if (target) {
        map->anchor = NODE;
        map->anchorNode = num;
        map->centreLat = target->lat;
        map->centreLon = target->lon;
        map->centred = true;
        map->fit = false;
        // The smallest step which also contains us, so the relationship is visible at once. With no own
        // position, the 1 km step (Map 07).
        map->rung = RUNG_DEFAULT;
        if (map->own.valid()) {
            const float room = std::min(l.mapW, l.mapH) / 2.0f - FIT_MARGIN;
            for (uint8_t r = 0; r < RUNGS; r++) {
                if (target->distance / (map->rungMetres(r) / l.pxPerStep) <= room) {
                    map->rung = r;
                    break;
                }
                map->rung = RUNGS - 1;
            }
        }
    } else {
        // No position to centre on: the default view, and a panel which says why (E4)
        map->fit = true;
        map->anchor = ME;
    }

    map->viewPreset = true;
    if (!hud->showApplet(i, EInk::UpdateTypes::UNSPECIFIED, true)) {
        map->viewPreset = false;
        return false;
    }
    return true;
}

// Node detail's back chip, when Node detail was opened from the map: the prior view, untouched
bool InkHUD::T5MapApplet::openRestoring()
{
    InkHUD *hud = InkHUD::getInstance();
    const int8_t i = indexOf("Map");
    if (i < 0)
        return false;
    T5MapApplet *map = static_cast<T5MapApplet *>(hud->userApplets[i]);
    map->viewPreset = true;
    if (!hud->showApplet(i, EInk::UpdateTypes::UNSPECIFIED, true)) {
        map->viewPreset = false;
        return false;
    }
    return true;
}

// ---- copy --------------------------------------------------------------------------------------------

std::string InkHUD::T5MapApplet::distanceString(float metres) const
{
    char out[16];
    if (imperial()) {
        const float miles = metres / MI;
        if (miles < 0.1f)
            snprintf(out, sizeof(out), "%u ft", (unsigned)lround(metres * 3.28084f));
        else if (miles < 10)
            snprintf(out, sizeof(out), "%.1f mi", miles);
        else
            snprintf(out, sizeof(out), "%u mi", (unsigned)lround(miles));
    } else if (metres < 1000) {
        snprintf(out, sizeof(out), "%u m", (unsigned)lround(metres));
    } else if (metres < 10000) {
        snprintf(out, sizeof(out), "%.1f km", metres / 1000);
    } else {
        snprintf(out, sizeof(out), "%u km", (unsigned)lround(metres / 1000));
    }
    return out;
}

// Minutes to 59, then whole hours, then days: the panel only redraws when the shown value changes (Map 08)
std::string InkHUD::T5MapApplet::ageString(uint32_t secs) const
{
    if (secs == AGE_UNKNOWN)
        return "?";
    if (secs < 60)
        return "now";
    if (secs < 60 * 60)
        return to_string(secs / 60) + " min";
    if (secs < 24 * 60 * 60)
        return to_string(secs / (60 * 60)) + " H";
    const uint32_t days = secs / (24 * 60 * 60);
    return to_string(days) + (days == 1 ? " DAY" : " DAYS");
}

// GPS FIX · n SAT / GPS · NO FIX · n SAT / GPS · SEARCHING · n SAT / GPS OFF / FIXED POS / PHONE POS.
// Never a fix dimension: TINYGPS_OPTION_NO_CUSTOM_FIELDS leaves no reliable 2D / 3D answer (Map 13).
std::string InkHUD::T5MapApplet::gpsLine() const
{
    if (own.manual)
        return "FIXED POS";
    if (own.phone)
        return "PHONE POS";

    const bool off = config.position.gps_mode != meshtastic_Config_PositionConfig_GpsMode_ENABLED;
    if (off)
        return "GPS OFF";

    const std::string sats = to_string(gpsStatus->getNumSatellites()) + " SAT";
    if (gpsStatus->getHasLock())
        return "GPS FIX " + std::string(SEP) + " " + sats;
    // No lock: the wording turns on whether we have ever had a position to fall back on
    return std::string("GPS ") + SEP + (own.valid() ? " NO FIX " : " SEARCHING ") + SEP + " " + sats;
}

// The counts, or the one sentence that explains an abnormal state (Map 01, 09)
std::string InkHUD::T5MapApplet::countsLine() const
{
    uint16_t plotted = 0;
    uint32_t newest = AGE_UNKNOWN;
    bool anyFresh = false;
    for (const Plot &p : plots) {
        if (!selectable(p))
            continue; // An old deep-link target is drawn, but still counted as OLD, not as plotted (E10)
        plotted++;
        if (!p.stale)
            anyFresh = true;
        newest = std::min(newest, p.age);
    }

    if (plotted) {
        // No own position is the structural fact: it explains the centring, so it comes first
        if (!own.valid())
            return std::string("NO OWN POSITION ") + SEP + " CENTRED ON NODES";
        if (own.kind == OWN_LAST_FIX) {
            const std::string clock = t5ListTime(own.fixTime, false);
            std::string line = "OWN POSITION FROM LAST FIX";
            if (!clock.empty())
                line += " " + clock;
            const uint32_t now = getValidTime(RTCQuality::RTCQualityDevice);
            if (now && own.fixTime && own.fixTime <= now)
                line += std::string(" ") + SEP + " " + ageString(now - own.fixTime);
            return line;
        }
        if (!anyFresh && newest != AGE_UNKNOWN)
            return "ALL " + to_string(plotted) + " POSITIONS STALE " + SEP + " NEWEST " + ageString(newest);
    }

    std::string line = to_string(plotted) + " PLOTTED";
    if (staleCount)
        line += std::string(" ") + SEP + " " + to_string(staleCount) + " STALE";
    if (oldCount)
        line += std::string(" ") + SEP + " " + to_string(oldCount) + " OLD";
    if (noPosition)
        line += std::string(" ") + SEP + " " + to_string(noPosition) + " NO POSITION";
    if (beyondCount)
        line += std::string(" ") + SEP + " " + to_string(beyondCount) + " BEYOND " + (imperial() ? "250 mi" : "250 km");
    return line;
}

// ---- rendering ---------------------------------------------------------------------------------------

void InkHUD::T5MapApplet::onRender(bool full)
{
    (void)full;
    const Layout l = layout();

    // Everything the frame shows is settled before a pixel is drawn, from the clock and nodeDB as they
    // stand now. That is what makes the frame after a wake one reconciled frame, rather than the
    // snapshot's state redrawn and corrected in a second one (Map 08, System UI 08).
    reconcile(l);

    drawHeaderBand(l);
    drawMapBand(l);
    drawStrip(l);
    drawPanel(l);
    drawNav();
}

void InkHUD::T5MapApplet::drawHeaderBand(const Layout &l)
{
    const int16_t rule = l.console ? CONSOLE_BAND : 99;

    if (backNode && !l.console) {
        // Carry deep link: one-part "‹ MAP", the GPS state where the status sits, the counts on line 2.
        // Back still returns to the node (Panel Fit D12)
        carryBackRight = drawSubPageTitle("MAP", gpsLine());
        setFont(fontSmall);
        printAt(MARGIN, 80 - 19, t5Elide(*this, countsLine(), width() - 2 * MARGIN)); // Baseline 80
        drawLine(0, rule, width() - 1, rule, BLACK);
        return;
    }

    setFont(fontLarge);
    int16_t lineLeft;
    if (backNode) {
        // Deep link: the chip takes the title position and MAP moves beside it. The nav still inverts
        // MAP - the map is a top-level place, not a child screen (Map 07)
        const std::string crumb = std::string(BACK) + " " + t5NodeTag(backNode);
        printBold(MARGIN, l.console ? 12 : 14, crumb);
        const int16_t mapLeft = std::max<int16_t>(150, MARGIN + getTextWidth(crumb) + 12); // Crumb in its own font
        setFont(fontMedium);
        printAt(mapLeft, l.console ? 16 : 22, "MAP");
        lineLeft = l.console ? std::max<int16_t>(240, mapLeft + getTextWidth("MAP") + 16) : MARGIN;
    } else {
        printBold(MARGIN, l.console ? 12 : 14, "MAP");
        lineLeft = l.console ? 110 : MARGIN;
    }

    setFont(fontSmall);
    const std::string gps = gpsLine();
    const int16_t gpsLeft = width() - MARGIN - getTextWidth(gps);
    printAt(width() - MARGIN, l.console ? 22 : 28, gps, RIGHT);

    // The counts, or the sentence which explains an abnormal state. Cut short of the GPS state rather
    // than running under it (Map 01)
    const int16_t lineTop = l.console ? 22 : 66;
    setCrop(lineLeft, 0, std::max<int16_t>(0, gpsLeft - 12 - lineLeft), rule);
    printAt(lineLeft, lineTop, t5Elide(*this, countsLine(), std::max<int16_t>(0, gpsLeft - 12 - lineLeft)));
    resetCrop();

    drawLine(0, rule, width() - 1, rule, BLACK);
}

// Five cells: ZOOM OUT | SCALE (read-only) | ZOOM IN | FIT | CENTER target (Map 01, 04)
void InkHUD::T5MapApplet::drawStrip(const Layout &l)
{
    drawLine(0, l.stripTop, l.stripW - 1, l.stripTop, BLACK);
    if (!l.console)
        drawLine(0, l.stripTop + l.stripH - 1, l.stripW - 1, l.stripTop + l.stripH - 1, BLACK);

    const Plot *sel = find(selected);
    const bool anything = centred && (own.valid() || !plots.empty());

    // CENTER states what it would move to: the selection's short name while anchored on us, ME while
    // anchored on a node. Struck when there is nothing to toggle to (Map 04)
    std::string target = DASH;
    bool canCenter = false;
    if (choosing) {
        target = DASH;
    } else if (!anything) {
        target = DASH;
    } else if (!own.valid()) {
        target = "ME"; // Nowhere to go back to: the view is centred on the nodes (E1)
    } else if (anchor == NODE) {
        target = "ME";
        canCenter = true;
    } else if (selected) {
        target = t5NodeTag(selected);
        canCenter = sel && drawn(*sel); // A selection with no position cannot be centred on (E4)
    } else {
        target = "ME"; // Nothing selected to move to (E3)
    }

    struct Cell {
        const char *cap;
        std::string value;
        bool inverted, struck;
    };
    const Cell cells[] = {
        {"ZOOM", "OUT", false, !anything || rung == 0},
        {"SCALE", anything ? rungLabel(rung, false) : DASH, false, false},
        {"ZOOM", "IN", false, !anything || rung + 1 >= RUNGS},
        {"VIEW", "FIT", fit && anything, !anything},
        {"CENTER", target, false, !canCenter},
    };

    for (uint8_t i = 0; i < 5; i++) {
        const int16_t left = i * (l.stripW + 1) / 5;
        const int16_t right = std::min<int16_t>((i + 1) * (l.stripW + 1) / 5 - 1, l.stripW);
        if (i > 0)
            fillRect(left - 1, l.stripTop + 1, 1, l.stripH - 2, BLACK);
        // SCALE is read-only: regular weight, and never a target (Map 04)
        drawCell(left, l.stripTop + 1, right - left, l.stripH - 2, cells[i].cap, cells[i].value, cells[i].inverted,
                 cells[i].struck, i != 1);
    }
}

// A control cell: its caption over its value, both centred
void InkHUD::T5MapApplet::drawCell(int16_t left, int16_t top, uint16_t w, uint16_t h, const char *cap, const std::string &value,
                                   bool inverted, bool struck, bool bold)
{
    const int16_t centerX = left + w / 2;
    if (inverted) {
        fillRect(left, top, w, h, BLACK);
        setTextColor(WHITE);
    }
    const int16_t capH = fontSmall.lineHeight(), valueH = fontMedium.lineHeight();
    const int16_t blockTop = top + (h - capH - 2 - valueH) / 2;

    setFont(fontSmall);
    printAt(centerX, blockTop, cap, CENTER);
    setFont(fontMedium);
    int16_t valueTop = blockTop + capH + 2;
    if (getTextWidth(value) + 1 > (int16_t)w - 8) {
        // A short name too wide for the cell steps down to small on the same baseline, never elided (Panel Fit D7)
        setFont(fontSmall);
        valueTop += fontMedium.heightAboveCursor() - fontSmall.heightAboveCursor();
    }
    if (bold)
        printBold(centerX, valueTop, value, CENTER);
    else
        printAt(centerX, valueTop, value, CENTER);

    if (inverted)
        setTextColor(BLACK);
    if (struck) {
        const uint16_t tw = getTextWidth(value);
        strikeThrough(centerX - tw / 2 - 14, valueTop + getFont().lineHeight() / 2, tw + 28); // The face actually drawn
    }
}

// ---- the map band ------------------------------------------------------------------------------------

void InkHUD::T5MapApplet::drawArrow(int16_t cx, int16_t cy, int16_t degrees, Color color)
{
    // No Win-1253 glyph exists for any of the eight arrows, and loading a font for one shape would be
    // worse than drawing it - as the mode control's arrows already are
    const float a = ((degrees % 360) + 360) % 360 * (float)M_PI / 180.0f;
    const float sa = sinf(a), ca = cosf(a);
    const int16_t tipX = cx + lround(7 * sa), tipY = cy - lround(7 * ca);
    const int16_t backX = cx - lround(4 * sa), backY = cy + lround(4 * ca);
    fillTriangle(tipX, tipY, backX - lround(5 * ca), backY - lround(5 * sa), backX + lround(5 * ca), backY + lround(5 * sa),
                 color);
}

// US: a 26 px ring with a centre dot, always drawn last and never labelled. Without lock the ring turns
// dashed and gains its LAST FIX label; losing lock never moves the map (Map 03, E2).
void InkHUD::T5MapApplet::drawUs(const Layout &l, int16_t x, int16_t y)
{
    (void)l;
    constexpr int16_t R = 13, STROKE = 3;
    // The 3 px white halo, so the ring survives crossing a range ring or a marker
    fillCircle(x, y, R + STROKE, WHITE);

    if (own.kind == OWN_LAST_FIX) {
        // Dashed: eight arcs with eight gaps, struck radially so the stroke stays 3 px
        for (uint16_t deg = 0; deg < 360; deg++) {
            if ((deg / 12) % 2)
                continue;
            const float a = deg * (float)M_PI / 180.0f;
            for (int16_t t = 0; t < STROKE; t++)
                drawPixel(x + lround((R - t) * sinf(a)), y - lround((R - t) * cosf(a)), BLACK);
        }
    } else {
        for (int16_t t = 0; t < STROKE; t++)
            drawCircle(x, y, R - t, BLACK);
        fillCircle(x, y, 3, BLACK); // The 6 px centre dot
    }
}

// Two 1 px dotted rings around US, at one and two steps. They stand in for the missing basemap, and they
// are part of the base render: no separate decoration pass, and no update of their own (Map 03, D7).
void InkHUD::T5MapApplet::drawRings(const Layout &l, int16_t ux, int16_t uy)
{
    for (uint8_t ring = 1; ring <= 2; ring++) {
        const int16_t r = ring * l.pxPerStep;
        // One pixel every fourth of the circumference-step, so the dots stay evenly spaced at any radius
        const float stepRad = 4.0f / std::max<int16_t>(r, 1);
        for (float a = 0; a < 2 * (float)M_PI; a += stepRad) {
            const int16_t x = ux + lround(r * sinf(a)), y = uy - lround(r * cosf(a));
            if (x >= 0 && x < l.mapW && y >= l.mapTop && y < l.mapTop + l.mapH)
                drawPixel(x, y, BLACK);
        }
    }
}

// Ring labels are placed last: markers, their labels, badges, the focus box and US are reserved first,
// and never move for a label. Each label tries the top of its ring, then the bottom, right and left, in a
// text + 8 by 26 box with its baseline 9 px below the point; with all four covered it is skipped, as the
// SCALE cell states the step anyway (Panel Fit D11).
void InkHUD::T5MapApplet::labelRings(const Layout &l, int16_t ux, int16_t uy, std::vector<Rect> &taken)
{
    taken.push_back(Rect{(int16_t)(ux - 16), (int16_t)(uy - 16), 32, 32}); // US: the 26 px ring and its halo
    setFont(fontSmall);
    for (uint8_t ring = 1; ring <= 2; ring++) {
        const int16_t r = ring * l.pxPerStep;
        const std::string label = rungLabel(rung, ring == 2);
        const uint16_t tw = getTextWidth(label) + 8;
        const int16_t at[4][2] = {
            {ux, (int16_t)(uy - r)}, {ux, (int16_t)(uy + r)}, {(int16_t)(ux + r), uy}, {(int16_t)(ux - r), uy}};
        for (uint8_t side = 0; side < 4; side++) {
            const Rect box{(int16_t)(at[side][0] - tw / 2), (int16_t)(at[side][1] - 13), tw, 26};
            if (box.l < 0 || box.l + (int16_t)box.w > l.mapW || box.t < l.mapTop || box.t + (int16_t)box.h > l.mapTop + l.mapH)
                continue;
            if (!place(taken, box))
                continue;
            fillRect(box.l, box.t, box.w, box.h, WHITE);
            printAt(box.l + 4, at[side][1] + 9 - 19, label); // Small caps end 19 px below the print top
            break;
        }
    }
}

void InkHUD::T5MapApplet::drawScaleBar(const Layout &l)
{
    // Replaces the rings whenever US is not on the map: the user still has a distance reference (E1, E8)
    const int16_t barW = l.pxPerStep, h = 8;
    const int16_t left = 14, bottom = l.mapTop + l.mapH - 12;
    fillRect(left, bottom - h, 2, h, BLACK);
    fillRect(left + barW - 2, bottom - h, 2, h, BLACK);
    fillRect(left, bottom - 2, barW, 2, BLACK);
    setFont(fontSmall);
    printAt(left + barW + 6, bottom, rungLabel(rung, false), LEFT, BOTTOM);
}

// An inverted tag clamped to the map edge on the bearing line, with an arrow and a distance. Only the
// selected node and US ever get one: a clipped marker would lose awareness of them (Map 03, 05).
void InkHUD::T5MapApplet::drawEdgeTag(const Layout &l, const std::string &name, const std::string &away, int16_t degrees,
                                      Rect &box)
{
    setFont(fontSmall);
    const int16_t lh = fontSmall.lineHeight();
    const int16_t arrowW = 18;
    const int16_t boxW = getTextWidth(name) + arrowW + getTextWidth(away) + 2 * 8, boxH = lh + 2 * 4;

    // Walk out from the centre along the bearing until the box would leave the band, then step back
    const float a = degrees * (float)M_PI / 180.0f;
    const float sa = sinf(a), ca = cosf(a);
    float t = 0;
    const float limit = std::max(l.mapW, l.mapH);
    while (t < limit) {
        const int16_t px = l.cx + lround(t * sa), py = l.cy - lround(t * ca);
        if (px - boxW / 2 < 4 || px + boxW / 2 > l.mapW - 4 || py - boxH / 2 < l.mapTop + 4 ||
            py + boxH / 2 > l.mapTop + l.mapH - 4)
            break;
        t += 2;
    }
    const int16_t cx = l.cx + lround(t * sa), cy = l.cy - lround(t * ca);
    const int16_t left = std::min<int16_t>(std::max<int16_t>(4, cx - boxW / 2), l.mapW - 4 - boxW);
    const int16_t top = std::min<int16_t>(std::max<int16_t>(l.mapTop + 4, cy - boxH / 2), l.mapTop + l.mapH - 4 - boxH);

    box = Rect{left, top, (uint16_t)boxW, (uint16_t)boxH};
    fillRect(left - 2, top - 2, boxW + 4, boxH + 4, WHITE);
    fillRect(left, top, boxW, boxH, BLACK);
    setTextColor(WHITE);
    printBold(left + 8, top + 4, name);
    drawArrow(left + 8 + getTextWidth(name) + arrowW / 2, top + boxH / 2, degrees, WHITE);
    printBold(left + 8 + getTextWidth(name) + arrowW, top + 4, away);
    setTextColor(BLACK);
}

bool InkHUD::T5MapApplet::place(std::vector<Rect> &taken, const Rect &r) const
{
    for (const Rect &o : taken)
        if (r.hits(o))
            return false;
    taken.push_back(r);
    return true;
}

// Shape carries identity, fill carries freshness, a frame carries selection. Every marker gets a white
// halo, so it survives crossing a ring or another marker (Map 03).
void InkHUD::T5MapApplet::drawMarkers(const Layout &l, std::vector<Rect> &taken)
{
    // Cluster boxes first: one box stands in for its members, so those markers are never drawn
    std::vector<int8_t> drawnGroups;
    for (const Plot &p : plots) {
        if (p.group < 0 || !p.onScreen)
            continue;
        if (std::find(drawnGroups.begin(), drawnGroups.end(), p.group) != drawnGroups.end())
            continue;
        drawnGroups.push_back(p.group);

        uint16_t members = 0;
        for (const Plot &q : plots)
            if (q.group == p.group)
                members++;

        constexpr int16_t H = 14; // Half of the 28 px box
        fillRect(p.x - H - 2, p.y - H - 2, 2 * H + 4, 2 * H + 4, WHITE);
        fillRect(p.x - H, p.y - H, 2 * H, 2 * H, WHITE);
        drawRect(p.x - H, p.y - H, 2 * H, 2 * H, BLACK);
        drawRect(p.x - H + 1, p.y - H + 1, 2 * H - 2, 2 * H - 2, BLACK);
        setFont(fontSmall);
        printBold(p.x, p.y - fontSmall.lineHeight() / 2, to_string(members), CENTER);
        taken.push_back(Rect{(int16_t)(p.x - H - 2), (int16_t)(p.y - H - 2), 2 * H + 4, 2 * H + 4});
    }

    // Plain markers, selection last so its frame sits over its neighbours
    auto square = [this](const Plot &p) {
        constexpr int16_t H = 7; // Half of the 14 px square
        fillRect(p.x - H - 2, p.y - H - 2, 2 * H + 4, 2 * H + 4, WHITE);
        if (p.stale || p.old) {
            // Hollow: the position is 2 h old or more. Never a blink, never a special glyph
            fillRect(p.x - H, p.y - H, 2 * H, 2 * H, WHITE);
            drawRect(p.x - H, p.y - H, 2 * H, 2 * H, BLACK);
            drawRect(p.x - H + 1, p.y - H + 1, 2 * H - 2, 2 * H - 2, BLACK);
        } else {
            fillRect(p.x - H, p.y - H, 2 * H, 2 * H, BLACK);
        }
    };

    for (const Plot &p : plots) {
        if (!drawn(p) || !p.onScreen || p.group >= 0 || p.num == selected)
            continue;
        square(p);
        taken.push_back(Rect{(int16_t)(p.x - 9), (int16_t)(p.y - 9), 18, 18});
    }

    const Plot *sel = find(selected);
    if (sel && drawn(*sel) && sel->onScreen) {
        constexpr int16_t F = 18; // Half of the 36 px frame
        fillRect(sel->x - F - 2, sel->y - F - 2, 2 * F + 4, 2 * F + 4, WHITE);
        drawRect(sel->x - F, sel->y - F, 2 * F, 2 * F, BLACK);
        drawRect(sel->x - F + 1, sel->y - F + 1, 2 * F - 2, 2 * F - 2, BLACK);
        square(*sel); // Stale selected is hollow inside the frame
        taken.push_back(Rect{(int16_t)(sel->x - F - 2), (int16_t)(sel->y - F - 2), 2 * F + 4, 2 * F + 4});
    }

    // Labels, by priority: selected, then favourites, then nearest. Above twelve markers in view only the
    // selection and favourites are labelled. A label that would collide is dropped, never the marker.
    uint16_t inView = 0;
    for (const Plot &p : plots)
        if (drawn(p) && p.onScreen && p.group < 0)
            inView++;
    const bool allLabels = inView <= 12;

    std::vector<const Plot *> labelled;
    for (const Plot &p : plots)
        if (drawn(p) && p.onScreen && p.group < 0 && (allLabels || p.favorite || p.num == selected))
            labelled.push_back(&p);
    std::sort(labelled.begin(), labelled.end(), [this](const Plot *a, const Plot *b) {
        if ((a->num == selected) != (b->num == selected))
            return a->num == selected;
        if (a->favorite != b->favorite)
            return a->favorite;
        if (a->distance >= 0 && b->distance >= 0 && a->distance != b->distance)
            return a->distance < b->distance;
        return a->num < b->num;
    });

    setFont(fontSmall);
    const int16_t lh = fontSmall.lineHeight();
    for (const Plot *p : labelled) {
        // Short name on the side facing the map centre, as the shared base applet does. A stale marker
        // carries its age with it (Map 03).
        std::string text = t5NodeTag(p->num);
        if (p->stale || p->old)
            text += std::string(" ") + SEP + " " + ageString(p->age);

        const bool isSel = p->num == selected;
        const bool right = p->x < l.mapW / 2;
        const int16_t gap = isSel ? 22 : 12;
        const uint16_t pad = isSel ? 6 : 4;
        const uint16_t tw = getTextWidth(text) + 2 * pad;
        const int16_t left = right ? p->x + gap : p->x - gap - tw;
        const int16_t top = p->y - lh / 2;
        const Rect box{left, top, tw, (uint16_t)lh};
        if (box.l < 0 || box.l + (int16_t)box.w > l.mapW)
            continue;
        if (!place(taken, box))
            continue;

        if (isSel) {
            fillRect(box.l - 2, box.t - 2, box.w + 4, box.h + 4, WHITE);
            fillRect(box.l, box.t, box.w, box.h, BLACK);
            setTextColor(WHITE);
            printBold(box.l + pad, box.t, text);
            setTextColor(BLACK);
        } else {
            fillRect(box.l, box.t, box.w, box.h, WHITE);
            // Regular weight when the label carries an age, bold when it is just a name (Map 03)
            if (p->stale || p->old)
                printAt(box.l + pad, box.t, text);
            else
                printBold(box.l + pad, box.t, text);
        }
    }
}

void InkHUD::T5MapApplet::drawMapBand(const Layout &l)
{
    selTag = usTag = Rect{}; // Nothing is tappable until this frame draws it
    setCrop(0, l.mapTop, l.mapW, l.mapH);

    // The only full-map message: no own position and nothing plotted, so no control could change what is
    // shown (E7, state 10)
    if (!centred) {
        const int16_t boxLeft = 40, boxW = l.mapW - 2 * 40;
        int16_t top = l.mapTop + (l.console ? 70 : 70);
        setFont(fontMedium);
        printAt(boxLeft + boxW / 2, top, "No positions yet", CENTER);
        top += fontMedium.lineHeight() + 10;
        setFont(fontSmall);
        printWrapped(boxLeft, top, boxW,
                     "GPS is off and no node has shared a position. Turn GPS on in MENU, or wait for a "
                     "position broadcast.");
        resetCrop();
        return;
    }

    setFont(fontSmall);
    printBold(l.mapW - 12 - getTextWidth("N"), l.mapTop + 10, "N");
    drawArrow(l.mapW - 12 - getTextWidth("N") - 10, l.mapTop + 10 + fontSmall.lineHeight() / 2, 0, BLACK);

    // Where US falls in the view. The rings follow US, so anchoring on a node moves them with it (M2).
    int16_t ux = 0, uy = 0;
    bool usOnScreen = false;
    if (own.valid()) {
        toPixels(l, own.lat, own.lon, ux, uy);
        usOnScreen = inBand(l, ux, uy, 13); // Half of the 26 px ring
    }

    // The one labelled US state: GPS on, no lock, and a position we once had (E2). Its box is known now,
    // so the ring labels can avoid it
    std::string lastFix;
    Rect lastFixBox{};
    if (own.valid() && usOnScreen && own.kind == OWN_LAST_FIX) {
        const std::string clock = t5ListTime(own.fixTime, false);
        lastFix = clock.empty() ? "LAST FIX" : "LAST FIX " + clock;
        setFont(fontSmall);
        lastFixBox = Rect{(int16_t)(ux + 18), (int16_t)(uy - fontSmall.lineHeight() / 2), (uint16_t)(getTextWidth(lastFix) + 8),
                          (uint16_t)fontSmall.lineHeight()};
        if (lastFixBox.l + (int16_t)lastFixBox.w > l.mapW)
            lastFix.clear();
    }

    std::vector<Rect> taken;
    if (usOnScreen)
        drawRings(l, ux, uy);
    else
        drawScaleBar(l); // The rings went with us, so a scale bar takes over (E1, E8)

    drawMarkers(l, taken);
    if (!lastFix.empty())
        taken.push_back(lastFixBox);
    if (usOnScreen)
        labelRings(l, ux, uy, taken);

    // Off-screen awareness, for the selection and for us alone
    const Plot *sel = find(selected);
    if (sel && drawn(*sel) && !sel->onScreen) {
        const float b = GeoCoord::bearing(centreLat, centreLon, sel->lat, sel->lon);
        const int16_t deg = (lround(GeoCoord::toDegrees(b)) + 360) % 360;
        const float away = GeoCoord::latLongToMeter(sel->lat, sel->lon, centreLat, centreLon);
        drawEdgeTag(l, t5NodeTag(sel->num), distanceString(away), deg, selTag);
    }
    if (own.valid() && !usOnScreen) {
        const float b = GeoCoord::bearing(centreLat, centreLon, own.lat, own.lon);
        const int16_t deg = (lround(GeoCoord::toDegrees(b)) + 360) % 360;
        const float away = GeoCoord::latLongToMeter(own.lat, own.lon, centreLat, centreLon);
        drawEdgeTag(l, "YOU", distanceString(away), deg, usTag);
    }

    // US last, as the shared base does
    if (own.valid() && usOnScreen) {
        drawUs(l, ux, uy);
        if (!lastFix.empty()) {
            setFont(fontSmall);
            fillRect(lastFixBox.l, lastFixBox.t, lastFixBox.w, lastFixBox.h, WHITE);
            printAt(lastFixBox.l + 4, lastFixBox.t, lastFix);
        }
    }

    resetCrop();
}

// ---- the panel ---------------------------------------------------------------------------------------

// Exactly one node, and everything numeric about it. Nothing numeric per-node goes on the map, so a
// position packet never forces the map band to redraw unless a marker actually moved (Map 01).
void InkHUD::T5MapApplet::drawPanel(const Layout &l)
{
    if (l.console)
        fillRect(610, l.panelTop, 2, 355, BLACK); // The column rule, down to the nav

    const Plot *sel = find(selected);
    const bool nopos = selected && !sel;

    if (choosing) {
        drawChooser(l);
        return;
    }

    if (!selected) {
        // No selection: the panel explains the absence and gives a count. No action row (E3, E7).
        const bool anyPlotted = !plots.empty();
        const std::string heading = anyPlotted ? "Nothing to select" : "No other node has shared a position";
        std::string body = to_string(noPosition) + (noPosition == 1 ? " node heard" : " nodes heard");
        body += anyPlotted ? ". None has a position."
                           : ", none with a position yet. Each one appears here when its next position "
                             "broadcast arrives.";

        const int16_t left = l.console ? 628 : MARGIN;
        const int16_t top = l.console ? 84 : 670;
        const uint16_t w = l.console ? 284 : l.panelW - 2 * MARGIN;
        setFont(fontMedium);
        printAt(left, top, heading);
        setFont(fontSmall);
        printWrapped(left, top + fontMedium.lineHeight() + 8, w, body);
        return;
    }

    // Name row: short name, long name clipped to one line, then FAV and the order position
    const std::string tag = t5NodeTag(selected);
    meshtastic_NodeInfoLite *info = nodeDB->getMeshNode(selected);
    const bool fav = info && nodeInfoLiteIsFavorite(info);

    setFont(fontSmall);
    std::string right = orderLabel();
    if (fav)
        right = "FAV  " + right; // No Win-1253 star glyph exists, so the word carries it
    printAt(width() - MARGIN, l.console ? 82 : 676, right, RIGHT);

    setFont(fontLarge);
    printBold(l.console ? 628 : MARGIN, l.console ? 74 : 666, tag);
    // The long name starts after the tag as drawn (Carry), or below it (Console), never inside it
    const int16_t tagRight = MARGIN + getTextWidth(tag) + 2 + 12;
    const int16_t nameTop = l.console ? 74 + fontLarge.lineHeight() : 673;

    setFont(fontSmall);
    const int16_t rightLeft = width() - MARGIN - getTextWidth(right) - 12;
    setFont(l.console ? fontSmall : fontMedium);
    const int16_t nameLeft = l.console ? 628 : std::max<int16_t>(118, tagRight);
    const int16_t nameW = l.console ? 284 : rightLeft - nameLeft;
    printAt(nameLeft, nameTop, t5Elide(*this, t5NodeTitle(selected), std::max<int16_t>(0, nameW)));

    if (nopos) {
        // SHOW ON MAP still opens the map; it does not pretend. The panel names the node and says why it
        // isn't there (E4).
        const int16_t left = l.console ? 628 : MARGIN;
        const int16_t top = l.console ? 154 : 712;
        if (!l.console)
            dottedRule(0, top, l.panelW);
        setFont(fontMedium);
        printBold(left, top + 8, "NO POSITION REPORTED");
        std::string why;
        if (info) {
            const std::string heard = info->last_heard ? t5ListTime(info->last_heard, false) : "";
            if (!heard.empty())
                why = "Heard " + heard + " " + SEP + " ";
            if (info->has_hops_away)
                why += hopsString(info->hops_away) + " " + SEP + " ";
        }
        why += "no position packet received.";
        setFont(fontSmall);
        printAt(left, top + 8 + fontMedium.lineHeight() + 3, why);
        drawActions(l);
        return;
    }

    // Four facts. DIST · THEN says the distance was true when the position was, and POS AGE is the one
    // fact that changes meaning when it goes stale, so it is the one that inverts (Map 01, E5).
    const bool stale = sel->stale || sel->old;
    std::string dist = DASH, bearing = DASH;
    if (sel->distance >= 0) {
        // The small bold value box is 105 px (Carry) or 136 px (Console). What does not fit gives way whole
        // before anything elides: the precision, then the degrees; the compass word stays (Panel Fit D4)
        setFont(fontSmall);
        const int16_t valueW = (l.console ? 158 : 127) - 22 - 1;
        // Against a last fix rather than a live one, every distance is an approximation and says so (E2)
        const std::string approx = own.approx() ? "~" : "";
        dist = approx + distanceString(sel->distance);
        const float r = precisionRadius(sel->precision);
        const std::string withRadius = dist + " \xB1 " + distanceString(r); // Win-1253 plus-minus: the sender's precision
        if (r > 0)
            dist = getTextWidth(withRadius) + 1 <= valueW ? withRadius : "~" + distanceString(sel->distance);
        char deg[16];
        snprintf(deg, sizeof(deg), "%03d\xB0 ", sel->degrees); // Win-1253 degree sign
        bearing = approx + std::string(deg) + GeoCoord::degreesToBearing(sel->degrees);
        if (getTextWidth(bearing) + 1 > valueW)
            bearing = approx + GeoCoord::degreesToBearing(sel->degrees);
    }

    struct Fact {
        std::string key, value;
        bool inverted;
    };
    const Fact facts[] = {
        {stale ? std::string("DIST ") + SEP + " THEN" : "DIST", dist, false},
        {"BEARING", bearing, false},
        {"POS AGE", (sel->approxAge ? "~" : "") + ageString(sel->age), stale},
        {"HOPS",
         sel->hops < 0    ? "?"
         : sel->hops == 0 ? "DIRECT"
                          : to_string(sel->hops),
         false},
    };

    const uint8_t cols = l.console ? 2 : 4;
    const int16_t gridTop = l.console ? 156 : 712;
    const int16_t cellW = l.console ? 158 : 127, cellH = l.console ? 100 : 63;
    const int16_t gridLeft = l.console ? 612 : 0;
    // Panel Fit D4: label small regular, value small bold. Carry baselines 738 / 768 in the 712-776 row;
    // Console baselines top+36 / top+74. Small caps are 18 px, so tops sit 18 px above those baselines
    const int16_t labelTop = l.console ? 18 : 8, valueTop = l.console ? 56 : 38;

    if (!l.console)
        dottedRule(0, gridTop, l.panelW);
    for (uint8_t i = 0; i < 4; i++) {
        const int16_t left = gridLeft + (i % cols) * cellW, top = gridTop + (i / cols) * cellH;
        if (l.console)
            dottedRule(left, top, cellW);
        setFont(fontSmall);
        printAt(left + 16, top + labelTop, facts[i].key);
        if (facts[i].inverted) {
            const std::string value = t5Elide(*this, facts[i].value, cellW - 22 - 12 - 1);
            const uint16_t tw = getTextWidth(value) + 12;
            fillRect(left + 16, top + valueTop, tw, fontSmall.lineHeight(), BLACK);
            setTextColor(WHITE);
            printBold(left + 16 + 6, top + valueTop, value);
            setTextColor(BLACK);
        } else {
            // Only a distance past five digits can still need this (D4)
            printBold(left + 16, top + valueTop, t5Elide(*this, facts[i].value, cellW - 22 - 1)); // The 105 px value box (D4)
        }
    }

    drawActions(l);
}

// PREV | DETAIL | NEXT, struck together when there is no order to move through
void InkHUD::T5MapApplet::drawActions(const Layout &l)
{
    drawLine(l.actionLeft, l.actionTop, l.actionLeft + l.actionW - 1, l.actionTop, BLACK);
    const bool live = !order.empty();
    const std::string labels[] = {std::string(BACK) + " PREV", "DETAIL", std::string("NEXT ") + FWD};
    const int16_t h = l.console ? 64 : 64;

    setFont(l.console ? fontSmall : fontMedium);
    for (uint8_t i = 0; i < 3; i++) {
        const int16_t left = l.actionLeft + i * (l.actionW + 1) / 3;
        const int16_t right = std::min<int16_t>(l.actionLeft + (i + 1) * (l.actionW + 1) / 3 - 1, l.actionLeft + l.actionW);
        if (i > 0)
            fillRect(left - 1, l.actionTop + 1, 1, h - 2, BLACK);
        const int16_t centerX = left + (right - left) / 2, centerY = l.actionTop + h / 2;
        // DETAIL stays live for any selection: a node with no position still has a Node detail
        const bool cellLive = i == 1 ? selected != 0 : live;
        printThick(centerX, centerY, labels[i], 2, 1);
        if (!cellLive) {
            const uint16_t tw = getTextWidth(labels[i]);
            strikeThrough(centerX - tw / 2 - 14, centerY, tw + 28);
        }
    }
}

// Tapping a cluster turns the panel into a chooser without moving the map. Choosing a member selects it;
// CANCEL restores the previous selection. Above four members the last cell reads "+n · ZOOM IN" (M3, K3).
void InkHUD::T5MapApplet::drawChooser(const Layout &l)
{
    const uint16_t total = chooser.size();
    const uint8_t shown = total > CHOOSER_CELLS ? CHOOSER_CELLS - 1 : (uint8_t)total;
    const bool overflow = total > CHOOSER_CELLS;
    const std::string title = to_string(total) + " NODES AT THIS SPOT";
    const std::string more = "+" + to_string(total - shown);

    if (l.console) {
        setFont(fontMedium);
        printBold(628, 74, title);
        setFont(fontSmall);
        printAt(628, 110, "Tap one to select it.");

        // 67 px rows in the panel column: short name left, distance and age right
        for (uint8_t i = 0; i < shown + (overflow ? 1 : 0); i++) {
            const int16_t top = CHOOSER_TOP_C + i * CHOOSER_ROW_C;
            dottedRule(l.panelLeft, top, l.panelW);
            const int16_t mid = top + (CHOOSER_ROW_C - fontMedium.lineHeight()) / 2;
            setFont(fontMedium);
            if (i == shown) {
                printBold(628, mid, more + " " + SEP + " ZOOM IN");
                break;
            }
            printBold(628, mid, t5NodeTag(chooser[i]));
            setFont(fontSmall);
            printAt(l.panelLeft + l.panelW - MARGIN, top + (CHOOSER_ROW_C - fontSmall.lineHeight()) / 2, chooserFacts(chooser[i]),
                    RIGHT);
        }

        // CANCEL takes the panel column's share of the control band
        drawLine(l.actionLeft, l.actionTop, l.actionLeft + l.actionW - 1, l.actionTop, BLACK);
        setFont(fontSmall);
        printThick(l.actionLeft + l.actionW / 2, l.actionTop + 32, "CANCEL", 2, 1);
        return;
    }

    setFont(fontMedium);
    printBold(MARGIN, 670, title);

    // CANCEL is a boxed chip in the name row, so the member columns keep their full height
    setFont(fontSmall);
    const uint16_t cw = getTextWidth("CANCEL") + 2 * 12 + 4, ch = fontSmall.lineHeight() + 2 * 6 + 4;
    const int16_t cl = l.panelW - MARGIN - cw;
    drawRect(cl, 663, cw, ch, BLACK);
    drawRect(cl + 1, 664, cw - 2, ch - 2, BLACK);
    printBold(cl + cw / 2, 663 + 2 + 6, "CANCEL", CENTER);

    drawLine(0, CHOOSER_TOP, l.panelW - 1, CHOOSER_TOP, BLACK);
    const uint8_t cells = shown + (overflow ? 1 : 0);
    for (uint8_t i = 0; i < cells; i++) {
        const int16_t left = i * (l.panelW + 1) / cells;
        const int16_t right = std::min<int16_t>((i + 1) * (l.panelW + 1) / cells - 1, l.panelW);
        if (i > 0)
            fillRect(left - 1, CHOOSER_TOP + 1, 1, CHOOSER_H - 2, BLACK);
        const int16_t centerX = left + (right - left) / 2;

        // Name over facts, the pair centred in the column
        const int16_t nameH = fontLarge.lineHeight(), factH = fontSmall.lineHeight();
        const int16_t top = CHOOSER_TOP + (CHOOSER_H - nameH - 6 - factH) / 2;
        setFont(fontLarge);
        printBold(centerX, top, i == shown ? more : t5NodeTag(chooser[i]), CENTER);
        setFont(fontSmall);
        printAt(centerX, top + nameH + 6, i == shown ? "ZOOM IN" : chooserFacts(chooser[i]), CENTER);
    }
}

// "2.1 km · 4 min": how far, and how old the position is
std::string InkHUD::T5MapApplet::chooserFacts(NodeNum num) const
{
    const Plot *p = find(num);
    if (!p)
        return "";
    const std::string dist = p->distance >= 0 ? distanceString(p->distance) : std::string(DASH);
    return dist + " " + SEP + " " + ageString(p->age);
}

// A 1 px dotted rule, as the panel's fact separators are drawn
void InkHUD::T5MapApplet::dottedRule(int16_t left, int16_t y, uint16_t w)
{
    for (int16_t x = left; x < left + (int16_t)w; x += 3)
        drawPixel(x, y, BLACK);
}

// ---- touch -------------------------------------------------------------------------------------------

// Taps only: no pan, no long press. One set of verbs in both modes - select, scale, anchor (Map 04).
bool InkHUD::T5MapApplet::onTouchPoint(uint16_t x, uint16_t y, bool longPress)
{
    if (longPress)
        return true; // Reserved, with no hidden function. The stock fallback would open the shared menu

    const Layout l = layout();
    const int16_t tx = x - getTile()->getLeft();
    const int16_t ty = y - getTile()->getTop();

    if (handleNavTap(tx, ty)) {
        // MAP while already here resets to FIT on ME and drops the chip. Every other destination leaves
        // the map, and onForeground resets it on the way back in (Map 04, 07)
        if (tx < navWidth() && (Destination)(tx * 6 / navWidth()) == MAP) {
            resetView();
            requestUpdate();
        }
        return true;
    }

    // The header's back chip: the only way back to the node the deep link came from
    if (backNode && ty < (l.console ? CONSOLE_BAND : 100) && tx < (l.console ? 260 : carryBackRight)) {
        const NodeNum target = backNode;
        backNode = 0;
        T5NodeDetailApplet::open(target, NODES);
        return true;
    }

    // Map band: a marker, its label, a cluster box, or an off-screen tag
    if (ty >= l.mapTop && ty < l.mapTop + l.mapH && tx < l.mapW) {
        handleMapTap(l, tx, ty);
        return true;
    }

    // The control strip
    if (ty >= l.stripTop && ty < l.stripTop + l.stripH && tx < l.stripW) {
        const uint8_t cell = std::min<uint8_t>(4, tx * 5 / l.stripW);
        if (choosing && l.console)
            return true; // The Console chooser puts CANCEL in the panel column, not here
        switch (cell) {
        case 0:
            zoom(-1);
            break;
        case 1:
            break; // SCALE is read-only, and not a target
        case 2:
            zoom(+1);
            break;
        case 3:
            if (!fit) { // Tapping FIT while it is active does nothing
                fit = true;
                anchor = ME;
                anchorNode = 0;
                centred = false;
                order.clear(); // FIT re-snapshots the order (Map 05)
                requestUpdate();
            }
            break;
        case 4:
            toggleAnchor();
            break;
        }
        return true;
    }

    handlePanelTap(l, tx, ty);
    return true; // Consume every tap: the button fallback would cycle applets
}

void InkHUD::T5MapApplet::handleMapTap(const Layout &l, int16_t tx, int16_t ty)
{
    // An off-screen tag acts as CENTER on that target. Tested against the rect the last frame actually
    // drew, so the target and what the user sees cannot drift apart (Map 04).
    const Plot *sel = find(selected);
    if (selTag.w && selTag.hits(Rect{tx, ty, 1, 1}) && sel) {
        anchorNode = selected;
        anchor = NODE;
        fit = false;
        centred = false;
        requestUpdate();
        return;
    }
    if (usTag.w && usTag.hits(Rect{tx, ty, 1, 1})) {
        anchor = ME;
        anchorNode = 0;
        fit = false;
        centred = false;
        requestUpdate();
        return;
    }

    // Nearest marker centre within 32 px wins. A cluster box opens the chooser instead (Map 04).
    const Plot *best = nullptr;
    int32_t bestD2 = 32 * 32;
    for (const Plot &p : plots) {
        if (!drawn(p) || !p.onScreen)
            continue;
        const int32_t dx = tx - p.x, dy = ty - p.y;
        const int32_t d2 = dx * dx + dy * dy;
        if (d2 <= bestD2) {
            bestD2 = d2;
            best = &p;
        }
    }
    if (!best)
        return; // Empty map: nothing at all, so an accidental tap costs nothing

    if (best->group >= 0) {
        chooser.clear();
        for (const Plot &p : plots)
            if (p.group == best->group)
                chooser.push_back(p.num);
        if (chooser.size() >= 2) {
            priorSelection = selected;
            choosing = true;
            requestUpdate();
        }
        return;
    }
    select(best->num);
}

void InkHUD::T5MapApplet::handlePanelTap(const Layout &l, int16_t tx, int16_t ty)
{
    if (choosing) {
        // A member cell selects without moving the map; CANCEL restores the previous selection. The
        // overflow cell is inert: it says what to do, it does not do it.
        const uint16_t total = chooser.size();
        const uint8_t shown = total > CHOOSER_CELLS ? CHOOSER_CELLS - 1 : (uint8_t)total;
        const bool overflow = total > CHOOSER_CELLS;
        const uint8_t cells = shown + (overflow ? 1 : 0);

        if (l.console) {
            if (ty >= l.actionTop && tx >= l.actionLeft) {
                choosing = false;
                select(priorSelection);
                requestUpdate();
                return;
            }
            const int16_t row = (ty - CHOOSER_TOP_C) / CHOOSER_ROW_C;
            if (ty >= CHOOSER_TOP_C && row >= 0 && row < shown && tx >= l.panelLeft) {
                choosing = false;
                select(chooser[row]);
            }
            return;
        }

        // The CANCEL chip sits at the right of the title row. Its half of that row is the target, which
        // is wider than the chip and leaves the title inert
        if (ty >= l.panelTop && ty < CHOOSER_TOP && tx > l.panelW / 2) {
            choosing = false;
            select(priorSelection);
            requestUpdate();
            return;
        }
        if (ty >= CHOOSER_TOP && ty < CHOOSER_TOP + CHOOSER_H && cells) {
            const uint8_t cell = std::min<uint8_t>(cells - 1, tx * cells / l.panelW);
            if (cell < shown) {
                choosing = false;
                select(chooser[cell]);
            }
        }
        return;
    }

    if (ty < l.actionTop || ty >= l.actionTop + 64 || tx < l.actionLeft)
        return;
    switch (std::min<uint8_t>(2, (tx - l.actionLeft) * 3 / l.actionW)) {
    case 0:
        step(-1);
        break;
    case 1:
        openDetail();
        break;
    case 2:
        step(+1);
        break;
    }
}

#endif

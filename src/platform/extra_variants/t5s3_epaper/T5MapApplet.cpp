#if defined(T5_S3_EPAPER_PRO) && defined(MESHTASTIC_INCLUDE_INKHUD)

#include "./T5MapApplet.h"
#include "./T5NodeDetailApplet.h"
#include "./T5Text.h"

#include "GPSStatus.h"
#include "gps/GeoCoord.h"
#include "gps/RTC.h"
#include "mesh/NodeDB.h"
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
    order.clear();   // Re-snapshotted against the model reconcile() is about to read
}

// Position packets only mark the model dirty: every value is re-read from nodeDB at render time, so there
// is nothing here to keep in step. Never requestAutoshow() (Map 08).
ProcessMessage InkHUD::T5MapApplet::handleReceived(const meshtastic_MeshPacket &mp)
{
    (void)mp;
    if (isForeground())
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
            return;
        }
        anchor = ME; // The anchor node left the map: fall back to us
    }

    if (own.valid()) {
        const float moved = centred ? GeoCoord::latLongToMeter(own.lat, own.lon, centreLat, centreLon) : 0;
        const float drift = moved / (rungMetres(rung) / l.pxPerStep);
        const float allow = RECENTRE_FRACTION * std::min(l.mapW, l.mapH);
        if (!centred || (moved > OWN_MOVE_M && drift > allow)) {
            centreLat = own.lat;
            centreLon = own.lon;
            centred = true;
        }
        ownLastLat = own.lat;
        ownLastLon = own.lon;
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
    if (n) {
        centreLat = sumLat / n;
        centreLon = sumLon / n;
        centred = true;
    } else {
        centred = false;
    }
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
void InkHUD::T5MapApplet::project(const Layout &l)
{
    const float metresPerPx = rungMetres(rung) / l.pxPerStep;
    for (Plot &p : plots) {
        p.group = -1;
        p.onScreen = false;
        if (!centred)
            continue;
        const float d = GeoCoord::latLongToMeter(p.lat, p.lon, centreLat, centreLon);
        const float b = GeoCoord::bearing(centreLat, centreLon, p.lat, p.lon);
        p.x = l.cx + lround(d * sin(b) / metresPerPx);
        p.y = l.cy - lround(d * cos(b) / metresPerPx);
        p.onScreen = p.x >= 7 && p.x < l.mapW - 7 && p.y >= l.mapTop + 7 && p.y < l.mapTop + l.mapH - 7;
    }
}

// Markers whose centres fall within 24 px merge into one box, recomputed on every scale or anchor change.
// The selected node is never absorbed: it keeps its frame and the rest cluster around it (Map 03).
void InkHUD::T5MapApplet::buildClusters(const Layout &l)
{
    (void)l;
    // Walk the order snapshot, so which marker leads a cluster does not depend on nodeDB's ordering
    std::vector<NodeNum> walk = order;
    for (const Plot &p : plots)
        if (drawn(p) && std::find(walk.begin(), walk.end(), p.num) == walk.end())
            walk.push_back(p.num);

    std::vector<Plot *> leads;
    for (NodeNum num : walk) {
        Plot *p = nullptr;
        for (Plot &q : plots)
            if (q.num == num)
                p = &q;
        if (!p || !drawn(*p) || !p->onScreen || p->num == selected)
            continue;

        bool joined = false;
        for (uint8_t g = 0; g < leads.size(); g++) {
            const int16_t dx = p->x - leads[g]->x, dy = p->y - leads[g]->y;
            if (dx * dx + dy * dy <= CLUSTER_PX * CLUSTER_PX) {
                p->group = g;
                joined = true;
                break;
            }
        }
        if (!joined) {
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

    setFont(fontLarge);
    int16_t lineLeft;
    if (backNode) {
        // Deep link: the chip takes the title position and MAP moves beside it. The nav still inverts
        // MAP - the map is a top-level place, not a child screen (Map 07)
        printBold(MARGIN, l.console ? 12 : 14, std::string(BACK) + " " + t5NodeTag(backNode));
        setFont(fontMedium);
        printAt(150, l.console ? 16 : 22, "MAP");
        lineLeft = l.console ? 240 : MARGIN;
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
    printAt(lineLeft, lineTop, countsLine());
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
    const int16_t valueTop = blockTop + capH + 2;
    if (bold)
        printBold(centerX, valueTop, value, CENTER);
    else
        printAt(centerX, valueTop, value, CENTER);

    if (inverted)
        setTextColor(BLACK);
    if (struck) {
        const uint16_t tw = getTextWidth(value);
        strikeThrough(centerX - tw / 2 - 14, valueTop + valueH / 2, tw + 28);
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
void InkHUD::T5MapApplet::drawRings(const Layout &l, int16_t ux, int16_t uy, std::vector<Rect> &taken)
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

    // The label sits at the top of each ring, and moves to the next free compass point - right, bottom,
    // left - rather than colliding with a marker or a node label. If none is free it is dropped for that
    // ring: the SCALE cell always states the step anyway (Map 03).
    setFont(fontSmall);
    const int16_t lh = fontSmall.lineHeight();
    for (uint8_t ring = 1; ring <= 2; ring++) {
        const int16_t r = ring * l.pxPerStep;
        const std::string label = rungLabel(rung, ring == 2);
        const uint16_t tw = getTextWidth(label) + 2 * 3;
        // The top of the ring first, then right, bottom and left (Map 03)
        const int16_t at[4][2] = {
            {ux, (int16_t)(uy - r)}, {(int16_t)(ux + r), uy}, {ux, (int16_t)(uy + r)}, {(int16_t)(ux - r), uy}};
        for (uint8_t side = 0; side < 4; side++) {
            const Rect box{(int16_t)(at[side][0] - tw / 2), (int16_t)(at[side][1] - lh / 2), tw, (uint16_t)lh};
            if (box.l < 0 || box.l + (int16_t)box.w > l.mapW || box.t < l.mapTop || box.t + (int16_t)box.h > l.mapTop + l.mapH)
                continue;
            if (!place(taken, box))
                continue;
            fillRect(box.l, box.t, box.w, box.h, WHITE);
            printAt(box.l + 3, box.t, label);
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
void InkHUD::T5MapApplet::drawEdgeTag(const Layout &l, const std::string &name, const std::string &away, int16_t degrees)
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
        const float metresPerPx = rungMetres(rung) / l.pxPerStep;
        const float d = GeoCoord::latLongToMeter(own.lat, own.lon, centreLat, centreLon);
        const float b = GeoCoord::bearing(centreLat, centreLon, own.lat, own.lon);
        ux = l.cx + lround(d * sinf(b) / metresPerPx);
        uy = l.cy - lround(d * cosf(b) / metresPerPx);
        usOnScreen = ux >= 13 && ux < l.mapW - 13 && uy >= l.mapTop + 13 && uy < l.mapTop + l.mapH - 13;
    }

    std::vector<Rect> taken;
    if (usOnScreen)
        drawRings(l, ux, uy, taken);
    else
        drawScaleBar(l); // The rings went with us, so a scale bar takes over (E1, E8)

    drawMarkers(l, taken);

    // Off-screen awareness, for the selection and for us alone
    const Plot *sel = find(selected);
    if (sel && drawn(*sel) && !sel->onScreen) {
        const float b = GeoCoord::bearing(centreLat, centreLon, sel->lat, sel->lon);
        const int16_t deg = (lround(GeoCoord::toDegrees(b)) + 360) % 360;
        const float away = GeoCoord::latLongToMeter(sel->lat, sel->lon, centreLat, centreLon);
        drawEdgeTag(l, t5NodeTag(sel->num), distanceString(away), deg);
    }
    if (own.valid() && !usOnScreen) {
        const float b = GeoCoord::bearing(centreLat, centreLon, own.lat, own.lon);
        const int16_t deg = (lround(GeoCoord::toDegrees(b)) + 360) % 360;
        const float away = GeoCoord::latLongToMeter(own.lat, own.lon, centreLat, centreLon);
        drawEdgeTag(l, "YOU", distanceString(away), deg);
    }

    // US last, as the shared base does
    if (own.valid() && usOnScreen) {
        drawUs(l, ux, uy);
        if (own.kind == OWN_LAST_FIX) {
            // The one labelled US state: GPS on, no lock, and a position we once had (E2)
            const std::string clock = t5ListTime(own.fixTime, false);
            const std::string text = clock.empty() ? "LAST FIX" : "LAST FIX " + clock;
            setFont(fontSmall);
            const uint16_t tw = getTextWidth(text) + 8;
            const int16_t left = ux + 18, top = uy - fontSmall.lineHeight() / 2;
            if (left + (int16_t)tw <= l.mapW) {
                fillRect(left, top, tw, fontSmall.lineHeight(), WHITE);
                printAt(left + 4, top, text);
            }
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

    setFont(l.console ? fontSmall : fontMedium);
    const int16_t nameLeft = l.console ? 628 : 118;
    const uint16_t nameW = l.console ? 284 : 266;
    printAt(nameLeft, l.console ? 114 : 673, t5Elide(*this, t5NodeTitle(selected), nameW));

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
        // Against a last fix rather than a live one, every distance is an approximation and says so (E2)
        dist = (own.approx() ? std::string("~") : std::string("")) + distanceString(sel->distance);
        const float r = precisionRadius(sel->precision);
        if (r > 0)
            dist += " \xB1 " + distanceString(r); // Win-1253 plus-minus: the sender's own precision
        char deg[16];
        snprintf(deg, sizeof(deg), "%03d\xB0 ", sel->degrees); // Win-1253 degree sign
        bearing = (own.approx() ? "~" : "") + std::string(deg) + GeoCoord::degreesToBearing(sel->degrees);
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
    const int16_t gridTop = l.console ? 154 : 712;
    const int16_t cellW = l.console ? 158 : 127, cellH = l.console ? 100 : 63;
    const int16_t gridLeft = l.console ? 612 : 0;
    const int16_t padTop = l.console ? 14 : 8, gap = l.console ? 6 : 3;

    if (!l.console)
        dottedRule(0, gridTop, l.panelW);
    for (uint8_t i = 0; i < 4; i++) {
        const int16_t left = gridLeft + (i % cols) * cellW, top = gridTop + (i / cols) * cellH;
        if (l.console)
            dottedRule(left, top, cellW);
        setFont(fontSmall);
        printAt(left + 16, top + padTop, facts[i].key);
        const int16_t valueTop = top + padTop + fontSmall.lineHeight() + gap;
        setFont(fontMedium);
        if (facts[i].inverted) {
            const uint16_t tw = getTextWidth(facts[i].value) + 12;
            fillRect(left + 16, valueTop, tw, fontMedium.lineHeight(), BLACK);
            setTextColor(WHITE);
            printBold(left + 16 + 6, valueTop, facts[i].value);
            setTextColor(BLACK);
        } else {
            printBold(left + 16, valueTop, facts[i].value);
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
    if (backNode && ty < (l.console ? CONSOLE_BAND : 100) && tx < 260) {
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
    // An off-screen tag acts as CENTER on that target (Map 04). Tested first: it sits over the map edge.
    const Plot *sel = find(selected);
    if (sel && drawn(*sel) && !sel->onScreen && edgeTagHit(l, tx, ty, sel->lat, sel->lon)) {
        anchorNode = selected;
        anchor = NODE;
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

// Whether a tap landed on the tag drawn for a target on the given bearing
bool InkHUD::T5MapApplet::edgeTagHit(const Layout &l, int16_t tx, int16_t ty, double lat, double lon)
{
    if (!centred)
        return false;
    const float b = GeoCoord::bearing(centreLat, centreLon, lat, lon);
    const int16_t deg = (lround(GeoCoord::toDegrees(b)) + 360) % 360;
    const float a = deg * (float)M_PI / 180.0f;
    // The tag sits out near the edge on the bearing line: a generous box around that point
    const int16_t ex = l.cx + lround(std::min(l.mapW, l.mapH) / 2.0f * sinf(a));
    const int16_t ey = l.cy - lround(std::min(l.mapW, l.mapH) / 2.0f * cosf(a));
    return std::abs(tx - ex) <= 80 && std::abs(ty - ey) <= 32;
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

        if (ty >= l.panelTop && ty < CHOOSER_TOP) { // The CANCEL chip's row
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

#ifdef MESHTASTIC_INCLUDE_INKHUD

/*

T5 Map: a relative-position instrument, not a phone map. No basemap ships, so it answers three questions:
where am I, who has told me where they are, and how far and which way is each one (Map 00).

Five bands in both modes - header, map, controls, panel, nav - laid out from the live dimensions: Carry
(508 x 928) and Console (928 x 508). Standalone rather than a MapApplet subclass: everything a subclass
would need for hit-testing is private there, and the shared base has no age, selection or touch (Map 12).

*/

#pragma once

#include "configuration.h"

#include "./T5Applet.h"

#include "SinglePortModule.h"
#include "concurrency/OSThread.h"

namespace NicheGraphics::InkHUD
{

class T5MapApplet : public T5Applet, public SinglePortModule, public concurrency::OSThread
{
  public:
    T5MapApplet();

    static void begin(); // Starts the freshness tick. After InkHUD::begin(), like the other T5 surfaces

    // Node detail's SHOW ON MAP. Selects and centres that node, at the smallest ladder step which also
    // contains us, and arms the back chip. Accepts a position older than 24 h, and a node with none at
    // all (Map 07). False, and nothing shown, when there is no map to show it on.
    static bool focusNode(NodeNum num);

    // Node detail's back chip, when Node detail was opened from here: shows the map with the selection,
    // centre and zoom it had. The one return slot, so no Map <-> Node detail history builds up (Map 07).
    static bool openRestoring();

    void onRender(bool full) override;
    bool onTouchPoint(uint16_t x, uint16_t y, bool longPress) override;
    void onForeground() override;

  protected:
    Destination currentDestination() override { return MAP; }

    // Position packets, our own included: the applet holds no position of its own, so every value is
    // re-read from nodeDB at render time. Never requestAutoshow(): the foreground changes only when the
    // user navigates (Map 08).
    void onActivate() override { loopbackOk = true; }
    void onDeactivate() override { loopbackOk = false; }
    ProcessMessage handleReceived(const meshtastic_MeshPacket &mp) override;

    int32_t runOnce() override; // The one-minute freshness tick (Map 08)

  private:
    // ---- the frozen ladder (Map 06) ----------------------------------------------------------------

    struct Rung {
        float metric, imperial;        // Metres to the first ring, in each unit system
        const char *m1, *m2, *i1, *i2; // Ring 1 and ring 2 labels, metric and imperial
    };
    static const Rung LADDER[];
    static constexpr uint8_t RUNGS = 10;
    static constexpr uint8_t RUNG_DEFAULT = 3; // The 1 km / 1 mi step, where FIT lands with nothing to fit

    float rungMetres(uint8_t rung) const; // Metres per step at this rung, in the configured units
    const char *rungLabel(uint8_t rung, bool second) const;

    // A drawn rectangle, for the label and ring-label collision tests (Map 03)
    struct Rect {
        int16_t l, t;
        uint16_t w, h;
        bool hits(const Rect &o) const
        {
            return l < o.l + (int16_t)o.w && o.l < l + (int16_t)w && t < o.t + (int16_t)o.h && o.t < t + (int16_t)h;
        }
    };

    // ---- geometry ----------------------------------------------------------------------------------

    // Band edges are frozen device pixels, not derived from each other, so a change to one cannot
    // silently move the rest (Map 02, 10).
    struct Layout {
        bool console;
        int16_t mapTop, mapW, mapH; // Map band, from x = 0
        int16_t cx, cy;             // Viewport centre, in tile space
        int16_t stripTop, stripW, stripH;
        int16_t panelLeft, panelTop, panelW;    // Panel origin: below the strip in Carry, right of it in Console
        int16_t actionLeft, actionTop, actionW; // PREV / DETAIL / NEXT
        uint16_t pxPerStep;
    };
    Layout layout() const;

    // ---- the position model (Map 09) ---------------------------------------------------------------

    static constexpr uint32_t STALE_SECS = 2 * 60 * 60; // >= 2 h: hollow
    static constexpr uint32_t OLD_SECS = 24 * 60 * 60;  // > 24 h: hidden from discovery, FIT, selection
    static constexpr uint32_t AGE_UNKNOWN = UINT32_MAX; // No valid clock: freshness unknown, drawn solid
    static constexpr uint16_t CLUSTER_PX = 24;          // Centres closer than this merge
    static constexpr uint16_t FIT_MARGIN = 24;          // Clearance FIT leaves at the edge
    static constexpr float BEYOND_STEPS = 2.5f;         // Further than the top rung can show
    static constexpr float MOVED_PX = 6.0f;             // On-screen movement below which a packet is not worth a frame
    static constexpr float OWN_MOVE_M = 50.0f;          // PositionsApplet's own threshold
    static constexpr float RECENTRE_FRACTION = 0.25f;
    static constexpr uint8_t CHOOSER_CELLS = 4;   // Members offered before the last cell becomes "+n"
    static constexpr int16_t CHOOSER_TOP = 712;   // Carry: the member row, below the title
    static constexpr int16_t CHOOSER_H = 127;     // Carry: its height, to the panel's bottom rule
    static constexpr int16_t CHOOSER_TOP_C = 154; // Console: the first member row in the panel column
    static constexpr int16_t CHOOSER_ROW_C = 67;  // Console: one member row   // Self drift which re-centres the view

    // One positioned node, as the map sees it. Rebuilt from nodeDB on every render, so nothing here is
    // a second copy of state that nodeDB already owns.
    struct Plot {
        NodeNum num = 0;
        double lat = 0, lon = 0;
        uint32_t age = AGE_UNKNOWN;   // Position age in seconds
        uint32_t heard = AGE_UNKNOWN; // Since last heard, for the orders that cannot use distance
        bool approxAge = false;       // Age fell back to last_heard: "~" in the panel
        float distance = -1;          // Metres from us; < 0 when our own position is unknown
        int16_t degrees = -1;         // Bearing from us; < 0 when unknown
        uint32_t precision = 32;      // precision_bits as received
        int16_t hops = -1;
        bool favorite = false;
        bool stale = false;   // >= 2 h
        bool old = false;     // > 24 h
        bool beyond = false;  // Outside the top rung: left out of FIT, counted in the header
        int16_t x = 0, y = 0; // Map-band pixels, from project()
        bool onScreen = false;
        int8_t group = -1; // Cluster this marker belongs to, or -1
    };

    // Our own position, and which of the frozen four states we are in (Map 09)
    enum OwnKind : uint8_t {
        OWN_NONE,    // 1 / 3: no usable own position at all
        OWN_LIVE,    // GPS lock, or a position whose source we treat as live
        OWN_LAST_FIX // 2: GPS on, no lock, but a position we once had
    };

    struct Own {
        OwnKind kind = OWN_NONE;
        double lat = 0, lon = 0;
        uint32_t fixTime = 0; // Wall clock of the position behind OWN_LAST_FIX
        bool manual = false;  // Fixed position
        bool phone = false;   // Position from the phone
        bool valid() const { return kind != OWN_NONE; }
        bool approx() const { return kind == OWN_LAST_FIX; } // Distances read "~=" against a stale fix
    };

    // ---- view state (RAM for the session; never written to flash, Map 05) --------------------------

    enum Anchor : uint8_t { ME, NODE };

    NodeNum selected = 0; // 0 only when nothing selectable is plotted
    Anchor anchor = ME;
    NodeNum anchorNode = 0;
    uint8_t rung = RUNG_DEFAULT;
    bool fit = true;
    std::vector<NodeNum> order; // PREV / NEXT snapshot: never re-sorted live
    NodeNum backNode = 0;       // Deep-link back slot; 0 = no chip
    NodeNum deepTarget = 0;     // The explicit SHOW ON MAP target, exempt from the 24 h rule while selected
    double centreLat = 0, centreLon = 0;
    bool centred = false;    // centreLat / centreLon hold a real point
    bool viewPreset = false; // An entry point set the view itself: onForeground must not reset it

    // Cluster chooser: the panel becomes a picker without the map moving (Map 11, M3)
    bool choosing = false;
    std::vector<NodeNum> chooser;
    NodeNum priorSelection = 0; // CANCEL restores this

    // ---- per-render working state ------------------------------------------------------------------
    // Left as the last frame drew it, so a tap hit-tests exactly what the user is looking at.

    std::vector<Plot> plots;
    Own own;
    uint16_t noPosition = 0, staleCount = 0, oldCount = 0, beyondCount = 0;
    float ownLastLat = 0, ownLastLon = 0; // Last own position we accepted, for the 50 m threshold

    // ---- pipeline ----------------------------------------------------------------------------------

    // Rebuilds the model from nodeDB and settles the view against it: freshness, the selection
    // invariant, the order, the centre and the FIT step. Runs first in onRender, which is what makes the
    // frame after a wake a reconciled one rather than the snapshot's state redrawn (Map 08).
    void reconcile(const Layout &l);
    void readPlots();
    void readOwn();
    void settleSelection();
    void settleOrder();
    void settleCentre(const Layout &l);
    void applyFit(const Layout &l);
    void project(const Layout &l);
    void buildClusters(const Layout &l);

    const Plot *find(NodeNum num) const;
    bool selectable(const Plot &p) const; // In the ordinary discovery / FIT / selection set
    bool drawn(const Plot &p) const;      // Selectable, or the explicit deep-link target

    void resetView();         // Nav MAP, Home and a fresh entry: FIT, anchor ME, order re-snapshotted
    void select(NodeNum num); // Selecting never moves the map (Map 05)
    void step(int8_t delta);  // PREV / NEXT through the order snapshot, wrapping
    void zoom(int8_t delta);  // One ladder step, leaving FIT
    void toggleAnchor();
    void openDetail();

    // ---- drawing -----------------------------------------------------------------------------------

    void drawHeaderBand(const Layout &l);
    void drawMapBand(const Layout &l);
    void drawStrip(const Layout &l);
    void drawPanel(const Layout &l);

    void drawUs(const Layout &l, int16_t x, int16_t y);
    void drawRings(const Layout &l, int16_t ux, int16_t uy, std::vector<Rect> &taken);
    void drawMarkers(const Layout &l, std::vector<Rect> &taken);
    void drawEdgeTag(const Layout &l, const std::string &name, const std::string &away, int16_t degrees);
    void drawScaleBar(const Layout &l);
    void drawActions(const Layout &l);
    void drawChooser(const Layout &l);
    void drawArrow(int16_t cx, int16_t cy, int16_t degrees, Color color); // Eight-point: no Win-1253 glyph
    void dottedRule(int16_t left, int16_t y, uint16_t w);
    void drawCell(int16_t left, int16_t top, uint16_t w, uint16_t h, const char *cap, const std::string &value, bool inverted,
                  bool struck, bool bold);

    std::string countsLine() const;
    std::string gpsLine() const;
    std::string distanceString(float metres) const;
    std::string ageString(uint32_t secs) const;
    std::string chooserFacts(NodeNum num) const;

    // Order position of the selection, "6 / 10" or "\x97 / 10" for a target outside the order
    std::string orderLabel() const;

    int16_t selectionIndex() const; // Index in order, or -1

    void handleMapTap(const Layout &l, int16_t tx, int16_t ty);
    void handlePanelTap(const Layout &l, int16_t tx, int16_t ty);
    bool edgeTagHit(const Layout &l, int16_t tx, int16_t ty, double lat, double lon);

    bool place(std::vector<Rect> &taken, const Rect &r) const; // Free? Then take it
};

} // namespace NicheGraphics::InkHUD

#endif

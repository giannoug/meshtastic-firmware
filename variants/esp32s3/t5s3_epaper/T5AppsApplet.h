#ifdef MESHTASTIC_INCLUDE_INKHUD

/*

T5 Apps: the sixth destination. A short, fixed shelf of mesh tools - Trace Route, RF Status and
Sensors - above the T5 nav (Apps 01, 03).

One registered applet for the whole shelf. Each tool is a mode of this class, not an applet of its
own: InkHUD stores applet choices by index in a fixed 16-slot array, the T5 build already fills 15
of those, and a tool is a screen rather than something a user cycles to. So the launcher and all
three tools share this one slot, and the mode enum below is the whole router (Apps 05 N5).

The node picker is the one exception, and it is not a user applet either: the frozen picker is a
task layer - no nav, CANCEL instead, and one Home press peels it rather than going Home - which only
a system applet can be. See T5TracePickTask.

Carry and Console are two compositions of the same model, picked from the live dimensions. Nothing
here is written to flash (Apps 11).

*/

#pragma once

#include "configuration.h"

#include "./T5Applet.h"

#include "concurrency/OSThread.h"
#include "mesh/MeshModule.h"
#include "modules/TraceRouteModule.h"

namespace NicheGraphics::InkHUD
{

class T5AppsApplet : public T5Applet, public MeshModule, public concurrency::OSThread
{
  public:
    T5AppsApplet();

    // The fixed catalogue, in frozen order. Hidden entries collapse without leaving a gap (Apps 04)
    enum class Tool : uint8_t { TRACE_ROUTE, RF_STATUS, SENSORS };
    static constexpr uint8_t TOOL_COUNT = 3;

    // Hidden: the device can never do this, so no card at all. Blocked: it could, once the user
    // changes a setting they can reach on this device, so the card stays and says which (Apps 04)
    enum class Avail : uint8_t { HIDDEN, BLOCKED, READY };

    static T5AppsApplet *get(); // Null before registration

    static void onModeChanged(); // Re-places the picker's own full-screen tile

    // The picker hands its result back here, or reports that it was cancelled
    static void targetPicked(NodeNum num);
    static void pickCancelled();

    void onActivate() override;
    void onDeactivate() override;
    void onForeground() override;
    void onBackground() override;
    void onRender(bool full) override;
    bool onTouchPoint(uint16_t x, uint16_t y, bool longPress) override;
    void onNavUp() override;
    void onNavDown() override;

    bool wantPacket(const meshtastic_MeshPacket *p) override;
    ProcessMessage handleReceived(const meshtastic_MeshPacket &mp) override;

  protected:
    // One slow tick. It exists for deadlines the frozen UI promises and nothing else: the trace
    // reply window, the moment the cooldown ends, and the >= 60 s value refresh (Apps 12)
    int32_t runOnce() override;

  private:
    Destination currentDestination() override { return APPS; }

    // ---- internal router (Apps 07 N1, N5) ----
    enum class Mode : uint8_t { LAUNCHER, TRACE, RF, SENSORS, FAILED };

    Mode mode = Mode::LAUNCHER;
    Tool failedTool = Tool::SENSORS; // Which tool X1 is apologising for
    void openTool(Tool t);
    void backToLauncher();

    // ---- catalogue ----
    struct Entry {
        Tool id;
        const char *code; // Two-letter badge: a scanning aid, never the only label
        const char *name;
        const char *desc;
    };
    static const Entry CATALOGUE[TOOL_COUNT];

    Avail availability(Tool t) const;
    const char *blockedReason(Tool t) const; // Short caps reason for a BLOCKED card, else nullptr
    std::string statusLine(Tool t) const;    // The card's live line, <= 40 chars
    std::vector<Tool> visibleTools() const;  // READY and BLOCKED, in catalogue order

    // ---- launcher (Apps 06, 07, 08) ----
    uint16_t launcherFirst = 0; // Index of the first visible entry, so a mode switch keeps it on screen
    uint8_t cardsPerPage();
    uint8_t launcherPages();
    void renderLauncher();
    void drawCard(int16_t left, int16_t top, uint16_t w, uint16_t h, Tool t);
    // Rows on screen and what a tap on each opens, rebuilt every launcher render
    std::vector<Tool> cardsShown;
    std::vector<int16_t> cardTops;
    int16_t cardLeftEdge = 0, cardWidth = 0, cardHeight = 0;
    uint8_t cardColumns = 1;

    // ---- trace route (Apps 09) ----
    // Exactly the frozen states. The module keeps its own; ours is what the screen promises, which
    // outlives it: the module clears its target at 10 s and we still owe the user a 60 s window
    enum class Trace : uint8_t { NO_TARGET, READY, TRACING, RESULT, NO_REPLY, BLOCKED };

    Trace traceState = Trace::NO_TARGET;
    NodeNum traceTarget = 0;
    uint32_t traceRequestId = 0; // The request we are waiting for, from the module
    uint32_t traceSentAtMs = 0;
    uint32_t traceSentAtTime = 0;   // Wall clock of the send, for the frozen hh:mm:ss copy
    bool haveResult = false;        // A result for traceTarget, this session
    TraceRouteResult traceResult{}; // As the module decoded it
    uint32_t traceReplyMs = 0;      // Send to result, measured here
    uint32_t traceResultAtTime = 0; // Wall clock of the reply
    bool cooldownWasActive = false; // So the one FAST when it ends fires exactly once

    void startTrace();
    void adoptResult(); // Pull a matching result out of the module, if there is one
    void renderTrace();
    void renderTraceCarry();
    void renderTraceConsole();
    bool traceBlocked() const;
    uint32_t traceDeadlineMs() const;
    std::string traceSummary() const;                 // "3 hops towards \xB7 2 hops back \xB7 reply in 4.1 s"
    void drawRouteSections(int16_t &y, bool console); // TOWARDS and BACK, never merged
    void drawResultActions(int16_t &y);               // RUN AGAIN (+ when) and NEW TARGET

    // ---- rf status (Apps 15) ----
    void renderRf();

    // ---- sensors (Apps 18, 19) ----
    void renderSensors();
    // One frozen section: a detected sensor and the cells it actually reports
    struct Reading {
        const char *key;
        std::string value;
    };
    struct Section {
        std::string label;             // "ENVIRONMENT \xB7 BME280"
        std::vector<Reading> readings; // Empty: found, nothing measured yet (S2)
    };
    std::vector<Section> sensorSections() const;

    // Action hit targets collected during a render, so touch never re-derives geometry
    enum class Hit : uint8_t { NONE, CHOOSE_NODE, START_TRACE, RUN_AGAIN, NEW_TARGET, TARGET_CARD, BACK_TO_APPS };
    struct Target {
        Hit what;
        int16_t top, bottom, left, right;
    };
    std::vector<Target> hits;
    void addHit(Hit what, int16_t left, int16_t top, uint16_t w, uint16_t h);
    Hit hitAt(int16_t x, int16_t y) const;

    // ---- panes ----
    // Carry is one column; Console splits the body (Apps 06). Every row below measures from the
    // current pane rather than from the screen, so a tool lists its rows the same way in both.
    int16_t paneLeft = 0;
    uint16_t paneSpan = 0; // 0 means the full width
    void setPane(int16_t left, uint16_t w);
    void clearPane();
    int16_t paneL() { return paneLeft; }
    int16_t paneR() { return paneLeft + (int16_t)(paneSpan ? paneSpan : width()); }

    // ---- shared row vocabulary (Apps 05 N4, Menu geometry) ----
    // y is advanced by each row, so a tool lists its rows top to bottom and never computes offsets
    void rowSection(int16_t &y, const std::string &label);
    void rowInfo(int16_t &y, const std::string &label, const std::string &value);
    void rowFacts(int16_t &y, const std::vector<Reading> &cells, uint8_t cols, bool large);
    void rowNote(int16_t &y, const std::string &text);
    void rowBullet(int16_t &y, const std::string &text);
    void rowBig(int16_t &y, const std::string &title, const std::string &body, int16_t pad);
    void rowBanner(int16_t &y, const std::string &title, const std::string &body);
    // What a hop box says the node was: an endpoint is filled, a relay outlined, a relay that could
    // not decode the packet dashed (Apps T5)
    enum HopKind : uint8_t { HOP_END, HOP_MID, HOP_UNK };
    void rowHop(int16_t &y, uint8_t n, const std::string &name, const std::string &sub, const std::string &snr, HopKind kind);
    void rowBarHeader(int16_t &y);
    void rowBar(int16_t &y, const std::string &label, uint32_t rx, uint32_t tx, uint32_t max);
    void rowTarget(int16_t &y, bool allowChange, bool small);
    // An action box. `enabled` false draws the frozen dashed box with its reason chip instead
    void rowAction(int16_t &y, Hit what, const std::string &label, bool inverted, bool enabled = true,
                   const std::string &reason = "");

    // ---- header, paging, redraw discipline ----
    // The frozen header: title, the battery figure, and a context line naming the tool's subject.
    // `back` empty on the launcher, which has no Back (Apps 06)
    void drawHeader(const std::string &title, const std::string &context, const char *back);
    int16_t headerHeight() { return isConsole() ? CONSOLE_BAND + 1 : 100; }
    int16_t bodyBottom(); // Above the nav, or above the pager when one is shown
    void drawPager(uint8_t page, uint8_t pages);
    bool pagerShown = false;
    int16_t carryBackRight = 200; // The Carry Back target ends here: the drawn title's right edge + 12 (D12)
    int16_t pagerTop = 0;

    // Apps 12: a value redraw is worth at most one frame a minute, and only when its text actually
    // changed. Holds the last line we drew so an unchanged poll asks for nothing.
    std::string lastValueText;
    uint32_t lastValueRedrawMs = 0;
    std::string valueSignature();  // The formatted values the open tool shows, concatenated
    std::string rfSignature();     // The RF figures, formatted exactly as the screen shows them
    std::string sensorSignature(); // Likewise for the sensor readings
};

} // namespace NicheGraphics::InkHUD

#endif

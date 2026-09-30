#ifdef MESHTASTIC_INCLUDE_INKHUD

/*

The T5 system layer: the one owner of every T5 system surface.

It is an arbiter, not a window manager. At most one of banner / card / terminal
is visible at a time, higher priority hides (never dismisses) lower, and most
events never earn a surface at all - they update the owning screen's own text
and stop there. System UI 01-05.

    P0  TERMINAL   restarting, switched off, battery empty        replaces
everything P1  CARD       critical battery > pairing                     covers
the nav with its own band P2  BANNER     incoming messages, as one aggregate
sits above the nav P3  BANNER     low battery, pairing not completed same shell,
no OPEN P4  STATUS     everything else                                not a
surface

The lock band is a STATUS-class surface of its own: it takes the nav slot
whenever touch is gated and coexists with a banner or a card, so it is tracked
separately from that one-slot priority ladder.

One applet, because the priority ladder is one decision. Splitting the surfaces
across applets would mean deciding it once per applet.

*/

#pragma once

#include "configuration.h"

#include "T5Applet.h"
#include "concurrency/OSThread.h"
#include "graphics/niche/InkHUD/SystemApplet.h"
#include "mesh/MeshTypes.h"
#include "mesh/generated/meshtastic/mesh.pb.h"

namespace NicheGraphics::InkHUD
{

class T5SystemLayer : public SystemApplet, public concurrency::OSThread
{
  public:
    T5SystemLayer();

    static void begin();                // Construct, register, and wire the observers
    static T5SystemLayer *get();        // Null before begin()
    static T5SystemLayer *getIfBegun(); // Same, for callers that run during startup

    // Which card is holding the P1 slot. Declared in priority order: a higher one
    // pre-empts a lower.
    enum class Card : uint8_t { NONE, PAIRING, CRITICAL_BATTERY };

    // Which notice is holding the P3 info slot. Declared in priority order.
    enum class Info : uint8_t { NONE, PAIRING_FAILED, LOW_BATTERY };

    // The P0 slot. Each one is a final image: it replaces the frame and ignores
    // all input.
    enum class Terminal : uint8_t { NONE, RESTARTING, SWITCHED_OFF, BATTERY_EMPTY };

    // Geometry is frozen per mode and must not be derived from the other one
    // (System UI 14).
    struct Geometry {
        int16_t bannerTop, bannerH; // Banner rect, full width, 4 px top rule
        int16_t tagTop, tagH;       // Tag chip and sender share this line
        int16_t previewTop,
            previewH; // The preview's hard two-line box: the banner never grows
        int16_t previewLeft, previewW;
        int16_t cardTop;               // Card rect runs from here to the bottom edge
        int16_t bandTop;               // Nav slot: the card button band and the lock band
        int16_t actionTop, actionH;    // The two banner buttons
        int16_t openLeft, openW;       // OPEN / MESSAGES
        int16_t dismissLeft, dismissW; // DISMISS
        int16_t hitTop, hitH;          // Button hit rects, taller than the visible buttons
        int16_t hitLeft, hitRight;     // Outer edges of the pair
        int16_t hitSplit;              // Where the two hit rects meet: no dead zone, no overlap
        int16_t pad;                   // Card and terminal body side padding
        int16_t headerH;               // Frozen header band, as this mode's screens use it
    };
    const Geometry &geometry() const;

    // --- events in
    // ---------------------------------------------------------------------------

    void onTimeoutGateSet();   // The display timeout gated the UI: capture the
                               // ASLEEP stamp
    void onTimeoutGateClear(); // A real user wake: reconcile, then let one frame
                               // be drawn
    void onTouchPreferenceChanged();

    // Carry <-> Console. Our tile spans the screen, and updateLayout() has just swapped its width and
    // height, so the region is re-set here. Every coordinate moved, so the gate re-arms too (G6).
    void onModeChanged();

    // Whether a surface is covering screen content right now. A T5 screen
    // underneath asks this before a timed redraw of its own: it keeps its state
    // up to date, but it does not request a refresh for a change only it can see,
    // which would erase the surface on top of it (System UI 18, R2).
    bool coversContent() const;

    void showTerminal(Terminal t);

    // The frozen banner deep link: the represented conversation, or the Messages
    // root for a batch
    void openMessaging();

    // The firmware is shutting down because the battery is empty, not because the
    // user asked. Called from the PowerFSM low-battery entry, which is the only
    // place that knows the difference.
    static void noteBatteryEmptyShutdown();

    // --- InkHUD hooks
    // ------------------------------------------------------------------------

    void onRender(bool full) override;
    bool onTouchPoint(uint16_t x, uint16_t y, bool longPress) override;
    void onExitShort() override; // The T5 Home action peels one layer at a time
    void onShutdown() override;  // SWITCHED OFF, or BATTERY EMPTY on the low-voltage path
    void onReboot() override;    // RESTARTING
    int32_t runOnce() override;

  private:
    // --- surface state
    // ----------------------------------------------------------------------

    Card cardVisible = Card::NONE;
    Card cardHeld = Card::NONE; // At most one, per System UI 05
    Info info = Info::NONE;
    Terminal terminal = Terminal::NONE;

    // The message aggregate: what arrived since the user last looked. RAM only,
    // never persisted.
    struct Aggregate {
        uint16_t count = 0;        // Messages, not conversations
        uint8_t conversations = 0; // Distinct senders / channels, capped at CONVERSATION_CAP
        bool overflowed = false;   // More conversations than we counted: "8+"
        bool hasDM = false;        // A DM in the batch always keeps the headline
        bool fromSleep = false;    // Folded in while gated: the WHILE ASLEEP wake summary
        NodeNum sender = 0;        // Newest headline sender, for the conversation OPEN goes to
        std::string shortName;
        std::string longName;
        std::string preview;
        uint8_t channel = 0; // Channel index, when the headline is channel traffic
        bool channelHeadline = false;
        uint32_t timestamp = 0; // Newest message, for the HH:MM on the right
        bool empty() const { return count == 0; }
    };
    Aggregate messages;

    static constexpr uint8_t CONVERSATION_CAP = 8;
    uint32_t conversationIds[CONVERSATION_CAP] = {};

    void clearAggregate();
    void clearInfo();
    void raiseInfo(Info kind);
    void noteConversation(uint32_t id);

    // --- the lock band
    // ----------------------------------------------------------------------

    bool stampValid = false;
    uint32_t stampEpoch = 0; // Captured once, when the gate is set. Never ticks while asleep.

    // --- timers
    // -----------------------------------------------------------------------------
    //
    // Two clocks, deliberately. Every Phase 3 timer names which one it uses.
    //
    //   AWAKE      awakeMs below. Stops accumulating while the snapshot gate is
    //   set, so a banner does
    //              not expire merely because the display slept through its
    //              timeout (System UI 08). Used by: message banner (Carry 60 s),
    //              info banner once shown (30 s).
    //   WALL       millis(). Runs regardless of the gate. Expiry while gated
    //   draws nothing; the
    //              expired surface is removed in the wake reconcile, before the
    //              wake frame. Used by: info banner never shown (10 min), message
    //              batch window (10 s), power evaluation (60 s), surface arming
    //              (300 ms).
    //
    // The pairing card has no T5 timer at all: the BLE stack owns its lifetime.

    uint32_t awakeMs = 0; // Milliseconds spent not snapshot-gated, monotonic
    uint32_t lastTickMs = 0;

    uint32_t messageExpiresAtAwakeMs = 0; // 0 = no timeout (Console, decision 3)
    uint32_t messageBatchUntilMs = 0;     // Coalesce window: redraw at most once per 10 s
    bool messagePending = false;          // Aggregate changed but the batch window has not closed
    bool messagePendingDM = false;        // ...and a DM is part of what is owed, so it may redraw in the dark

    uint32_t infoExpiresAtAwakeMs = 0; // Once shown
    uint32_t infoDropAtMs = 0;         // Never shown: independent 10 min lifetime

    static constexpr uint32_t MESSAGE_TIMEOUT_MS = 60 * 1000UL; // Carry only
    static constexpr uint32_t MESSAGE_BATCH_MS = 10 * 1000UL;
    static constexpr uint32_t INFO_SHOWN_MS = 30 * 1000UL;
    static constexpr uint32_t INFO_UNSEEN_MS = 10 * 60 * 1000UL;
    static constexpr uint32_t ARM_MS = 300; // System UI G1

    // --- stale-touch gate (System UI G1-G8)
    // --------------------------------------------------

    uint32_t renderedAtMs = 0; // The frame carrying the new surface was drawn;
                               // its refresh may still run
    uint32_t armedAtMs = 0;    // Refresh finished with no finger down: the 300 ms starts here
    bool armPending = false;   // A surface appeared; arming waits for the refresh
                               // and a finger-free panel
    void disarm();             // A new appearance: G1 applies again
    void tryArm();             // Checks G1's three conditions; called from runOnce
    bool armed() const;

    // --- arbitration
    // ------------------------------------------------------------------------

    void reconcile();            // Drop expired surfaces, promote held ones, set foreground
                                 // and tile
    void requestSurfaceUpdate(); // One request for the whole layer, never one per
                                 // surface
    void restoreCoveredScreen(); // A surface has gone: reconcile, re-arm, bring the screen back (R1)

    bool messageVisible() const;
    bool infoVisible() const;
    bool bandVisible() const;

    // --- drawing
    // ----------------------------------------------------------------------------

    void drawBand();
    void drawBanner();
    void drawBannerButton(int16_t left, int16_t w, const std::string &label); // Outlined, 2 px
    void drawCard();
    void drawCardRow(int16_t top, const char *label, const std::string &value);
    void drawTerminal();
    void drawTerminalRow(int16_t top, const char *label, const std::string &value);
    void drawButtonBand(const char *verb); // A card's single-verb band, in the nav position
    void clearSurface(int16_t top, int16_t bottom);

    void printBold(int16_t x, int16_t y,
                   const std::string &text); // Top-aligned, left edge
    void printBoldRight(int16_t right, int16_t y,
                        const std::string &text); // Top-aligned, right edge

    std::string stampText(bool twoLine, std::string *second) const;

    // --- observers
    // --------------------------------------------------------------------------

    CallbackObserver<T5SystemLayer, const meshtastic_MeshPacket *> textMessageObserver =
        CallbackObserver<T5SystemLayer, const meshtastic_MeshPacket *>(this, &T5SystemLayer::onReceiveTextMessage);
    int onReceiveTextMessage(const meshtastic_MeshPacket *p);

    CallbackObserver<T5SystemLayer, const meshtastic::Status *> bluetoothStatusObserver =
        CallbackObserver<T5SystemLayer, const meshtastic::Status *>(this, &T5SystemLayer::onBluetoothStatusUpdate);
    int onBluetoothStatusUpdate(const meshtastic::Status *status);

    // --- power watcher (System UI 10, Lock and Power 10)
    // -------------------------------------

    uint32_t lastPowerCheckMs = 0;
    int8_t pendingLowSamples = 0; // A crossing must hold for 2 consecutive checks
    int8_t pendingCriticalSamples = 0;
    bool lowArmed = true; // Fires once per crossing; re-arms above the hysteresis point
    bool criticalArmed = true;
    void checkPower();

    static constexpr uint8_t LOW_ENTER_PCT = 20;
    static constexpr uint8_t LOW_REARM_PCT = 25;
    static constexpr uint8_t CRITICAL_ENTER_PCT = 5;
    static constexpr uint8_t CRITICAL_REARM_PCT = 8;
    static constexpr uint32_t POWER_CHECK_MS = 60 * 1000UL;

    // --- pairing
    // ----------------------------------------------------------------------------

    std::string passkey;
};

} // namespace NicheGraphics::InkHUD

#endif

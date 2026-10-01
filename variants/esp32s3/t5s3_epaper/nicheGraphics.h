/*

Most of the Meshtastic firmware uses preprocessor macros throughout the code to support different hardware variants.
NicheGraphics attempts a different approach:

Per-device config takes place in this setupNicheGraphics() method
(And a small amount in platformio.ini)

This file sets up InkHUD for the LilyGo T5-E-Paper-S3-Pro.

The board uses a 4.7" ED047TC1 parallel e-paper display (960×540, 8-bit parallel interface).
This is driven via the FastEPD library through the NicheGraphics ED047TC1 driver adapter.

*/

#pragma once

#include "configuration.h"

#ifdef MESHTASTIC_INCLUDE_NICHE_GRAPHICS

// InkHUD-specific components
// ---------------------------
#include "graphics/niche/InkHUD/InkHUD.h"

// Applets
#include "T5AppsApplet.h"
#include "T5ComposeTask.h"
#include "T5ConversationApplet.h"
#include "T5HomeApplet.h"
#include "T5MapApplet.h"
#include "T5MenuApplet.h"
#include "T5MenuTask.h"
#include "T5MessageModel.h"
#include "T5MessagesApplet.h"
#include "T5NodeDetailApplet.h"
#include "T5NodesApplet.h"
#include "T5SystemLayer.h"
#include "T5TracePickTask.h"
#include "graphics/niche/InkHUD/Applets/User/AllMessage/AllMessageApplet.h"
#include "graphics/niche/InkHUD/Applets/User/DM/DMApplet.h"
#include "graphics/niche/InkHUD/Applets/User/Heard/HeardApplet.h"
#include "graphics/niche/InkHUD/Applets/User/Positions/PositionsApplet.h"
#include "graphics/niche/InkHUD/Applets/User/RecentsList/RecentsListApplet.h"
#include "graphics/niche/InkHUD/Applets/User/ThreadedMessage/ThreadedMessageApplet.h"
#include "graphics/niche/InkHUD/Applets/User/Waypoints/WaypointListApplet.h"

// Shared NicheGraphics components
// --------------------------------
#include "graphics/niche/Drivers/EInk/ED047TC1.h"
#include "graphics/niche/Inputs/TwoButton.h"

#ifdef T5_INKHUD_SCREENSHOT
#include "T5Screenshot.h"
#endif

void setupNicheGraphics()
{
    using namespace NicheGraphics;

    // E-Ink Driver
    // -----------------------------
    // The ED047TC1 is a parallel display - no SPI bus setup needed.
    // begin() args are part of the EInk interface but are ignored for parallel displays.

#ifdef T5_INKHUD_SCREENSHOT
    Drivers::EInk *driver = new T5Screenshot::Driver; // ED047TC1 that exposes the framebuffer
#else
    Drivers::EInk *driver = new Drivers::ED047TC1;
#endif
    driver->begin(nullptr, 0, 0, 0);

    // InkHUD
    // ----------------------------

    InkHUD::InkHUD *inkhud = InkHUD::InkHUD::getInstance();

    // Set the driver
    inkhud->setDriver(driver);

    // Set how many FAST updates per FULL update
    // Set how unhealthy additional FAST updates beyond this number are
    inkhud->setDisplayResilience(7, 1.5);

    // Prepare fonts - use larger sizes to suit the 4.7" screen at ~234 DPI
    InkHUD::Applet::fontLarge = FREESANS_24PT_WIN1253;
    InkHUD::Applet::fontMedium = FREESANS_18PT_WIN1253;
    InkHUD::Applet::fontSmall = FREESANS_12PT_WIN1253;

    // Customize default settings
    // One tile, always: every native screen is a full-screen composition with frozen pixel geometry.
    // This is only the first-boot default - saved settings replace it wholesale - so T5HomeApplet::begin()
    // pins the same invariant again once they are loaded.
    inkhud->persistence->settings.userTiles.maxCount = 1;
    inkhud->persistence->settings.rotation = static_cast<uint8_t>(T5Mode::CARRY); // First-boot default: Carry (portrait)
    inkhud->persistence->settings.userTiles.count = 1; // One tile only by default, keep things simple for new users
    inkhud->persistence->settings.optionalFeatures.batteryIcon = false; // T5 UI draws its own battery status
    inkhud->persistence->settings.optionalMenuItems.backlight = false;

    // Alignment must cancel rotation for visual-frame touch input: (rotation + alignment) % 4 == 0.
    inkhud->persistence->settings.joystick.alignment = (4 - inkhud->persistence->settings.rotation) % 4;

    // Pick applets
    // Note: order of applets determines priority of "auto-show" feature
    // No stock applet autoshows on T5: the foreground screen changes only when
    // the user navigates. The four shared messaging applets stay registered so
    // every saved applet index keeps its meaning, but they are never user-facing:
    // T5HomeApplet::begin() deactivates them once saved settings are loaded, so
    // native Messaging is the only messaging UI (Messaging Final 34). Channel
    // text now reaches MessageStore through T5MessageModel instead, for every
    // enabled channel rather than only these two.
    // InkHUD saves applet choices by index into fixed MAX_USERAPPLETS_GLOBAL arrays, and
    // WindowManager::addApplet() writes settings->userApplets.active[size() - 1] with no bounds
    // check - so one registration too many is an out-of-bounds write rather than a dropped applet.
    // Count them here, where the T5 list actually lives, and fail loudly at boot instead.
    uint8_t t5Registered = 0;
    auto t5AddApplet = [&](const char *name, InkHUD::Applet *applet, bool active, bool autoshow, uint8_t onTile = (uint8_t)-1) {
        assert(t5Registered < InkHUD::Persistence::MAX_USERAPPLETS_GLOBAL);
        t5Registered++;
        inkhud->addApplet(name, applet, active, autoshow, onTile);
    };

    t5AddApplet("All Messages", new InkHUD::AllMessageApplet, false,
                false);                                     // Retired on T5
    t5AddApplet("DMs", new InkHUD::DMApplet, false, false); // Retired on T5
    t5AddApplet("Channel 0", new InkHUD::ThreadedMessageApplet(0), false,
                false); // Retired on T5
    t5AddApplet("Channel 1", new InkHUD::ThreadedMessageApplet(1), false,
                false); // Retired on T5
    t5AddApplet("Positions", new InkHUD::PositionsApplet, false,
                false); // Retired on T5
    t5AddApplet("Waypoints", new InkHUD::WaypointListApplet, false,
                false); // Out of v1
    t5AddApplet("Recents List", new InkHUD::RecentsListApplet, true,
                false); // Activated, not autoshown
    t5AddApplet("Heard", new InkHUD::HeardApplet, true,
                false); // Activated, not autoshown
    // Slot 8 of sixteen: the native menu, in place of the shared Favorites Map that the native Map
    // retired. The menu is a destination and a peer of Home / Nodes / Messages / Map / Apps, so it is
    // a user applet like them - a system applet cannot be the base screen without leaving the applet
    // underneath alive and rendering. Replaced in place, so the total stays at sixteen and no later
    // index moves; T5HomeApplet::begin() handles the stale slot-8 state a saved file may hold.
    t5AddApplet(InkHUD::T5MenuApplet::APPLET_NAME, new InkHUD::T5MenuApplet, true,
                false); // Activated, not autoshown
    // T5 applets go last: InkHUD saves applet choices by index, so appending
    // keeps existing saved indices valid
    t5AddApplet("Home", new InkHUD::T5HomeApplet, true, false,
                0); // Activated, not autoshown, default on tile 0
    t5AddApplet("Nodes", new InkHUD::T5NodesApplet, true,
                false); // Activated, not autoshown
    t5AddApplet("Node Detail", new InkHUD::T5NodeDetailApplet, true,
                false); // Activated, not autoshown
    // Appended after the Phase 2 T5 applets, for the same reason: no existing
    // saved index moves
    t5AddApplet("Messages", new InkHUD::T5MessagesApplet, true,
                false); // Activated, not autoshown
    t5AddApplet("Conversation", new InkHUD::T5ConversationApplet, true,
                false); // Activated, not autoshown
    // Appended for the same reason again: the native map takes slot 15 of 16, and
    // no saved index moves. The shared Positions / Waypoints applets above stay
    // registered but inactive, so they keep their indices while no user-facing
    // route can reach them; T5HomeApplet::begin() holds them off whatever a
    // saved settings file says (Map 12, D6).
    t5AddApplet("Map", new InkHUD::T5MapApplet, true,
                false); // Activated, not autoshown
    // Appended once more, for the same reason: Apps takes slot 16 of 16 and no saved index moves.
    // This is the last user applet the T5 build has room for - the three tools inside it are modes
    // of this one applet, not applets of their own, and the node picker is a system applet (Apps 03)
    t5AddApplet("Apps", new InkHUD::T5AppsApplet, true,
                false); // Activated, not autoshown

    // Touch-capable board: InkHUD needs to know whether touch is live. The shared TOUCH OFF strip it
    // would otherwise raise is suppressed below - the T5 lock band replaces it.
    inkhud->setTouchEnabledProvider(isTouchInputEnabled);

    // T5 draws its own system surfaces: banner, card, lock band and the terminal power states. The
    // shared equivalents must not also appear. Inert on every other InkHUD board.
    inkhud->replacesStockSystemSurfaces = true;

    // Hold the image while the display timeout has the UI gated: the base screen is a snapshot until
    // the user wakes the device. Keyed on the timeout gate alone - a manual TOUCH OFF keeps rendering.
    inkhud->setRenderHoldProvider(T5UiGate::snapshotHeld);

    // Start running InkHUD
    inkhud->begin();
    InkHUD::T5MessageModel::begin(); // Before the surfaces which read it, and
                                     // before they register with it
    InkHUD::T5HomeApplet::begin();   // Saved settings loaded by begin() override
                                     // the defaults above
    // The menu itself is a user applet, registered above and brought up by the ordinary showApplet
    // path, so there is nothing to begin here. Only its task layer is a system applet.
    InkHUD::T5MenuTask::begin();      // Its own pickers and confirmations, drawn over it
    InkHUD::T5TracePickTask::begin(); // A task layer, like the composer: before the system layer
    InkHUD::T5ComposeTask::begin();   // Before the system layer: a task's input
                                      // claim is checked first (System UI 04)
    InkHUD::T5MapApplet::begin();     // After begin(): needs its registered index
    InkHUD::T5SystemLayer::begin();   // Last: the system layer draws over every
                                      // other surface
#ifdef T5_INKHUD_SCREENSHOT
    T5Screenshot::begin();
#endif
    // The front light is a persisted preference of its own, so restore it here rather than leaving it
    // at the compiled-in default.
    t5BacklightSetUserEnabled(t5BacklightLoadPreference());

    // Arm GT911 capacitive-home callback only after InkHUD startup is complete.
    t5SetHomeCapButtonEventsEnabled(true);

    // Keep Wireless Paper single-button semantics regardless of persisted settings:
    // short press advances, long press opens menu/selects.
    inkhud->persistence->settings.joystick.enabled = false;

    // Buttons
    // --------------------------

    Inputs::TwoButton *buttons = Inputs::TwoButton::getInstance(); // A shared NicheGraphics component

    // #0: BOOT button (primary user input for InkHUD navigation on T5-S3)
#if defined(T5_S3_EPAPER_PRO_V1)
    buttons->setWiring(0, PIN_BUTTON2);
#else
    buttons->setWiring(0, BUTTON_PIN);
#endif
    // Short press is the T5 Home action, or nothing at all when it is the press that woke the display.
    // Long press has no production UI action: the stock applet cycling and the shared InkHUD Menu are
    // not user-facing on T5.
#ifdef T5_INKHUD_SCREENSHOT
    // The screenshot chord consumes its BOOT press
    buttons->setHandlerShortPress(0, []() {
        if (!T5Screenshot::swallowBoot())
            t5BootShortPress();
    });
    buttons->setHandlerLongPress(0, []() {
        if (!T5Screenshot::swallowBoot())
            t5BootLongPress();
    });
#else
    buttons->setHandlerShortPress(0, []() { t5BootShortPress(); });
    buttons->setHandlerLongPress(0, []() { t5BootLongPress(); });
#endif

    buttons->start();
}

#endif

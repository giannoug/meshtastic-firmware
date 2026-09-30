#ifdef MESHTASTIC_INCLUDE_INKHUD

/*

    InkHUD's main class
    - singleton
    - mediator between the various components

*/

#pragma once

#include "configuration.h"

#include "graphics/niche/Drivers/EInk/EInk.h"

#include "./AppletFont.h"

#include <vector>

namespace NicheGraphics::InkHUD
{

// Color, understood by display controller IC (as bit values)
// Also suitable for use as AdafruitGFX colors
enum Color : uint8_t {
    BLACK = 0,
    WHITE = 1,
};

class Applet;
class Events;
class Persistence;
class Renderer;
class SystemApplet;
class Tile;
class WindowManager;

class InkHUD
{
  public:
    using TouchEnabledProvider = bool (*)();
    using RenderHoldProvider = bool (*)();

    static InkHUD *getInstance(); // Access to this singleton class

    // Configuration
    // - before InkHUD::begin, in variant nicheGraphics.h,

    void setDriver(Drivers::EInk *driver);
    void setDisplayResilience(uint8_t fastPerFull = 5, float stressMultiplier = 2.0);
    void addApplet(const char *name, Applet *a, bool defaultActive = false, bool defaultAutoshow = false, uint8_t onTile = -1);
    void notifyApplyingChanges();

    void begin();

    // Optional touch-state provider for reusable touch status indicators.
    void setTouchEnabledProvider(TouchEnabledProvider provider);
    bool hasTouchEnabledProvider() const;
    bool isTouchEnabled() const;

    // Optional render hold, for variants which want to freeze the UI while the display sleeps.
    // While the provider returns true the current image is retained as a snapshot: user applets do not
    // render, their update requests stay pending, autoshow does not run, and nothing clears the
    // framebuffer. System applets still render, as layers drawn over the held image. Without a provider
    // there is no hold, and rendering behaves exactly as it always has.
    void setRenderHoldProvider(RenderHoldProvider provider);
    bool isRenderHeld() const;

    // Set by a variant which draws its own notification, pairing, touch-status and shutdown / reboot
    // surfaces, and must not have the shared ones appear as well. The applets are still created, so
    // nothing that looks them up by name has to change; they simply never come to the foreground, and
    // LogoApplet stops drawing the shutdown and reboot screens. The boot splash is unaffected.
    // Default false: every other board behaves exactly as before.
    bool replacesStockSystemSurfaces = false;

    // Handle user-button press
    // - connected to an input source, in variant nicheGraphics.h

    void shortpress();
    void longpress();
    void exitShort();
    void exitLong();
    void navUp();
    void navDown();
    void navLeft();
    void navRight();
    void touchNavUp();
    void touchNavDown();
    void touchTap(uint16_t x, uint16_t y);
    void touchLongPress(uint16_t x, uint16_t y);

    // Freetext handlers
    void freeText(char c);
    void freeTextDone();
    void freeTextCancel();

    // Trigger UI changes
    // - called by various InkHUD components
    // - suitable(?) for use by aux button, connected in variant nicheGraphics.h

    void nextApplet();
    void prevApplet();
    NicheGraphics::InkHUD::Applet *getActiveApplet();
    void openMenu();
    void openAppSwitcher();
    void openAlignStick();
    void openKeyboard();
    void closeKeyboard();
    void nextTile();
    void prevTile();
    bool showApplet(uint8_t appletIndex, Drivers::EInk::UpdateTypes updateType = Drivers::EInk::UpdateTypes::FAST,
                    bool all = false);
    bool selectTileAt(uint16_t x, uint16_t y);
    void rotate();
    void rotateJoystick(uint8_t angle = 1); // rotate 90 deg by default
    void toggleBatteryIcon();

    // Used by TipsApplet to force menu to start on Region selection
    bool forceRegionMenu = false;

    // Input mode hint for devices that use a left/right rocker plus center button
    bool twoWayRocker = false;

    // Updating the display
    // - called by various InkHUD components

    void requestUpdate();
    void forceUpdate(Drivers::EInk::UpdateTypes type = Drivers::EInk::UpdateTypes::UNSPECIFIED, bool all = false,
                     bool async = true);
    void awaitUpdate();
    bool isUpdating(); // Non-blocking: is the panel still driving the previous update?

    // (Re)configuring WindowManager

    void autoshow();              // Bring an applet to foreground
    void updateAppletSelection(); // Change which applets are active
    void updateLayout();          // Change multiplexing (count, rotation)

    // Information passed between components

    uint16_t width();                    // From E-Ink driver
    uint16_t height();                   // From E-Ink driver
    std::vector<Tile *> getEmptyTiles(); // From WindowManager

    // Applets

    SystemApplet *getSystemApplet(const char *name);
    std::vector<Applet *> userApplets;
    std::vector<SystemApplet *> systemApplets;

    // Pass drawing output to Renderer
    void drawPixel(int16_t x, int16_t y, Color c);

    // Shared data which persists between boots
    Persistence *persistence = nullptr;

  private:
    InkHUD() {} // Constructor made private to force use of InkHUD::getInstance

    Events *events = nullptr;               // Handle non-specific firmware events
    Renderer *renderer = nullptr;           // Co-ordinate display updates
    WindowManager *windowManager = nullptr; // Multiplexing of applets
    TouchEnabledProvider touchEnabledProvider = nullptr;
    RenderHoldProvider renderHoldProvider = nullptr;
};

} // namespace NicheGraphics::InkHUD

#endif

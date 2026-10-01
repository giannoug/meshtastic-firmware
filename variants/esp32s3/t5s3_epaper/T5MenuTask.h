#ifdef MESHTASTIC_INCLUDE_INKHUD

/*

The menu's task layer: option pickers, confirmations, name fields and the applying band
(Menu Settings C3, C4, C12, C13, C17, K5, K6).

One class, because all four are the same thing in the frozen design - a full-screen layer with a
question in the header, no Back, and the nav band replaced by CANCEL and a verb. Splitting them would
mean deciding the stale-touch gate, the Home rule and the mode-switch rule once per class.

Phase 3 deliberately left the generic confirmation and busy surfaces until something needed them.
This is that something, and the rules they follow are the ones Phase 3 already set:

  - Home x1 cancels to the page underneath, x2 goes to T5 Home. Home never confirms.
  - Back never appears, so Back can never apply anything.
  - A finger already down when the layer appeared cannot activate it: taps arrive at release, so the
    gate compares when the contact began with when the layer was armed (System UI G1).
  - A system-layer card or a terminal state on top of us owns its own rectangle and its own input.
  - A mode switch preserves the whole operation: which picker, which page, which choice, which verb.

The RESTARTING and SHUT DOWN images are not ours. Those belong to T5SystemLayer's terminal states,
where they have been since Phase 3; a confirmed restart-class action schedules the reboot and the
layer draws the final frame (Menu Settings 21 of the brief).

*/

#pragma once

#include "configuration.h"

#include "./T5Keyboard.h"
#include "./T5Nav.h"
#include "./T5Settings.h"

#include "graphics/niche/InkHUD/SystemApplet.h"

namespace NicheGraphics::InkHUD
{

class T5MenuTask : public T5NavDraw<SystemApplet>
{
  public:
    T5MenuTask();

    static void begin();      // After T5MenuApplet, before the system layer
    static T5MenuTask *get(); // Null before begin()
    static bool isOpen();     // Any of the four layers is up

    static void openPicker(T5Act which, uint8_t channelIndex);
    static void openConfirm(const T5Confirm &spec, T5Act which, int32_t value);
    static void openNameField(bool shortName);

    static void onModeChanged();

    void onRender(bool full) override;
    bool onTouchPoint(uint16_t x, uint16_t y, bool longPress) override;
    void onExitShort() override; // Home x1 cancels; the next press goes Home
    void onNavUp() override;
    void onNavDown() override;
    void onApplyingChanges() override; // The T5 applying band, in place of the stock Logo screen

  private:
    enum class Mode : uint8_t { CLOSED, PICKER, CONFIRM, NAME, APPLYING };

    Mode mode = Mode::CLOSED;

    // PICKER
    T5Act pick = T5Act::NONE;
    uint8_t pickChannel = 0;
    uint8_t pickPage = 0;
    std::vector<T5Option> options; // Rebuilt on each render, so an external change cannot go unseen

    // CONFIRM
    T5Confirm spec;
    T5Act confirmAct = T5Act::NONE;
    int32_t confirmValue = 0;
    uint8_t confirmChoice = 0;

    // NAME
    bool nameShort = false;
    std::string typed; // In the display encoding the keyboard produces, as the composer's draft is
    T5Keyboard::Layer layer = T5Keyboard::Layer::LETTERS;
    T5Keyboard::Shift shift = T5Keyboard::Shift::OFF;
    uint32_t lastShiftTapMs = 0;

    // The stale-touch gate (System UI G1-G8). A layer that appears under a finger already down must
    // not activate from that contact.
    uint32_t armedAtMs = 0;
    bool armed() const;
    void arm();

    void showApplying(); // notifyApplyingChanges reached us: hold the band until the reboot
    void reconcile();
    void leave(); // Back to the menu page underneath, with nothing written

    // ---- layout, frozen per mode ----
    struct Metrics {
        int16_t headerH, bodyTop, bodyBottom, rowH, sectionH, pagerH, buttonBand;
        uint8_t columns; // Console gives the picker a second column
    };
    Metrics metrics() const;

    void drawHeader(const std::string &title, const std::string &context, const Metrics &m);
    void drawButtons(const Metrics &m, const char *cancel, const char *verb);
    void drawPicker(const Metrics &m);
    void drawConfirm(const Metrics &m);
    void drawName(const Metrics &m);
    void drawApplying(const Metrics &m);
    void drawOption(int16_t left, int16_t top, uint16_t w, int16_t h, const T5Option &o, bool console);
    void drawKeyboard();
    void drawField(const Metrics &m);

    // Option rows on screen and where each one sits, rebuilt on every render
    std::vector<uint8_t> optionsShown; // Index into `options`
    std::vector<int16_t> optionTops;
    std::vector<int16_t> optionLefts;
    std::vector<uint16_t> optionWidths;
    bool pagerShown = false;
    uint8_t pageCount = 1;
    int16_t choiceTop = 0, choiceHeight = 0; // The confirmation's one choice row, when it has one

    uint8_t optionsPerPage(const Metrics &m) const;
    void turnPage(int8_t delta);

    void type(char c);
    void backspace();
    void toggleShift();
    void commitName();

    static constexpr uint32_t ARM_MS = 300;              // System UI G1
    static constexpr uint32_t SHIFT_DOUBLE_TAP_MS = 400; // The composer's frozen value
};

} // namespace NicheGraphics::InkHUD

#endif

#if defined(T5_S3_EPAPER_PRO) && defined(MESHTASTIC_INCLUDE_INKHUD)

#include "./T5MenuTask.h"
#include "./T5Applet.h"
#include "./T5MenuApplet.h"
#include "./T5SystemLayer.h"
#include "./T5Text.h"

#include "UptimeClock.h"
#include "graphics/niche/InkHUD/InkHUD.h"
#include "graphics/niche/InkHUD/Tile.h"
#include "mesh/Throttle.h"

using namespace NicheGraphics;

namespace
{
InkHUD::T5MenuTask *instance = nullptr;

constexpr int16_t CARRY_HEADER_H = 100;
constexpr int16_t CONSOLE_HEADER_H = 64;
constexpr int16_t BUTTON_BAND = 88; // Where the nav would be: a task puts its buttons there instead
constexpr int16_t PAGER_H = 64;
constexpr int16_t CARRY_OPTION_H = 72;
constexpr int16_t CONSOLE_OPTION_H = 72;
constexpr int16_t RING = 28; // The option ring, filled when selected
} // namespace

InkHUD::T5MenuTask::T5MenuTask()
{
    // A temporary layer that replaces the whole screen, so it owns its whole region: it clears that
    // region itself rather than letting the Renderer do it, which is the only way the screen
    // underneath cannot show through a gap on a frame that re-renders everything - a wake from the
    // display timeout, a Carry / Console switch, or the applet below redrawing itself. Paired with
    // alwaysRender while open, so such a frame always redraws us last. The contract T5SystemLayer
    // already uses for its own full-screen states.
    clearsOwnRegion = true;
    (new Tile)->assignApplet(this);
    handleFreeText = false; // The name field draws the frozen T5 keyboard itself, as the composer does
}

void InkHUD::T5MenuTask::begin()
{
    assert(!instance);
    instance = new T5MenuTask;
    instance->name = "T5MenuTask";
    instance->activate();

    // After the menu, so this draws over it; before the system layer, so the layer still draws its
    // card over us and still takes the press when it does
    InkHUD::getInstance()->systemApplets.push_back(instance);
    instance->getTile()->setRegion(0, 0, InkHUD::getInstance()->width(), InkHUD::getInstance()->height());
}

InkHUD::T5MenuTask *InkHUD::T5MenuTask::get()
{
    return instance;
}

bool InkHUD::T5MenuTask::isOpen()
{
    return instance && instance->mode != Mode::CLOSED;
}

// G1: a layer that appeared under a finger already down must not activate from that contact. Taps
// arrive at release, so the gate compares when the contact began with when we were armed
void InkHUD::T5MenuTask::arm()
{
    armedAtMs = Time::getMillis();
}

bool InkHUD::T5MenuTask::armed() const
{
    if (!armedAtMs)
        return false;
    if (Throttle::isWithinTimespanMs(armedAtMs, ARM_MS))
        return false; // Still inside the frozen arming window
    // A contact that began before we appeared is the stale one the gate exists for
    return !t5TouchIsDown() || t5TouchDownAtMs() > armedAtMs;
}

void InkHUD::T5MenuTask::reconcile()
{
    const bool up = mode != Mode::CLOSED;
    handleInput = up; // While up, one Home press peels us rather than going Home
    // Drawn in every frame while we are up, so a redraw of the menu underneath cannot erase us, and
    // cleared again on close so a closed task costs nothing
    alwaysRender = up;
    if (up)
        bringToForeground();
    else
        sendToBackground();
}

void InkHUD::T5MenuTask::openPicker(T5Act which, uint8_t channelIndex)
{
    if (!instance)
        return;
    instance->mode = Mode::PICKER;
    instance->pick = which;
    instance->pickChannel = channelIndex;
    instance->pickPage = 0;
    instance->arm();
    instance->reconcile();
    instance->requestUpdate();
}

void InkHUD::T5MenuTask::openConfirm(const T5Confirm &spec, T5Act which, int32_t value)
{
    if (!instance)
        return;
    instance->mode = Mode::CONFIRM;
    instance->spec = spec;
    instance->confirmAct = which;
    instance->confirmValue = value;
    instance->confirmChoice = spec.choiceDefault;
    instance->arm();
    instance->reconcile();
    instance->requestUpdate();
}

void InkHUD::T5MenuTask::openNameField(bool shortName)
{
    if (!instance)
        return;
    instance->mode = Mode::NAME;
    instance->nameShort = shortName;
    instance->typed = t5OwnerName(shortName); // The field opens pre-filled, as the frozen row says
    instance->layer = T5Keyboard::Layer::LETTERS;
    instance->shift = T5Keyboard::Shift::OFF;
    instance->arm();
    instance->reconcile();
    instance->requestUpdate();
}

void InkHUD::T5MenuTask::showApplying()
{
    mode = Mode::APPLYING;
    arm();
    reconcile();
    requestUpdate();
}

// notifyApplyingChanges() reaches every system applet. The stock Logo screen is suppressed on T5, so
// this is the applying state - but only while the menu is the screen doing the applying
void InkHUD::T5MenuTask::onApplyingChanges()
{
    if (!T5MenuApplet::isOpen())
        return;

    // The applying band is not one of the surfaces Phase 3 lets redraw a frozen panel - those are the
    // DM banner, the pairing card, the critical-battery card and the three terminal states. So while
    // the snapshot gate is set the work proceeds but the band asks for nothing: the frozen frame is
    // left exactly as it is, no update is requested, and the front light is not touched. A
    // restart-class apply still gets its terminal frame from T5SystemLayer, which is allowed to
    // replace the snapshot at the actual transition. Anything that finished while gated is simply
    // read back from config by the wake reconcile, so there is no obsolete band to replay.
    if (T5UiGate::snapshotHeld())
        return;

    showApplying();
}

// CANCEL, Back-equivalent and the first Home press all land here: the page underneath, nothing written
void InkHUD::T5MenuTask::leave()
{
    const Mode was = mode;
    mode = Mode::CLOSED;
    reconcile();
    if (was == Mode::CONFIRM)
        T5MenuApplet::confirmCancelled();
    else
        T5MenuApplet::pickCancelled();
}

void InkHUD::T5MenuTask::onExitShort()
{
    // Home x1 cancels to the page underneath; the next press is an ordinary Home. Home never confirms
    if (mode == Mode::APPLYING)
        return; // All input is ignored until the restart arrives
    leave();
}

void InkHUD::T5MenuTask::onModeChanged()
{
    if (!instance)
        return;
    instance->getTile()->setRegion(0, 0, InkHUD::getInstance()->width(), InkHUD::getInstance()->height());
    // The operation survives the rotate: which picker, which choice, which verb, and the draft. Only
    // the page number is meaningless, because every row moved
    instance->pickPage = 0;
    // Every coordinate moved, so the stale-touch gate arms again
    instance->arm();
}

// ---------------------------------------------------------------------------- geometry

InkHUD::T5MenuTask::Metrics InkHUD::T5MenuTask::metrics() const
{
    Metrics m{};
    const bool console = t5IsConsole(*this);
    m.headerH = console ? CONSOLE_HEADER_H : CARRY_HEADER_H;
    m.bodyTop = m.headerH + 1;
    m.buttonBand = BUTTON_BAND;
    m.bodyBottom = height() - BUTTON_BAND;
    m.rowH = console ? CONSOLE_OPTION_H : CARRY_OPTION_H;
    m.sectionH = console ? 32 : 36;
    m.pagerH = PAGER_H;
    // The width buys a second column: eight options a page instead of four (K5)
    m.columns = console ? 2 : 1;
    return m;
}

uint8_t InkHUD::T5MenuTask::optionsPerPage(const Metrics &m) const
{
    const int16_t room = m.bodyBottom - m.bodyTop - m.pagerH;
    const int16_t perColumn = room / m.rowH;
    return (uint8_t)std::max<int16_t>(1, perColumn * m.columns);
}

// ---------------------------------------------------------------------------- drawing

void InkHUD::T5MenuTask::drawHeader(const std::string &title, const std::string &context, const Metrics &m)
{
    const bool console = t5IsConsole(*this);
    // The header is the question, and there is no Back: a task is left by CANCEL or by Home
    setFont(fontLarge);
    printBold(T5_MARGIN, console ? (m.headerH - fontLarge.lineHeight()) / 2 : 14, title);

    setFont(fontSmall);
    if (console) {
        printAt(width() - T5_MARGIN, (m.headerH - fontSmall.lineHeight()) / 2, context, RIGHT);
    } else {
        setCrop(T5_MARGIN, 0, (uint16_t)(width() - 2 * T5_MARGIN), m.headerH);
        printAt(T5_MARGIN, 66, context);
        resetCrop();
    }
    drawLine(0, m.headerH, width() - 1, m.headerH, BLACK);
}

// CANCEL left, the verb right and inverted. Neither is pre-selected, and the screen never
// auto-confirms (Menu Settings 07)
void InkHUD::T5MenuTask::drawButtons(const Metrics &m, const char *cancel, const char *verb)
{
    const int16_t top = height() - m.buttonBand;
    drawLine(0, top, width() - 1, top, BLACK);
    setFont(fontSmall);

    const int16_t h = fontSmall.lineHeight() + 2 * 12 + 4;
    const int16_t boxTop = top + (m.buttonBand - h) / 2;

    if (!verb) {
        // A picker has only CANCEL, centred where the nav would be
        const int16_t w = getTextWidth(cancel) + 2 * 16 + 4;
        const int16_t left = (width() - w) / 2;
        drawRect(left, boxTop, w, h, BLACK);
        drawRect(left + 1, boxTop + 1, w - 2, h - 2, BLACK);
        printBold(left + w / 2, boxTop + 12, cancel, CENTER);
        return;
    }

    const int16_t split = width() / 2;
    const int16_t cancelW = getTextWidth(cancel) + 2 * 16 + 4;
    const int16_t cancelLeft = (split - cancelW) / 2;
    drawRect(cancelLeft, boxTop, cancelW, h, BLACK);
    drawRect(cancelLeft + 1, boxTop + 1, cancelW - 2, h - 2, BLACK);
    printBold(cancelLeft + cancelW / 2, boxTop + 12, cancel, CENTER);

    const int16_t verbW = getTextWidth(verb) + 2 * 16 + 4;
    const int16_t verbLeft = split + (split - verbW) / 2;
    fillRect(verbLeft, boxTop, verbW, h, BLACK);
    setTextColor(WHITE);
    printThick(verbLeft + verbW / 2, boxTop + h / 2, verb, 2, 1);
    setTextColor(BLACK);
}

// Ring, label and detail; the selected ring is filled and the label bold (Menu Settings 03)
void InkHUD::T5MenuTask::drawOption(int16_t left, int16_t top, uint16_t w, int16_t h, const T5Option &o, bool console)
{
    const int16_t right = left + (int16_t)w;
    const int16_t ringX = left + T5_MARGIN + RING / 2;
    const int16_t centerY = top + h / 2;

    if (o.selectable) {
        drawCircle(ringX, centerY, RING / 2, BLACK);
        drawCircle(ringX, centerY, RING / 2 - 1, BLACK);
        if (o.selected)
            fillCircle(ringX, centerY, RING / 2 - 5, BLACK);
    }

    const int16_t textLeft = left + T5_MARGIN + RING + 14;
    const int16_t textRight = right - T5_MARGIN;

    if (o.detail.empty()) {
        setFont(fontMedium);
        setCrop(textLeft, top, std::max<int16_t>(0, textRight - textLeft), h);
        if (o.selected)
            printBold(textLeft, centerY - fontMedium.lineHeight() / 2, o.label);
        else
            printAt(textLeft, centerY, o.label, LEFT, MIDDLE);
        resetCrop();
    } else {
        setFont(fontMedium);
        setCrop(textLeft, top, std::max<int16_t>(0, textRight - textLeft), h);
        const int16_t labelTop = top + (console ? 8 : 10);
        if (o.selected)
            printBold(textLeft, labelTop, o.label);
        else
            printAt(textLeft, labelTop, o.label);
        resetCrop();

        setFont(fontSmall);
        setCrop(textLeft, top, std::max<int16_t>(0, textRight - textLeft), h);
        printAt(textLeft, labelTop + fontMedium.lineHeight() + 2, o.detail);
        resetCrop();
    }

    // The one option that is shown but will not be set says so in its own treatment
    if (!o.selectable) {
        setFont(fontSmall);
        t5DottedRect(*this, left + T5_MARGIN, centerY - 18, RING, 36);
    }

    drawLine(left, top + h - 1, right - 1, top + h - 1, BLACK);
}

void InkHUD::T5MenuTask::drawPicker(const Metrics &m)
{
    const bool console = t5IsConsole(*this);
    optionsShown.clear();
    optionTops.clear();
    optionLefts.clear();
    optionWidths.clear();

    drawHeader(t5PickTitle(pick), t5PickContext(pick), m);

    // Rebuilt on each render, so a value changed from the phone while the picker is open cannot go
    // unseen (Menu Settings 23)
    options = t5PickOptions(pick, pickChannel);

    const uint8_t per = optionsPerPage(m);
    pageCount = (uint8_t)std::max<size_t>(1, (options.size() + per - 1) / per);
    if (pickPage >= pageCount)
        pickPage = pageCount - 1;
    pagerShown = pageCount > 1;

    const int16_t colW = (int16_t)(width() / m.columns);
    const int16_t perColumn = per / m.columns;
    const int16_t bottom = m.bodyBottom - (pagerShown ? m.pagerH : 0);

    for (uint8_t i = 0; i < per; i++) {
        const size_t index = (size_t)pickPage * per + i;
        if (index >= options.size())
            break;
        // Column-major: the left column fills before the right one starts
        const int16_t left = console ? (int16_t)(i / perColumn) * colW : 0;
        const int16_t top = m.bodyTop + (console ? (int16_t)(i % perColumn) : (int16_t)i) * m.rowH;
        if (top + m.rowH > bottom)
            continue;
        drawOption(left, top, (uint16_t)colW, m.rowH, options[index], console);
        optionsShown.push_back((uint8_t)index);
        optionTops.push_back(top);
        optionLefts.push_back(left);
        optionWidths.push_back((uint16_t)colW);
    }
    if (console)
        drawLine(colW, m.bodyTop, colW, bottom - 1, BLACK);

    if (pagerShown) {
        const int16_t top = m.bodyBottom - m.pagerH;
        drawLine(0, top, width() - 1, top, BLACK);
        const int16_t cell = width() / 3;
        setFont(fontSmall);
        if (pickPage > 0)
            printAt(cell / 2, top + m.pagerH / 2, std::string(T5Applet::BACK) + " PREV", CENTER, MIDDLE);
        printAt(width() / 2, top + m.pagerH / 2, to_string(pickPage + 1) + " / " + to_string(pageCount), CENTER, MIDDLE);
        if (pickPage + 1 < pageCount)
            printAt(width() - cell / 2, top + m.pagerH / 2, std::string("NEXT ") + T5Applet::FWD, CENTER, MIDDLE);
    }

    drawButtons(m, "CANCEL", nullptr);
}

void InkHUD::T5MenuTask::drawConfirm(const Metrics &m)
{
    choiceTop = 0;
    choiceHeight = 0;

    drawHeader(spec.title, spec.crumb, m);

    int16_t y = m.bodyTop + 12;
    const int16_t left = T5_MARGIN, right = width() - T5_MARGIN;

    // NOW -> NEW, only when a value actually changes
    if (!spec.from.empty() || !spec.to.empty()) {
        setFont(fontSmall);
        printAt(left, y, "NOW");
        printAt(right, y, "NEW", RIGHT);
        y += fontSmall.lineHeight() + 4;
        setFont(fontMedium);
        printBold(left, y, spec.from);
        printBold(right, y, spec.to, RIGHT);
        printAt(width() / 2, y + fontMedium.lineHeight() / 2, T5Applet::FWD, CENTER, MIDDLE);
        y += fontMedium.lineHeight() + 16;
    }

    auto drawSection = [&](const std::string &label) {
        setFont(fontSmall);
        printAt(left, y + (m.sectionH - fontSmall.lineHeight()) / 2 - 2, label);
        drawLine(0, y + m.sectionH - 1, width() - 1, y + m.sectionH - 1, BLACK);
        y += m.sectionH;
    };

    auto drawBullets = [&](const std::vector<std::string> &lines) {
        setFont(fontSmall);
        for (const std::string &line : lines) {
            const std::string body = std::string(T5Applet::DASH) + " " + line;
            for (const std::string &wrapped : t5WrapLines(*this, body, (uint16_t)(right - left))) {
                if (y + fontSmall.lineHeight() > m.bodyBottom)
                    return;
                printAt(left, y, wrapped);
                y += fontSmall.lineHeight() + 2;
            }
            y += 8;
        }
    };

    if (!spec.bullets.empty()) {
        drawSection("WHAT HAPPENS");
        drawBullets(spec.bullets);
    }
    if (!spec.kept.empty()) {
        drawSection(spec.keptLabel.empty() ? "KEPT" : spec.keptLabel);
        drawBullets(spec.kept);
    }

    // The one choice row a confirmation may carry: CLEAR NODE LIST's KEEP FAVOURITES
    if (!spec.choices.empty()) {
        const int16_t cellW = t5IsConsole(*this) ? 200 : 180, cellH = 48;
        choiceTop = y + 8;
        choiceHeight = cellH + 16;
        const int16_t groupLeft = right - (int16_t)spec.choices.size() * cellW;
        setFont(fontSmall);
        for (size_t i = 0; i < spec.choices.size(); i++) {
            const int16_t cl = groupLeft + (int16_t)i * cellW;
            if (i == confirmChoice) {
                fillRect(cl, choiceTop, cellW, cellH, BLACK);
                setTextColor(WHITE);
                printThick(cl + cellW / 2, choiceTop + cellH / 2, spec.choices[i], 2, 1);
                setTextColor(BLACK);
            } else {
                drawRect(cl, choiceTop, cellW, cellH, BLACK);
                printAt(cl + cellW / 2, choiceTop + cellH / 2, spec.choices[i], CENTER, MIDDLE);
            }
        }
        y += choiceHeight;
    }

    drawButtons(m, "CANCEL", spec.verb.c_str());
}

// The frozen applying treatment. The verb is already gone - the buttons are replaced - so a finger
// still on its way down cannot land on something that has moved (System UI, busy verb disarm)
void InkHUD::T5MenuTask::drawApplying(const Metrics &m)
{
    setFont(fontLarge);
    const int16_t centerY = (m.bodyTop + m.bodyBottom) / 2;
    printBold(T5_MARGIN, centerY - fontLarge.lineHeight(), "Applying changes");
    setFont(fontSmall);
    printAt(T5_MARGIN, centerY + 8, "The device restarts in a moment.");

    // No buttons and no nav: every tap is ignored until the restart arrives
    const int16_t top = height() - m.buttonBand;
    drawLine(0, top, width() - 1, top, BLACK);
}

// ---------------------------------------------------------------------------- the name field

void InkHUD::T5MenuTask::drawField(const Metrics &m)
{
    (void)m;
    const bool console = t5IsConsole(*this);
    const int16_t top = console ? CONSOLE_HEADER_H + 12 : CARRY_HEADER_H + 16;
    const int16_t left = T5_MARGIN;
    const uint16_t w = (uint16_t)((console ? T5Keyboard::CONSOLE_KEYS_RIGHT : width()) - 2 * T5_MARGIN);
    const int16_t fieldBottom = (console ? T5Keyboard::CONSOLE_TOP : T5Keyboard::CARRY_TOP) - 12;

    drawRect(left, top, w, fieldBottom - top, BLACK);
    drawRect(left + 1, top + 1, w - 2, fieldBottom - top - 2, BLACK);

    setFont(fontLarge);
    setCrop(left + 12, top, w - 24, fieldBottom - top);
    printBold(left + 12, top + 12, typed.empty() ? std::string(T5Applet::DASH) : typed);
    resetCrop();

    // The frozen limit counts the UTF-8 result, as the composer's counter does
    setFont(fontSmall);
    const size_t limit = t5OwnerNameLimit(nameShort);
    const size_t used = t5Utf8Size(typed);
    printAt(left + w - 12, fieldBottom - fontSmall.lineHeight() - 10, to_string((int)limit - (int)used) + " LEFT", RIGHT);
}

void InkHUD::T5MenuTask::drawKeyboard()
{
    const bool console = t5IsConsole(*this);
    setFont(fontMedium);
    for (const T5Keyboard::Cell &c : T5Keyboard::cells(console, layer, shift)) {
        if (c.boundaryLeft)
            drawLine(c.left, c.top, c.left, c.bottom, BLACK);
        setFont(c.small ? fontSmall : fontMedium);
        const int16_t cx = (c.left + c.right) / 2, cy = (c.top + c.bottom) / 2;
        if (c.sub.empty()) {
            printAt(cx, cy, c.label, CENTER, MIDDLE);
        } else {
            printAt(cx, cy - fontSmall.lineHeight() / 2, c.label, CENTER, MIDDLE);
            setFont(fontSmall);
            printAt(cx, cy + fontSmall.lineHeight() / 2 + 2, c.sub, CENTER, MIDDLE);
        }
    }
    const int16_t *tops = T5Keyboard::rowTops(console);
    const int16_t rule = console ? T5Keyboard::CONSOLE_TOP : T5Keyboard::CARRY_TOP;
    fillRect(0, rule, width(), 2, BLACK);
    for (uint8_t r = 1; r < 5; r++)
        drawLine(0, tops[r], width() - 1, tops[r], BLACK);
}

void InkHUD::T5MenuTask::drawName(const Metrics &m)
{
    drawHeader(nameShort ? "SHORT NAME" : "LONG NAME",
               nameShort ? std::string("UP TO 4 CHARACTERS") : std::string("THE NAME THE MESH SEES"), m);
    drawField(m);
    drawKeyboard();
}

void InkHUD::T5MenuTask::type(char c)
{
    // The frozen limit counts the UTF-8 result, not the typed bytes
    std::string next = typed;
    next.push_back(c);
    if (t5Utf8Size(next) > t5OwnerNameLimit(nameShort))
        return;
    typed = next;
    if (shift == T5Keyboard::Shift::ONCE)
        shift = T5Keyboard::Shift::OFF;
    requestUpdate();
}

void InkHUD::T5MenuTask::backspace()
{
    if (typed.empty())
        return;
    typed.pop_back();
    requestUpdate();
}

void InkHUD::T5MenuTask::toggleShift()
{
    const uint32_t now = Time::getMillis();
    if (shift != T5Keyboard::Shift::CAPS && lastShiftTapMs && Throttle::isWithinTimespanMs(lastShiftTapMs, SHIFT_DOUBLE_TAP_MS))
        shift = T5Keyboard::Shift::CAPS; // A double tap locks CAPS
    else if (shift == T5Keyboard::Shift::CAPS)
        shift = T5Keyboard::Shift::OFF;
    else
        shift = shift == T5Keyboard::Shift::ONCE ? T5Keyboard::Shift::OFF : T5Keyboard::Shift::ONCE;
    lastShiftTapMs = now;
    requestUpdate();
}

// DONE writes and returns; CANCEL discards. The write itself belongs to the model
void InkHUD::T5MenuTask::commitName()
{
    const bool shortName = nameShort;
    const std::string utf8 = t5ToUtf8(typed);
    mode = Mode::CLOSED;
    reconcile();
    T5MenuApplet::nameEdited(shortName, utf8);
}

// ---------------------------------------------------------------------------- render

void InkHUD::T5MenuTask::onRender(bool full)
{
    (void)full;
    // alwaysRender puts us in every frame, including the one that adds the lock band when the display
    // timeout starts. While that hold is in force nothing underneath us is being redrawn either, so
    // there is nothing to cover: leave the frozen frame exactly as it is and let T5SystemLayer draw
    // the band over it, which is the one surface allowed to change a held image (Lock and Power 01).
    if (inkhud->isRenderHeld())
        return;

    // Opaque: this layer replaces the screen, and clearsOwnRegion means nothing else blanks it
    fillRect(0, 0, width(), height(), WHITE);
    const Metrics m = metrics();
    switch (mode) {
    case Mode::PICKER:
        drawPicker(m);
        return;
    case Mode::CONFIRM:
        drawConfirm(m);
        return;
    case Mode::NAME:
        drawName(m);
        return;
    case Mode::APPLYING:
        drawApplying(m);
        return;
    case Mode::CLOSED:
        return;
    }
}

// ---------------------------------------------------------------------------- input

void InkHUD::T5MenuTask::turnPage(int8_t delta)
{
    const int16_t next = (int16_t)pickPage + delta;
    if (next < 0 || next >= (int16_t)pageCount)
        return;
    pickPage = (uint8_t)next;
    requestUpdate();
}

void InkHUD::T5MenuTask::onNavUp()
{
    if (mode == Mode::PICKER)
        turnPage(-1);
}

void InkHUD::T5MenuTask::onNavDown()
{
    if (mode == Mode::PICKER)
        turnPage(1);
}

bool InkHUD::T5MenuTask::onTouchPoint(uint16_t x, uint16_t y, bool longPress)
{
    if (mode == Mode::CLOSED)
        return false;

    // A card or a terminal state is on top of us: its targets win, and the arbiter owns them
    const T5SystemLayer *system = T5SystemLayer::getIfBegun();
    if (system && system->coversContent())
        return false;

    // Every tap is ignored while the work is being applied, and so is Home
    if (mode == Mode::APPLYING)
        return true;

    if (longPress)
        return true; // Long press is unused in the menu

    // A finger that was already down when this layer appeared must not activate it
    if (!armed())
        return true;

    const int16_t tx = (int16_t)x - getTile()->getLeft();
    const int16_t ty = (int16_t)y - getTile()->getTop();
    const Metrics m = metrics();

    if (mode == Mode::NAME) {
        const bool console = t5IsConsole(*this);
        const T5Keyboard::Key key = T5Keyboard::hit(console, layer, shift, tx, ty);
        switch (key.kind) {
        case T5Keyboard::Key::CHAR:
            type(key.ch);
            return true;
        case T5Keyboard::Key::DEL:
            backspace();
            return true;
        case T5Keyboard::Key::SHIFT:
            toggleShift();
            return true;
        case T5Keyboard::Key::LAYER:
            layer = layer == T5Keyboard::Layer::LETTERS ? T5Keyboard::Layer::SYMBOLS : T5Keyboard::Layer::LETTERS;
            requestUpdate();
            return true;
        case T5Keyboard::Key::SEND:
            // The keyboard's commit key. An empty field cannot be committed: the admin path would
            // reject it, so the field keeps what it had
            if (!typed.empty())
                commitName();
            return true;
        default:
            break;
        }
        // CANCEL sits where the nav would be, as it does in every task
        if (ty >= height() - m.buttonBand) {
            leave();
            return true;
        }
        return true;
    }

    // The button band
    if (ty >= height() - m.buttonBand) {
        if (mode == Mode::PICKER) {
            leave(); // CANCEL: nothing written
            return true;
        }
        // A confirmation's two buttons meet at the middle: no dead zone, no overlap
        if (tx < width() / 2) {
            leave(); // CANCEL returns to the originating page
        } else {
            const T5Act act = confirmAct;
            // A confirmation that carries a choice row confirms with the cell, not with the value the
            // row was opened with. What the cell means is the action's business, not this layer's
            const int32_t value = spec.choices.empty() ? confirmValue : (int32_t)confirmChoice;
            mode = Mode::CLOSED;
            reconcile();
            T5MenuApplet::confirmed(act, value);
        }
        return true;
    }

    if (mode == Mode::CONFIRM) {
        // The choice row, when there is one
        if (choiceHeight && ty >= choiceTop && ty < choiceTop + choiceHeight) {
            const int16_t cellW = t5IsConsole(*this) ? 200 : 180;
            const int16_t groupLeft = width() - T5_MARGIN - (int16_t)spec.choices.size() * cellW;
            if (tx >= groupLeft) {
                const uint8_t cell = (uint8_t)((tx - groupLeft) / cellW);
                if (cell < spec.choices.size() && cell != confirmChoice) {
                    confirmChoice = cell;
                    requestUpdate();
                }
            }
        }
        return true; // Nothing else in a confirmation is a target
    }

    // The picker's pager
    if (pagerShown && ty >= m.bodyBottom - m.pagerH && ty < m.bodyBottom) {
        const int16_t cell = width() / 3;
        if (tx < cell)
            turnPage(-1);
        else if (tx >= width() - cell)
            turnPage(1);
        return true;
    }

    // An option. Applying it, or opening the confirmation it needs, is the menu's decision
    for (size_t i = 0; i < optionsShown.size(); i++) {
        if (ty < optionTops[i] || ty >= optionTops[i] + m.rowH)
            continue;
        if (tx < optionLefts[i] || tx >= optionLefts[i] + (int16_t)optionWidths[i])
            continue;
        const T5Option &o = options[optionsShown[i]];
        if (!o.selectable)
            return true; // Shown, but never set
        if (o.selected) {
            leave(); // Tapping the current value returns silently, with nothing written
            return true;
        }
        const T5Act which = pick;
        const int32_t value = o.value;
        mode = Mode::CLOSED;
        reconcile();
        T5MenuApplet::pickChosen(which, value);
        return true;
    }

    return true; // A task consumes every tap inside it
}

#endif

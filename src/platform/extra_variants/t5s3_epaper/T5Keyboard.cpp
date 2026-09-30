#if defined(T5_S3_EPAPER_PRO) && defined(MESHTASTIC_INCLUDE_INKHUD)

#include "./T5Keyboard.h"

#include <cmath>

using namespace NicheGraphics;

namespace
{
// The frozen layouts. Uppercase legends throughout; the shift state changes
// nothing but what a letter key produces (Messaging Final 06).
const char *const LETTERS[3] = {"QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"};
const char *const SYMBOLS[3] = {"1234567890", "-/:;()@&\"", "!'#+=*%"};

// The degree sign takes the SHIFT position in the symbols layer, in both modes
constexpr char DEGREE = '\xB0'; // Win-1253

// --- Carry, from the frozen key table
// -------------------------------------------------------------
constexpr int16_t CARRY_ROW_Y[5] = {573, 662, 751, 840, 929};
const int16_t CARRY_R1[11] = {0, 51, 102, 152, 203, 254, 305, 356, 406, 457, 508};
const int16_t CARRY_R2[10] = {25, 76, 127, 178, 229, 279, 330, 381, 432, 483};
const int16_t CARRY_R3[8] = {76, 127, 178, 229, 280, 330, 381, 432}; // Between SHIFT and DEL
constexpr int16_t CARRY_SHIFT_RIGHT = 76, CARRY_DEL_LEFT = 432;
constexpr int16_t CARRY_R4[6] = {0, 102, 152, 406, 457, 508}; // 123 , SPACE . ?

// --- Console
// ---------------------------------------------------------------------------------------
constexpr int16_t CONSOLE_ROW_Y[5] = {167, 253, 338, 424, 509};
constexpr int16_t CONSOLE_KEYS_RIGHT = 778;
constexpr int16_t CONSOLE_SHIFT_RIGHT = 156, CONSOLE_R3_END_LEFT = 700;
constexpr int16_t CONSOLE_R4[6] = {0, 156, 234, 622, 700, 778};
constexpr int16_t CONSOLE_RAIL_SPLIT = 338; // DEL above, SEND below

int16_t consoleR1(uint8_t i)
{
    return (int16_t)lround(i * 77.8);
}
int16_t consoleR2(uint8_t i)
{
    return 39 + (int16_t)lround(i * 77.8);
}
int16_t consoleR3(uint8_t i)
{
    return CONSOLE_SHIFT_RIGHT + (int16_t)lround(i * 77.7);
}

// Which row a y falls in, or -1
int8_t rowAt(bool console, int16_t y)
{
    const int16_t *rows = console ? CONSOLE_ROW_Y : CARRY_ROW_Y;
    for (int8_t r = 0; r < 4; r++) {
        if (y >= rows[r] && y < rows[r + 1])
            return r;
    }
    return -1;
}

// The character a letter key produces, given the shift state
char shifted(char c, InkHUD::T5Keyboard::Shift shift)
{
    if (c < 'A' || c > 'Z')
        return c; // Symbols and digits are unaffected
    return shift == InkHUD::T5Keyboard::Shift::OFF ? (char)(c - 'A' + 'a') : c;
}

// Index of the interval containing x, in a boundary table, or -1
int8_t slot(const int16_t *bounds, uint8_t count, int16_t x)
{
    for (uint8_t i = 0; i + 1 < count; i++) {
        if (x >= bounds[i] && x < bounds[i + 1])
            return i;
    }
    return -1;
}
} // namespace

InkHUD::T5Keyboard::Key InkHUD::T5Keyboard::hit(bool console, Layer layer, Shift shift, int16_t x, int16_t y)
{
    Key key;
    const char *const *rows = layer == Layer::LETTERS ? LETTERS : SYMBOLS;

    if (console && x >= CONSOLE_RAIL_LEFT) {
        if (y < CONSOLE_ROW_Y[0])
            return key; // The rail top holds the counter and the NEW tag: neither is
                        // a target
        key.kind = y < CONSOLE_RAIL_SPLIT ? Key::DEL : Key::SEND;
        return key;
    }

    const int8_t row = rowAt(console, y);
    if (row < 0)
        return key;

    // Row 4 is the same in both layers: the layer switch, comma, space, full
    // stop, question mark
    if (row == 3) {
        const int16_t *bounds = console ? CONSOLE_R4 : CARRY_R4;
        const int8_t i = slot(bounds, 6, x);
        if (i < 0)
            return key;
        if (i == 0) {
            key.kind = Key::LAYER;
            return key;
        }
        key.kind = Key::CHAR;
        key.ch = i == 1 ? ',' : i == 2 ? ' ' : i == 3 ? '.' : '?';
        return key;
    }

    if (row == 0) {
        if (console) {
            for (uint8_t i = 0; i < 10; i++) {
                if (x >= consoleR1(i) && x < consoleR1(i + 1)) {
                    key.kind = Key::CHAR;
                    key.ch = shifted(rows[0][i], shift);
                    return key;
                }
            }
            return key;
        }
        const int8_t i = slot(CARRY_R1, 11, x);
        if (i >= 0) {
            key.kind = Key::CHAR;
            key.ch = shifted(rows[0][i], shift);
        }
        return key;
    }

    if (row == 1) {
        // The inset half-keys extend the outer two keys' bounds to the panel edge
        const uint8_t count = 9;
        for (uint8_t i = 0; i < count; i++) {
            const int16_t from = console ? consoleR2(i) : CARRY_R2[i];
            const int16_t to = console ? consoleR2(i + 1) : CARRY_R2[i + 1];
            const int16_t left = i == 0 ? 0 : from;
            const int16_t right = i == count - 1 ? (console ? CONSOLE_KEYS_RIGHT : 508) : to;
            if (x >= left && x < right) {
                key.kind = Key::CHAR;
                key.ch = shifted(rows[1][i], shift);
                return key;
            }
        }
        return key;
    }

    // Row 3: SHIFT, seven keys, then DEL (Carry) or the apostrophe / underscore
    // key (Console)
    const int16_t shiftRight = console ? CONSOLE_SHIFT_RIGHT : CARRY_SHIFT_RIGHT;
    if (x < shiftRight) {
        // The symbols layer puts the degree sign where SHIFT lives; there is
        // nothing to shift there
        if (layer == Layer::SYMBOLS) {
            key.kind = Key::CHAR;
            key.ch = DEGREE;
        } else
            key.kind = Key::SHIFT;
        return key;
    }
    if (!console && x >= CARRY_DEL_LEFT) {
        key.kind = Key::DEL;
        return key;
    }
    if (console && x >= CONSOLE_R3_END_LEFT) {
        key.kind = Key::CHAR;
        key.ch = layer == Layer::LETTERS ? '\'' : '_';
        return key;
    }
    for (uint8_t i = 0; i < 7; i++) {
        const int16_t from = console ? consoleR3(i) : CARRY_R3[i];
        const int16_t to = console ? consoleR3(i + 1) : CARRY_R3[i + 1];
        if (x >= from && x < to) {
            key.kind = Key::CHAR;
            key.ch = shifted(rows[2][i], shift);
            return key;
        }
    }
    return key;
}

const int16_t *InkHUD::T5Keyboard::rowTops(bool console)
{
    return console ? CONSOLE_ROW_Y : CARRY_ROW_Y;
}

// Every key of this layer, in drawing order, with the legend the frozen layout
// gives it
std::vector<InkHUD::T5Keyboard::Cell> InkHUD::T5Keyboard::cells(bool console, Layer layer, Shift shift)
{
    const char *const *rows = layer == Layer::LETTERS ? LETTERS : SYMBOLS;
    const int16_t *rowY = console ? CONSOLE_ROW_Y : CARRY_ROW_Y;
    const int16_t keysRight = console ? CONSOLE_KEYS_RIGHT : 508;
    const int16_t bottom = console ? 508 : 928;
    std::vector<Cell> out;

    auto add = [&out](int16_t left, int16_t right, int16_t top, int16_t bot, const std::string &label, bool small, bool boundary,
                      const std::string &sub = "") {
        Cell c;
        c.left = left;
        c.right = right;
        c.top = top;
        c.bottom = bot;
        c.label = label;
        c.sub = sub;
        c.small = small;
        c.boundaryLeft = boundary;
        out.push_back(c);
    };

    for (uint8_t i = 0; i < 10; i++)
        add(console ? consoleR1(i) : CARRY_R1[i], console ? consoleR1(i + 1) : CARRY_R1[i + 1], rowY[0], rowY[1],
            std::string(1, rows[0][i]), false, i > 0);

    for (uint8_t i = 0; i < 9; i++)
        add(console ? consoleR2(i) : CARRY_R2[i], console ? consoleR2(i + 1) : CARRY_R2[i + 1], rowY[1], rowY[2],
            std::string(1, rows[1][i]), false, i > 0);

    // Row 3: SHIFT (the degree sign in the symbols layer), seven keys, then DEL
    // or the end key
    const int16_t shiftRight = console ? CONSOLE_SHIFT_RIGHT : CARRY_SHIFT_RIGHT;
    if (layer == Layer::SYMBOLS)
        add(0, shiftRight, rowY[2], rowY[3], std::string(1, DEGREE), false, false);
    else
        add(0, shiftRight, rowY[2], rowY[3], shift == Shift::CAPS ? "CAPS" : "SHIFT", true, false,
            shift == Shift::CAPS   ? "LOCKED"
            : shift == Shift::ONCE ? "NEXT LETTER"
                                   : "OFF");

    for (uint8_t i = 0; i < 7; i++)
        add(console ? consoleR3(i) : CARRY_R3[i], console ? consoleR3(i + 1) : CARRY_R3[i + 1], rowY[2], rowY[3],
            std::string(1, rows[2][i]), false, true);

    const int16_t endLeft = console ? CONSOLE_R3_END_LEFT : CARRY_DEL_LEFT;
    if (console)
        add(endLeft, keysRight, rowY[2], rowY[3], layer == Layer::LETTERS ? "\'" : "_", false, true);
    else
        add(endLeft, keysRight, rowY[2], rowY[3], "DEL", true, true);

    const int16_t *r4 = console ? CONSOLE_R4 : CARRY_R4;
    static const char *const R4_LABELS[5] = {nullptr, ",", "SPACE", ".", "?"};
    for (uint8_t i = 0; i < 5; i++)
        add(r4[i], r4[i + 1], rowY[3], bottom, i == 0 ? (layer == Layer::LETTERS ? "123" : "ABC") : R4_LABELS[i],
            i == 0 || i == 2, i > 0);

    return out;
}

#endif

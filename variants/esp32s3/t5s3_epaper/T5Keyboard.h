#ifdef MESHTASTIC_INCLUDE_INKHUD

/*

The frozen T5 keyboard layout (Messaging Final 06).

Geometry and hit-testing only: it answers "which key is at this point" and "what
cells make up this layer", and the composer draws them. Keys tile edge to edge
and the 1 px black lines sit on the key boundaries, so there are no dead gaps
and every pixel of the keyboard belongs to exactly one key. Inset half-keys on
the second row extend the touch bounds of A and L to the panel edge. Legends are
always uppercase; the shift state changes nothing but what a letter key
produces.

*/

#pragma once

#include "configuration.h"

#include <string>
#include <vector>

namespace NicheGraphics::InkHUD
{

class T5Keyboard
{
  public:
    enum class Layer : uint8_t { LETTERS, SYMBOLS };
    enum class Shift : uint8_t { OFF, ONCE, CAPS }; // A tap arms one letter; a double tap locks CAPS

    // What a point on the keyboard means. CHAR carries the character the frozen
    // layout puts there.
    struct Key {
        enum Kind : uint8_t { NONE, CHAR, DEL, SHIFT, LAYER, SEND } kind = NONE;
        char ch = 0;
    };

    // One drawn key: its rect, its legend, and whether the legend needs the
    // smaller font because it is a word rather than a character. A second line is
    // used only by SHIFT, which states what it will do.
    struct Cell {
        int16_t left = 0, right = 0, top = 0, bottom = 0;
        std::string label;
        bool small = false;
        bool inverted = false;     // SHIFT once and CAPS: the key's state is its only visual difference
        bool boundaryLeft = false; // Draw the 1 px rule on this key's left edge
    };

    static Key hit(bool console, Layer layer, Shift shift, int16_t x, int16_t y);
    static std::vector<Cell> cells(bool console, Layer layer, Shift shift);

    // The five frozen row boundaries, top of row 1 to the bottom edge. Rows are
    // not a uniform height, so the rules between them are drawn from this table
    // rather than from a step.
    static const int16_t *rowTops(bool console);

    // Where the keyboard starts, so the composer knows how much room its field
    // has, and where the Console rail and its two buttons sit
    static constexpr int16_t CARRY_TOP = 571;   // 2 px rule at 571, rows from 573
    static constexpr int16_t CONSOLE_TOP = 165; // 2 px rule at 165, rows from 167
    static constexpr int16_t CARRY_ROWS_TOP = 573;
    static constexpr int16_t CONSOLE_ROWS_TOP = 167;
    static constexpr int16_t CONSOLE_RAIL_LEFT = 778;
    static constexpr int16_t CONSOLE_RAIL_SPLIT = 338; // DEL above, SEND below
    static constexpr int16_t CONSOLE_KEYS_RIGHT = 778;
};

} // namespace NicheGraphics::InkHUD

#endif

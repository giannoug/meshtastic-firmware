#ifdef MESHTASTIC_INCLUDE_INKHUD

/*

The frozen Menu / Settings model: what each page holds, what each row currently reads, whether it can
be used at all, and what writing it does (Menu Settings 05, 06, 07, 08, 11).

One table drives both layouts. T5MenuApplet lays rows out and hit-tests them; nothing here knows a
pixel. The writes live here too, next to the row that offers them, so there is exactly one path to
each config field - the lifted apply* helpers from the shared menu, which are file-static there and
so cannot simply be called (Menu Settings 00, last row).

*/

#pragma once

#include "configuration.h"

#include <string>
#include <vector>

namespace NicheGraphics::InkHUD
{

// The eight categories, their two sub-pages, and the root. Carry gives each one a page; Console
// gives the eight a tile each and the current one the pane (Menu Settings 01, 13).
enum class T5Page : uint8_t {
    ROOT,
    RADIO,
    CHANNELS,
    CHANNEL, // Per-channel detail, C7
    POSITION,
    CONNECTIVITY,
    SCREEN, // The DISPLAY page. Arduino.h defines DISPLAY as a macro, so the enumerator differs from
            // the label; nothing the user sees changes.
    POWER,
    DEVICE,
    ABOUT,
    DEVTOOLS, // T5_DEVTOOLS builds only, C15
};
constexpr uint8_t T5_CATEGORY_COUNT = 8; // RADIO..ABOUT, the tiles in Console

// The six frozen row types plus the banner, and the two read-only arrangements the wider pane uses
// (Menu Settings 03, C0)
enum class T5RowKind : uint8_t {
    SECTION,  // Small caps label with a rule under it. Not a target
    CATEGORY, // Root only: name plus a live summary line
    NAV,      // Label, bold value, chevron. Opens a page or a picker
    STATE,    // Label plus a filled ON / outlined OFF chip
    CHOICE,   // Two or three joined cells, the selected one inverted
    ACTION,   // Bordered box. A trailing ellipsis means a confirmation follows
    INFO,     // Label and value. Not a target
    NOTE,     // Wrapped sentence, usually "this lives in the phone app"
    BANNER,   // Inverted block: the one thing wrong with the device right now
    FACTS,    // Console: a two-column grid of read-only pairs
};

// What a tap on a row does. One entry per frozen control, so a row is data and the switch that
// applies it is the only code that knows the difference.
enum class T5Act : uint8_t {
    NONE,

    // Navigation
    OPEN_RADIO,
    OPEN_CHANNELS,
    OPEN_POSITION,
    OPEN_CONNECTIVITY,
    OPEN_DISPLAY,
    OPEN_POWER,
    OPEN_DEVICE,
    OPEN_ABOUT,
    OPEN_DEVTOOLS,
    OPEN_CHANNEL, // arg = channel index

    // Pickers (task layer)
    PICK_REGION,
    PICK_PRESET,
    PICK_HOP,
    PICK_ROLE,
    PICK_SLEEP,
    PICK_BROADCAST,
    PICK_PRECISION,
    PICK_TZ,
    PICK_PIN_MODE, // RANDOM or FIXED: a value row with a picker, as the label beside two cells does not fit (D12)

    // Keyboard fields
    EDIT_LONG_NAME,
    EDIT_SHORT_NAME,

    // States: value is the new state, 1 or 0
    SET_TRANSMIT,
    SET_GPS,
    SET_FIXED_POSITION,
    SET_SMART_POSITION,
    SET_SHARE_POSITION,
    SET_UPLINK,
    SET_DOWNLINK,
    SET_BLUETOOTH,
    SET_WIFI,
    SET_POWER_SAVING,
    SET_FRONT_LIGHT,
    SET_TOUCH,

    // Choices: value is the chosen cell
    SET_MODE,
    SET_UNITS,
    SET_CLOCK,
    SET_PIN_MODE,

    // Actions
    CLEAN_SCREEN,
    REBOOT,
    SHUT_DOWN,
    CLEAR_NODES, // value: 1 keep favourites, 0 everything
    CLEAR_MESSAGES,
    DEV_SCREENSHOT,
    DEV_TOUCH_CAL,
};

// One row of a page, built fresh on each render so a value can never be stale (Menu Settings 23)
struct T5Row {
    T5RowKind kind = T5RowKind::INFO;
    std::string label;
    std::string value;
    bool smallValue = false;   // The value in small bold at baseline top+42: the C6 channel role (Panel Fit D12)
    std::string summary;       // CATEGORY: the live second line
    std::string hint;          // Carry only; Console moves hints into the confirmation
    std::string reason;        // Why this row cannot be used, inside the dotted box
    const char *tag = nullptr; // "RESTARTS", on every setting that reboots. Not drawn (Panel Fit D2)
    bool available = true;
    bool on = false;                  // STATE
    std::vector<std::string> options; // CHOICE cells
    uint8_t selected = 0;             // CHOICE
    T5Act action = T5Act::NONE;
    int32_t arg = 0;                                        // What SET_* writes, or the channel OPEN_CHANNEL opens
    std::vector<std::pair<std::string, std::string>> facts; // FACTS
};

// One option of a picker. `selectable` is false for the row a picker shows but will not set -
// NOT SET in the region list (Menu Settings C3)
struct T5Option {
    std::string label;
    std::string detail;
    int32_t value = 0;
    bool selected = false;
    bool selectable = true;
    bool detailRight = false; // The detail right-aligned on the label's line, not under it (region ranges, D18)
};

// A confirmation, as the frozen template lays it out: question, optional NOW -> NEW block, two to
// four consequences, an optional KEPT line, and a verb that names the result (Menu Settings 07)
struct T5Confirm {
    std::string title;                // "CHANGE REGION?"
    std::string crumb;                // "RADIO > REGION"
    std::string from;                 // NOW, empty when nothing changes value
    std::string to;                   // NEW
    std::string fromDetail, toDetail; // A third line under each, small regular: the region ranges (D17)
    std::vector<std::string> bullets;
    std::string keptLabel; // "KEPT" section heading, empty when there is none
    std::vector<std::string> kept;
    std::string verb;      // The inverted right-hand button
    bool restarts = false; // The RESTARTING screen follows, and input stops
    // A confirmation may carry one choice row, which is how CLEAR NODE LIST offers KEEP FAVOURITES
    std::vector<std::string> choices;
    uint8_t choiceDefault = 0;
};

// What applying a row produced. BUSY means the work is visible and the task shows the frozen
// applying band until the reboot it scheduled arrives.
enum class T5ApplyResult : uint8_t { DONE, RESTARTING, FAILED };

// ---- the model ----------------------------------------------------------------------------

// `console` picks between the two arrangements of the same values: only the two read-only pages
// differ, where the wider pane gets the frozen facts grid instead of sections (K3, K4)
std::vector<T5Row> t5BuildPage(T5Page page, uint8_t channelIndex, bool console);
std::string t5PageTitle(T5Page page, uint8_t channelIndex);
std::string t5PageContext(T5Page page, uint8_t channelIndex); // The header's second line
const char *t5PageBack(T5Page page);                          // Label after the chevron, or nullptr at the root
T5Page t5PageParent(T5Page page);
T5Page t5CategoryPage(uint8_t index); // 0..7 in frozen order, for the Console tiles
const char *t5CategoryName(uint8_t index);
std::string t5CategorySummary(uint8_t index, bool shortForm); // Tile summaries are the short form
bool t5PageExists(T5Page page);                               // DEVTOOLS only in developer builds

// ---- pickers -----------------------------------------------------------------------------

std::vector<T5Option> t5PickOptions(T5Act which, uint8_t channelIndex);
std::string t5PickTitle(T5Act which);
std::string t5PickContext(T5Act which);

// ---- confirmations -----------------------------------------------------------------------

// True when this action must be confirmed, filling `out`. Everything else applies on tap and is
// reversed with one more tap, which is what keeps confirmations rare enough to be read.
bool t5NeedsConfirm(T5Act which, int32_t value, uint8_t channelIndex, T5Confirm &out);

// ---- applying ----------------------------------------------------------------------------

T5ApplyResult t5Apply(T5Act which, int32_t value, uint8_t channelIndex);

// The owner names, for the keyboard field. Writing happens on DONE.
std::string t5OwnerName(bool shortName);
size_t t5OwnerNameLimit(bool shortName);
T5ApplyResult t5SetOwnerName(bool shortName, const std::string &utf8);

} // namespace NicheGraphics::InkHUD

#endif

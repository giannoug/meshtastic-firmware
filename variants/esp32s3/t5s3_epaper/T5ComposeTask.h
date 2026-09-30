#ifdef MESHTASTIC_INCLUDE_INKHUD

/*

The T5 messaging task layer: pick a recipient, then write to them (Messaging
Final 04, 05, 06, 07).

A task, not a screen. It replaces the normal navigation instead of sitting
inside it - there is no nav bar - and it owns input while it is up, so a Home
press peels it rather than going Home. That is why it is a system applet:
free-text and task ownership both belong to this side of InkHUD, and the frozen
class handoff put the composer here deliberately.

Three layers, from the outside in:

  PICKER     NEW MESSAGE: the channels, four recent nodes, and ALL NODES
  NODE_LIST  the frozen Nodes list in pick mode. We draw nothing and let
T5NodesApplet show, but we keep input, so Home still closes the whole task in
one press COMPOSER   the destination, the draft, the byte counter, SEND and
CANCEL, over the keyboard

*/

#pragma once

#include "configuration.h"

#include "./T5Keyboard.h"
#include "./T5MessageModel.h"
#include "./T5Text.h"

#include "graphics/niche/InkHUD/SystemApplet.h"

namespace NicheGraphics::InkHUD
{

class T5ComposeTask : public SystemApplet, public T5Redrawable
{
  public:
    using Convo = T5MessageModel::Convo;

    T5ComposeTask();

    static void begin();
    static T5ComposeTask *get(); // Null before begin()

    static bool available();    // A channel exists to write to, so COMPOSE can open the picker
    static bool taskOpen();     // Any of our three layers is up
    static bool composerOpen(); // The composer specifically: no banner, and the
                                // frozen "n NEW" tag

    // Entry points A-E of the frozen table
    static void openPicker();              // COMPOSE, or NEW DIRECT MESSAGE in Console
    static void openReply(const Convo &c); // A / B, from a conversation's REPLY bar
    static void openForNode(NodeNum num);  // C, from Node detail's COMPOSE MESSAGE

    void onRender(bool full) override;
    bool onTouchPoint(uint16_t x, uint16_t y, bool longPress) override;
    void onExitShort() override; // The T5 Home action peels one layer
    // Swipes reach the first system applet holding input, which is us whenever
    // any layer is open. The picker pages its own list; in Nodes pick mode the
    // list underneath is the one that should move.
    void onNavUp() override;
    void onNavDown() override;
    void onMessagingChanged() override;
    void onModeChanged(); // Our tile spans the screen, so its region follows the
                          // new orientation

    // Nodes pick mode asks whether it is standing in for the picker, and hands
    // back the row that was tapped
    static bool pickingNode();
    static void nodePicked(NodeNum num);
    static void backToPicker();

  private:
    enum class Mode : uint8_t { CLOSED, PICKER, NODE_LIST, COMPOSER };

    // Where CANCEL and the first Home press land, from the frozen entry table
    enum class Origin : uint8_t { MESSAGES, CONVERSATION, NODE_DETAIL };

    Mode mode = Mode::CLOSED;
    Origin origin = Origin::MESSAGES;
    Convo destination;      // The composer's target
    NodeNum nodeOrigin = 0; // The node whose detail screen opened us, for CANCEL

    std::string typed; // The draft, in the display encoding the keyboard produces
    T5Keyboard::Layer layer = T5Keyboard::Layer::LETTERS;
    T5Keyboard::Shift shift = T5Keyboard::Shift::OFF;
    uint32_t lastShiftTapMs = 0;

    // One line of the picker: a section label or a destination. Paged like the
    // Messages list, because a device with eight channels overflows a Carry page
    // (Messaging Final 05).
    struct PickItem {
        bool label = false;
        std::string text; // LABEL only
        Convo convo;
    };
    std::vector<PickItem> pickItems();
    void pagePicker(int8_t delta);

    uint16_t firstItem = 0;          // The picker's first item on screen
    std::vector<uint16_t> pickPages; // First item of each page, recomputed on every render
    std::vector<Convo> pickRows;     // Destination rows on screen, top to bottom
    std::vector<int16_t> pickRowTops;
    bool pickPagerShown = false;
    bool allNodesShown = false;
    int16_t allNodesTop = 0;

    void openComposer(const Convo &c, Origin from);
    void close(bool keepDraft);
    void leaveTo(Origin target);
    void reconcile(); // Foreground, input claim and the tile, from the current mode

    void renderPicker();
    void renderComposer();
    void drawKeyboard();
    void drawField(int16_t left, int16_t top, uint16_t w, uint8_t lines, AppletFont font);
    void drawCancel();
    void drawNewTag(int16_t left, int16_t top);
    void drawSend(bool enabled);
    void drawCounter();
    void drawPickRow(int16_t left, int16_t top, uint16_t w, const Convo &c, bool console);
    std::string pickMeta(const Convo &c);
    std::string contextLine(std::string *second);

    void type(char c);
    void backspace(bool word);
    void toggleShift();
    void send();
    uint16_t bytesLeft() const;
    void armShift(); // The frozen auto-arm: at the start of a message, and after
                     // ". ", "? ", "! "

    std::vector<NodeNum> recentNodes(); // Favourites, then DM partners, de-duplicated, capped at four

    void printBold(int16_t x, int16_t y, const std::string &text, HorizontalAlignment ha = LEFT);
    void boxed(int16_t left, int16_t top, uint16_t w, uint16_t h, const std::string &label, bool filled, bool struck);

    static constexpr uint32_t SHIFT_DOUBLE_TAP_MS = 400; // Frozen
    static constexpr uint16_t COUNTER_FROM = 40;         // The counter appears at 40 bytes left
    static constexpr uint8_t RECENT_CAP = 4;
};

} // namespace NicheGraphics::InkHUD

#endif

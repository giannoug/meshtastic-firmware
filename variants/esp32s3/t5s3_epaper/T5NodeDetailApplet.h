#ifdef MESHTASTIC_INCLUDE_INKHUD

/*

T5 Node detail: one node's facts, its position relative to ours, and COMPOSE MESSAGE / SHOW ON MAP, above the T5 nav.
Opened from a T5 Nodes row, or from the map's DETAIL. One applet, two compositions picked from the live dimensions: Carry and
Console.

*/

#pragma once

#include "configuration.h"

#include "./T5Applet.h"

#include "mesh/MeshModule.h"

namespace NicheGraphics::InkHUD
{

class T5NodeDetailApplet : public T5Applet, public MeshModule
{
  public:
    T5NodeDetailApplet() : MeshModule("T5NodeDetailApplet") { hideFromAppSwitcher = true; } // Drill-down from Nodes

    // Show a nodeDB node. The origin is where Back returns: NODES for a Nodes row, MAP for the map's
    // DETAIL, which comes back with its selection, centre and zoom intact. False, and nothing shown, if
    // that isn't possible.
    static bool open(NodeNum num, Destination from = NODES);

    void onRender(bool full) override;
    bool onTouchPoint(uint16_t x, uint16_t y, bool longPress) override;

    bool wantPacket(const meshtastic_MeshPacket *p) override;
    ProcessMessage handleReceived(const meshtastic_MeshPacket &mp) override;

  private:
    // Node detail is a deeper screen, not a destination: the bar keeps showing where it was opened from,
    // and Back returns there. MAP is the second origin, and it returns with its view preserved (Map 07, D3).
    Destination currentDestination() override { return origin; }

    NodeNum node = 0;           // Shown node, re-read from nodeDB on every render
    Destination origin = NODES; // Where Back goes
};

} // namespace NicheGraphics::InkHUD

#endif

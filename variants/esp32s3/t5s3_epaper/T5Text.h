#ifdef MESHTASTIC_INCLUDE_INKHUD

/*

The text every T5 messaging surface shares: conversation labels, node names,
message times, wrapping and the frozen trailing ellipsis (Messaging Final 02,
03).

Free functions rather than T5Applet members because the composer and the picker
are system applets, and a system applet cannot inherit T5Applet without
inheriting Applet twice. Everything here needs only the public drawing API, plus
the Win-1253 re-encoding that all three T5 fonts share.

*/

#pragma once

#include "configuration.h"

#include "./T5MessageModel.h"

#include "graphics/niche/InkHUD/Applet.h"

namespace NicheGraphics::InkHUD
{

// UTF-8 to the extended-ASCII encoding the fonts use. Font independent: the
// three T5 fonts are all Win-1253, so the mapping does not depend on which one
// is set.
std::string t5Parse(const std::string &text);

std::string t5NodeId(NodeNum num);    // !0123abcd
std::string t5NodeTag(NodeNum num);   // Short name, or the last four hex digits
                                      // when there is no node info
std::string t5NodeTitle(NodeNum num); // Long name, or !id

std::string t5ConvoTag(const T5MessageModel::Convo &c);     // "CH 0", or the peer's tag
std::string t5ConvoTitle(const T5MessageModel::Convo &c);   // Channel name or modem preset, or the peer's name
std::string t5ConvoPreview(const T5MessageModel::Convo &c); // "SENDER: text", "You: text", or "No messages yet"

// Today HH:MM, YDAY, MON-SUN for the last six days, D MMM beyond, and nothing
// at all without a clock. Never relative: a list of absolute times does not
// have to be redrawn as it ages.
std::string t5ListTime(uint32_t timestamp, bool bootRelative);

// Cut to fit, with the frozen trailing ellipsis, using the font the applet
// currently has set
std::string t5Elide(Applet &applet, const std::string &text, uint16_t width);

// Word wrap at a pixel width. A single word longer than one line breaks at the
// edge (Messaging Final 03).
std::vector<std::string> t5WrapLines(Applet &applet, const std::string &text, uint16_t width);

// Pixel width of a string in the font the applet currently has set, through the
// public API
uint16_t t5TextWidth(Applet &applet, const std::string &text);

// First item of each page, filling greedily from the top. An item taller than a page still gets a page of
// its own, so no row can be unreachable. Shared by the Messages list and the recipient picker.
std::vector<uint16_t> t5Paginate(const std::vector<uint16_t> &heights, int16_t room);

// The keyboard types in the display encoding; the wire wants UTF-8. Only the
// degree sign differs, so the conversion is one substitution, and the frozen
// byte limit counts the UTF-8 result (Messaging Final 04).
std::string t5ToUtf8(const std::string &typed);
size_t t5Utf8Size(const std::string &typed);

} // namespace NicheGraphics::InkHUD

#endif

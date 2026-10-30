#if defined(T5_S3_EPAPER_PRO) && defined(MESHTASTIC_INCLUDE_INKHUD)

#include "./T5Text.h"

#include "gps/RTC.h"
#include "mesh/Channels.h"
#include "mesh/NodeDB.h"

#include <algorithm>
#include <cctype>
#include <ctime>

using namespace NicheGraphics;

namespace
{
const char *const WEEKDAYS[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
const char *const MONTHS[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
constexpr const char *ELLIPSIS = "\x85"; // Win-1253 horizontal ellipsis: present in all three T5 fonts

// parse() marks anything the font cannot draw with a SUB. A name made only of
// emoji has nothing left once they are removed, which is the signal to fall
// back to the node id.
std::string strip(std::string text)
{
    text.erase(std::remove(text.begin(), text.end(), '\x1A'), text.end());
    return text.substr(0, text.find_last_not_of(' ') + 1);
}
} // namespace

std::string InkHUD::t5Parse(const std::string &text)
{
    return Applet::fontSmall.decodeUTF8(text);
}

std::string InkHUD::t5NodeId(NodeNum num)
{
    char id[12];
    snprintf(id, sizeof(id), "!%08x", (unsigned)num);
    return id;
}

std::string InkHUD::t5NodeTag(NodeNum num)
{
    const meshtastic_NodeInfoLite *node = nodeDB->getMeshNode(num);
    if (nodeInfoLiteHasUser(node)) {
        const std::string parsed = strip(t5Parse(node->short_name));
        if (!parsed.empty())
            return parsed;
    }
    // A peer with no node info, or one whose short name is all emoji, is known by
    // its last four hex digits
    return t5NodeId(num).substr(5);
}

std::string InkHUD::t5NodeTitle(NodeNum num)
{
    const meshtastic_NodeInfoLite *node = nodeDB->getMeshNode(num);
    if (!nodeInfoLiteHasUser(node))
        return t5NodeId(num);
    const std::string name = strip(t5Parse(node->long_name));
    return name.empty() ? t5NodeId(num) : name;
}

std::string InkHUD::t5ConvoTag(const T5MessageModel::Convo &c)
{
    return c.isChannel() ? "CH " + std::to_string(c.channel) : t5NodeTag(c.peer);
}

// An empty primary name shows the modem preset the mesh is actually running -
// LongFast on a default device, never the stock "Public" sample - and an empty
// secondary is known only by its index.
std::string InkHUD::t5ConvoTitle(const T5MessageModel::Convo &c)
{
    if (!c.isChannel())
        return t5NodeTitle(c.peer);
    const meshtastic_Channel &ch = channels.getByIndex(c.channel);
    if (!ch.settings.name[0] && c.channel != channels.getPrimaryIndex())
        return "Channel " + std::to_string(c.channel);
    return t5Parse(channels.getName(c.channel));
}

std::string InkHUD::t5ConvoPreview(const T5MessageModel::Convo &c)
{
    T5MessageModel *model = T5MessageModel::get();
    const StoredMessage *m = model ? model->newest(c) : nullptr;
    if (!m)
        return "No messages yet";

    const std::string text = t5Parse(MessageStore::getText(*m));
    if (m->sender == nodeDB->getNodeNum())
        return "You: " + text;
    // A DM already names its peer in the tag and title, so only a channel row
    // needs the sender
    return c.isChannel() ? t5NodeTag(m->sender) + ": " + text : text;
}

std::string InkHUD::t5ListTime(uint32_t timestamp, bool bootRelative)
{
    if (!timestamp || bootRelative)
        return ""; // No wall clock stood behind this message, so the frozen rule
                   // shows no time at all

    const uint32_t now = getValidTime(RTCQuality::RTCQualityDevice, true);
    if (!now || timestamp > now + SEC_PER_DAY)
        return "";

    const uint32_t nowDay = now / SEC_PER_DAY, day = timestamp / SEC_PER_DAY;
    if (day == nowDay) {
        const uint32_t hms = timestamp % SEC_PER_DAY;
        const uint32_t hour = hms / SEC_PER_HOUR, minute = (hms % SEC_PER_HOUR) / SEC_PER_MIN;
        char clock[12];
        if (config.display.use_12h_clock)
            snprintf(clock, sizeof(clock), "%u:%02u %s", (hour % 12 == 0) ? 12u : (unsigned)(hour % 12), (unsigned)minute,
                     hour > 11 ? "PM" : "AM");
        else
            snprintf(clock, sizeof(clock), "%02u:%02u", (unsigned)hour, (unsigned)minute);
        return clock;
    }
    if (day + 1 == nowDay)
        return "YDAY";
    if (day + 6 >= nowDay)
        return WEEKDAYS[(day + 4) % 7]; // 1970-01-01 was a Thursday

    const time_t secs = timestamp;
    tm parts;
    if (!gmtime_r(&secs, &parts))
        return "";
    return std::to_string(parts.tm_mday) + " " + MONTHS[parts.tm_mon % 12];
}

uint16_t InkHUD::t5TextWidth(NicheGraphics::InkHUD::Applet &applet, const std::string &text)
{
    int16_t x, y;
    uint16_t w, h;
    applet.getTextBounds(text.c_str(), 0, 0, &x, &y, &w, &h);
    return w;
}

std::string InkHUD::t5Elide(NicheGraphics::InkHUD::Applet &applet, const std::string &text, uint16_t width)
{
    if (t5TextWidth(applet, text) <= width)
        return text;
    // Measured with the ellipsis attached: the gap before it is part of the width
    std::string cut = text;
    while (!cut.empty() && t5TextWidth(applet, cut + ELLIPSIS) > width)
        cut.pop_back();
    while (!cut.empty() && cut.back() == ' ')
        cut.pop_back();
    return cut + ELLIPSIS;
}

std::vector<uint16_t> InkHUD::t5Paginate(const std::vector<uint16_t> &heights, int16_t room)
{
    std::vector<uint16_t> pages;
    for (uint16_t i = 0; i < heights.size();) {
        pages.push_back(i);
        int16_t used = 0;
        while (i < heights.size() && used + (int16_t)heights[i] <= room) {
            used += heights[i];
            i++;
        }
        if (pages.back() == i)
            i++; // Taller than a whole page: it gets one to itself rather than being skipped
    }
    if (pages.empty())
        pages.push_back(0);
    return pages;
}

std::string InkHUD::t5ToUtf8(const std::string &typed)
{
    std::string out;
    out.reserve(typed.size());
    for (unsigned char ch : typed) {
        if (ch < 0x80)
            out += (char)ch;
        else if (ch == 0xB0) {
            out += '\xC2'; // U+00B0 DEGREE SIGN
            out += '\xB0';
        }
        // The frozen layouts produce nothing else outside ASCII, so there is no
        // table to keep in step
    }
    return out;
}

size_t InkHUD::t5Utf8Size(const std::string &typed)
{
    size_t bytes = 0;
    for (unsigned char ch : typed)
        bytes += ch < 0x80 ? 1 : ch == 0xB0 ? 2 : 0;
    return bytes;
}

std::vector<std::string> InkHUD::t5WrapLines(NicheGraphics::InkHUD::Applet &applet, const std::string &text, uint16_t width)
{
    std::vector<std::string> lines;
    std::string line;
    size_t pos = 0;
    while (true) {
        const size_t split = text.find_first_of(" \n", pos);
        const bool newline = split != std::string::npos && text[split] == '\n';
        const std::string word = text.substr(pos, split == std::string::npos ? std::string::npos : split - pos);

        if (!word.empty()) {
            const std::string candidate = line.empty() ? word : line + " " + word;
            if (t5TextWidth(applet, candidate) <= width)
                line = candidate;
            else if (!line.empty()) {
                lines.push_back(line);
                line = word;
            }
            if (line == word && t5TextWidth(applet, line) > width) {
                // One unbroken word wider than the column: break it at the edge, as the
                // frozen rule says
                std::string part;
                for (char ch : word) {
                    if (!part.empty() && t5TextWidth(applet, part + ch) > width) {
                        lines.push_back(part);
                        part.clear();
                    }
                    part += ch;
                }
                line = part;
            }
        }

        if (split == std::string::npos)
            break;
        if (newline) {
            lines.push_back(line);
            line.clear();
        }
        pos = split + 1;
    }
    if (!line.empty() || lines.empty())
        lines.push_back(line);
    return lines;
}

#endif

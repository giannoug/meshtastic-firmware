// Records every text run and rectangle the InkHUD applets draw, in logical screen coordinates, by
// linker-wrapping the GFX_Root primitives (see -Wl,--wrap in variants/native/t5-screens.ini).
// Text is captured per character from GFX::write and joined into runs that continue the same line.
#ifdef T5_SCREENS_HOST
#include "DrawLog.h"

#include "graphics/niche/InkHUD/Applet.h"
#include "graphics/niche/InkHUD/Tile.h"

#include <cstdio>

using NicheGraphics::InkHUD::Applet;

static std::vector<DrawRec> records;
static bool recording = false;
static const GFX *runOwner = nullptr;
static int16_t runCursorY = 0, runCursorEndX = 0;

extern "C" {
size_t __real__ZN3GFX5writeEh(GFX *self, uint8_t c);
void __real__ZN3GFX8drawRectEsssst(GFX *self, int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
void __real__ZN3GFX8fillRectEsssst(GFX *self, int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
void __real__ZN3GFX8drawLineEsssst(GFX *self, int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color);
}

// Test-only read of Applet's private crop: explicit instantiation is exempt from access checks
template <typename T> struct CropField {
    using type = T Applet::*;
};
struct CropLeft : CropField<int16_t> {
    friend type get(CropLeft);
};
struct CropTop : CropField<int16_t> {
    friend type get(CropTop);
};
struct CropWidth : CropField<uint16_t> {
    friend type get(CropWidth);
};
struct CropHeight : CropField<uint16_t> {
    friend type get(CropHeight);
};
template <typename Tag, typename Tag::type M> struct Rob {
    friend typename Tag::type get(Tag) { return M; }
};
template struct Rob<CropLeft, &Applet::cropLeft>;
template struct Rob<CropTop, &Applet::cropTop>;
template struct Rob<CropWidth, &Applet::cropWidth>;
template struct Rob<CropHeight, &Applet::cropHeight>;

// Where this applet's pixels can land, in screen coordinates: its crop, inside its tile
static void clipOf(GFX *self, DrawRec &r)
{
    Applet *a = static_cast<Applet *>(self);
    NicheGraphics::InkHUD::Tile *t = a->getTile();
    int16_t dx = t ? t->getLeft() : 0, dy = t ? t->getTop() : 0;
    const int16_t cl = a->*get(CropLeft()), ct = a->*get(CropTop());
    const uint16_t cw = a->*get(CropWidth()), ch = a->*get(CropHeight());
    int16_t l = std::max<int16_t>(cl, 0), tp = std::max<int16_t>(ct, 0);
    int16_t rr = std::min<int16_t>(cl + cw, a->width());
    int16_t bb = std::min<int16_t>(ct + ch, a->height());
    r.clipX = l + dx;
    r.clipY = tp + dy;
    r.clipW = std::max<int16_t>(0, rr - l);
    r.clipH = std::max<int16_t>(0, bb - tp);
}

static void tileOffset(GFX *self, int16_t &dx, int16_t &dy, std::string &owner)
{
    // In this build every GFX that draws is an InkHUD Applet
    Applet *a = static_cast<Applet *>(self);
    NicheGraphics::InkHUD::Tile *t = a->getTile();
    dx = t ? t->getLeft() : 0;
    dy = t ? t->getTop() : 0;
    owner = a->name ? a->name : "?";
}

static void addShape(DrawRec::Kind kind, GFX *self, int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color)
{
    if (!recording)
        return;
    int16_t dx, dy;
    DrawRec r;
    tileOffset(self, dx, dy, r.owner);
    r.kind = kind;
    r.x = x + dx;
    r.y = y + dy;
    r.w = w;
    r.h = h;
    r.color = color;
    clipOf(self, r);
    records.push_back(r);
    runOwner = nullptr; // A shape between two prints ends the text run
}

extern "C" size_t __wrap__ZN3GFX5writeEh(GFX *self, uint8_t c)
{
    if (!recording || c == '\n' || c == '\r')
        return __real__ZN3GFX5writeEh(self, c);

    int16_t cx = self->getCursorX(), cy = self->getCursorY();
    char s[2] = {(char)c, 0};
    int16_t bx, by;
    uint16_t bw, bh;
    self->getTextBounds(s, cx, cy, &bx, &by, &bw, &bh);
    size_t n = __real__ZN3GFX5writeEh(self, c);

    int16_t dx, dy;
    std::string owner;
    tileOffset(self, dx, dy, owner);
    bool extend = runOwner == self && runCursorY == cy && runCursorEndX == cx && !records.empty();
    if (!extend) {
        DrawRec r;
        r.kind = DrawRec::TEXT;
        r.owner = owner;
        r.x = bw ? bx + dx : cx + dx;
        r.y = bw ? by + dy : cy + dy;
        r.w = bw;
        r.h = bh;
        clipOf(self, r);
        records.push_back(r);
    }
    DrawRec &r = records.back();
    r.text.push_back((char)c);
    if (bw && bh) {
        int16_t l = std::min<int16_t>(r.w ? r.x : bx + dx, bx + dx), t = std::min<int16_t>(r.h ? r.y : by + dy, by + dy);
        int16_t rr = std::max<int16_t>(r.w ? r.x + r.w : 0, bx + dx + bw),
                bb = std::max<int16_t>(r.h ? r.y + r.h : 0, by + dy + bh);
        r.x = l;
        r.y = t;
        r.w = rr - l;
        r.h = bb - t;
    }
    runOwner = self;
    runCursorY = cy;
    runCursorEndX = self->getCursorX();
    return n;
}

extern "C" void __wrap__ZN3GFX8drawRectEsssst(GFX *self, int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color)
{
    addShape(DrawRec::RECT, self, x, y, w, h, color);
    __real__ZN3GFX8drawRectEsssst(self, x, y, w, h, color);
}

extern "C" void __wrap__ZN3GFX8fillRectEsssst(GFX *self, int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color)
{
    addShape(DrawRec::FILL, self, x, y, w, h, color);
    __real__ZN3GFX8fillRectEsssst(self, x, y, w, h, color);
}

extern "C" void __wrap__ZN3GFX8drawLineEsssst(GFX *self, int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color)
{
    // Stored as the line's bounding box; x/y is the first endpoint
    addShape(DrawRec::LINE, self, x0, y0, x1 - x0, y1 - y0, color);
    __real__ZN3GFX8drawLineEsssst(self, x0, y0, x1, y1, color);
}

void drawLogStart()
{
    records.clear();
    runOwner = nullptr;
    recording = true;
}

void drawLogStop()
{
    recording = false;
}

const std::vector<DrawRec> &drawLog()
{
    return records;
}

// JSON with text as hex bytes (Win-1253 as drawn); the tooling decodes it
bool drawLogDump(const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f)
        return false;
    static const char *kinds[] = {"text", "rect", "fill", "line"};
    fprintf(f, "[\n");
    for (size_t i = 0; i < records.size(); i++) {
        const DrawRec &r = records[i];
        fprintf(f,
                " {\"kind\":\"%s\",\"owner\":\"%s\",\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"color\":%u,"
                "\"clip\":[%d,%d,%d,%d],\"hex\":\"",
                kinds[r.kind], r.owner.c_str(), r.x, r.y, r.w, r.h, r.color, r.clipX, r.clipY, r.clipW, r.clipH);
        for (unsigned char c : r.text)
            fprintf(f, "%02x", c);
        fprintf(f, "\"}%s\n", i + 1 < records.size() ? "," : "");
    }
    fprintf(f, "]\n");
    fclose(f);
    return true;
}
#endif

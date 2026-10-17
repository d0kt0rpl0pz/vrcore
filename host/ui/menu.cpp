/*
 * menu.cpp -- the settings menu's pages and rows (the builders), the navigation from the d-pad, keys and buttons, the
 * pointer's ray and clicks, and the drawing of the panel: the layout pass, the controls and the text, in Dear ImGui
 * into quad layers. See menu.h for how it behaves and looks.
 */
#include "menu.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <SDL3/SDL.h>
#include <imgui.h>

#include "imgui_render.h"
#include "input_host.h"
#include "layers.h"
#include "log.h"
#include "text.h"

namespace vrcore::ui {

namespace {

// THE PICTURE (menu.h): the row pitch is 38 comp px, the unit of the design comps; kC is layer pixels a comp px and
// every size below is N * kC, so a headset session tunes them here. The margin is for the note and the front's
// boxes (room round their contents); kFontPx is the label face at the row size, the note's.
constexpr float kSize = 40.f / 48.f;
constexpr float kPitch = 0.0526f * kSize * Menu::kPxPerM;   // pixels a row: 38 comp px
constexpr float kC = kPitch / 38.f;                         // layer pixels a comp px
constexpr float kMargin = 0.05f * Menu::kPxPerM;            // the front's boxes' and the note's margin
constexpr float kHinge = 0.03f;                             // metres off the panel's edge
constexpr float kTurn = 30.f * 3.14159265f / 180.f;         // the boxes turned in towards you
constexpr float kAhead = 1.1f, kBelow = 0.05f;
constexpr float kCanvas = 4096.f;                           // each box's own stretch of ImGui's canvas

// The panel, in comp px. Faces: label 22 (rows), bold 24 (header), bold 13 (card titles), bold 22 (Return); value
// 16 (values, window rows, dropdown), 17 (help), 14 (hint, version, Reset): help and hint raised from 14 and 12 after
// the Quest 3 read them as a little hard (2026-10-16); capitals then 10.5 and 8.7 mm at 1.1 m.
constexpr float kFontPx = 22 * kC;                          // the row label's face; the note's
constexpr float kHeadPx = 24 * kC, kTitlePx = 13 * kC, kValPx = 16 * kC, kHelpPx = 17 * kC, kSmallPx = 14 * kC;
constexpr float kPanelBorder = 2 * kC, kPadTop = 20 * kC, kPadSide = 22 * kC, kPadBottom = 18 * kC;
constexpr float kHeadH = 28 * kC, kHeadGap = 12 * kC;       // the header's line, and the room under it
constexpr float kHeadTrack = 0.06f, kTitleTrack = 0.18f;    // letter spacing, in em
constexpr float kSepGap = 10 * kC, kSepW = 6 * kC, kSepH = 11 * kC;     // the header's chevron: room each side, size
constexpr float kCardBorder = 1.5f * kC, kCardPadX = 6 * kC, kCardPadTop = 6 * kC, kCardPadBottom = 6 * kC;
constexpr float kCardGap = 8 * kC, kTitleH = 28 * kC;
constexpr float kRowH = 38 * kC, kRowPadX = 10 * kC, kColGap = 14 * kC;     // a row; label column to control
constexpr float kBoxH = 28 * kC, kBoxPadX = 10 * kC, kBoxGap = 14 * kC;     // a choice's box: text, gap, triangle
constexpr float kTriW = 9 * kC, kTriH = 5 * kC;
constexpr float kArrowW = 30 * kC, kArrowGap = 8 * kC, kSlotMin = 70 * kC;  // a stepper
constexpr float kResetH = 24 * kC, kResetPadX = 8 * kC;
constexpr float kCheck = 15 * kC, kToggleGap = 9 * kC;   // a toggle's box, and the room after it
constexpr float kLine = 1.5f * kC;                          // a control's outline
constexpr float kPageW = 12 * kC;                           // a page row's chevron
constexpr float kReturnH = 36 * kC, kReturnPad = 16 * kC;
constexpr float kHelpAbove = 8 * kC, kRuleH = 2 * kC, kHelpPad = 10 * kC, kHelpLead = 1.45f;
constexpr float kHintGap = 10 * kC, kHintH = 18 * kC;
constexpr float kVersionGap = 24 * kC;                      // least room between the header and the version
constexpr float kLegendColGap = 24 * kC;                    // the CONTROLS box: widest button to its actions
constexpr float kLegendRule = std::max(1.f, kC);            // the rule between its rows
constexpr float kDropBorder = 2 * kC;                       // an open dropdown's edge

// The menu's colours (sRGB). The key (selected row, hot arrow, a dropdown's edge) and the heading (title, CONTROLS)
// come from the Theme at init; the rest is fixed.
ImU32 g_key = IM_COL32(0xFF, 0xC2, 0x1A, 255);
ImU32 g_heading = IM_COL32(0x8C, 0xFF, 0xCC, 255);
uint32_t g_key_rgb = 0xFFC21A;
ImU32 g_glow = IM_COL32(0xFF, 0xC2, 0x1A, 16);              // a hovered row's wash: the key, nearly clear
const ImU32 kRow = IM_COL32(0xC9, 0xD4, 0xE6, 255);         // row text
const ImU32 kWhite = IM_COL32(255, 255, 255, 255);          // values and bright text
const ImU32 kMuted = IM_COL32(0x6B, 0x7F, 0x96, 255);       // off text, version, hint
const ImU32 kHelp = IM_COL32(0x9F, 0xB4, 0xC4, 255);        // help text
const ImU32 kAction = IM_COL32(0x8F, 0xA3, 0xB8, 255);      // the legend's actions
const ImU32 kNote = IM_COL32(0xff, 0x90, 0x80, 255);         // why a pick did not work
const ImU32 kBack = IM_COL32(0x0A, 0x0E, 0x17, 245);        // the panel's ground
const ImU32 kBorder = IM_COL32(0x24, 0x32, 0x4A, 255);      // a rule, a button at rest, the dropdown's hover
const ImU32 kOutline = IM_COL32(0x3A, 0x4A, 0x63, 255);     // a control's outline
const ImU32 kCard = IM_COL32(0x0F, 0x15, 0x22, 255);        // a card's fill, a dropdown's
const ImU32 kLegendRuleCol = IM_COL32(0x2C, 0x34, 0x42, 255);   // between the CONTROLS rows

// Black or white, whichever reads better on `c` (WCAG contrast ratio from the sRGB relative luminance)
float luminance(ImU32 c)
{
    auto lin = [](float v) { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); };
    const float r = (float)((c >> IM_COL32_R_SHIFT) & 0xFF) / 255.f, g = (float)((c >> IM_COL32_G_SHIFT) & 0xFF) / 255.f,
                b = (float)((c >> IM_COL32_B_SHIFT) & 0xFF) / 255.f;
    return 0.2126f * lin(r) + 0.7152f * lin(g) + 0.0722f * lin(b);
}

ImU32 on_key(uint32_t rgb)
{
    const float l = luminance(IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, 255));
    const float vs_black = (l + 0.05f) / 0.05f, vs_white = 1.05f / (l + 0.05f);
    return vs_black >= vs_white ? IM_COL32(0, 0, 0, 255) : IM_COL32(255, 255, 255, 255);
}

// THE HINT: one line of the value face under the help; \x01 toggles the key words (white; the rest muted). The
// pointer form's second part is the widest it can be: point() leaves out what the hands' profiles do not bind
// (controller_buttons). widest() counts both forms, so nothing resizes when a controller appears.
// The CSV's text marks them (textstrings.csv head): *word* for the key words, | for the middle dot.
#define VR_MID " \xc2\xb7 "
std::string hint_markup(const char *text)
{
    std::string out;
    for (const char *p = text; *p; ++p) {
        if (*p == '*') out += '\x01';
        else if (*p == '|') out += "\xc2\xb7";
        else out += *p;
    }
    return out;
}
const std::string kKeysLine = hint_markup(txt::hint_keys);
const std::string kPointerHead = hint_markup(txt::hint_point);
const std::string kPointerBack = hint_markup(txt::hint_back);
const std::string kPointerMenu = hint_markup(txt::hint_menu);
const std::string kPointerTail = kPointerBack + VR_MID + kPointerMenu;

const ImU32 kDot = IM_COL32(255, 255, 255, 255);
constexpr float kDotPx = 0.008f * Menu::kPxPerM;            // the cursor's radius

// THE ROW KINDS (menu.h): the parts of a row the pointer tells apart
enum Zone { kNoZone = 0, kDec, kInc, kReset, kConfirm, kValue, kSeg0 = 100 };    // kSeg0 + i: a segmented row's i-th segment

double steady_s()
{
    return SDL_GetTicksNS() * 1e-9;
}

Quat yaw_quat(float yaw)
{
    return {0.f, std::sin(yaw * 0.5f), 0.f, std::cos(yaw * 0.5f)};
}

// a point in the panel's own space (x right, y up, +z towards you) to the room
void panel_point(const float pos[3], float yaw, float x, float y, float z, float out[3])
{
    float c = std::cos(yaw), s = std::sin(yaw);
    out[0] = pos[0] + c * x + s * z;
    out[1] = pos[1] + y;
    out[2] = pos[2] - s * x + c * z;
}

}

// ---- text and shapes ----

namespace {

float line_h(const UiFont &f)
{
    return f.f->CalcTextSizeA(f.px, 1e9f, 0, "Ag").y;
}

int utf8_len(unsigned char c)
{
    return c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
}

// TRACKING: ImGui has no letter spacing, so a tracked run is drawn glyph by glyph, `em` * size added after each;
// text_w is its twin. em 0: the plain run.
float text_w(const UiFont &f, const char *s, float em = 0)
{
    if (em == 0) return f.f->CalcTextSizeA(f.px, 1e9f, 0, s).x;
    float w = 0;
    for (const char *p = s; *p;) {
        const int n = std::min(utf8_len((unsigned char)*p), (int)std::strlen(p));
        w += f.f->CalcTextSizeA(f.px, 1e9f, 0, p, p + n).x + em * f.px;
        p += n;
    }
    return w;
}

// the text with its line box centred on `cy`; a bold that is the regular drawn twice a little apart
void put_text(ImDrawList *dl, const UiFont &f, float x, float cy, ImU32 col, const char *s, float em = 0)
{
    const float y = cy - line_h(f) * 0.5f;
    for (int pass = 0; pass < (f.fake ? 2 : 1); ++pass) {
        float px = x + (float)pass * 0.045f * f.px;
        if (em == 0) {
            dl->AddText(f.f, f.px, ImVec2(px, y), col, s);
            continue;
        }
        for (const char *p = s; *p;) {
            const int n = std::min(utf8_len((unsigned char)*p), (int)std::strlen(p));
            dl->AddText(f.f, f.px, ImVec2(px, y), col, p, p + n);
            px += f.f->CalcTextSizeA(f.px, 1e9f, 0, p, p + n).x + em * f.px;
            p += n;
        }
    }
}

std::string upper(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::toupper(c); });
    return s;
}

// the hint's key words (between \x01) white, the rest `rest`; its width without the marks
float hint_w(const UiFont &f, const char *s)
{
    std::string t;
    for (const char *p = s; *p; ++p)
        if (*p != '\x01') t += *p;
    return text_w(f, t.c_str());
}

void put_hint(ImDrawList *dl, const UiFont &f, float x, float cy, const char *s, ImU32 rest)
{
    bool key = false;
    std::string run;
    auto flush = [&] {
        if (run.empty()) return;
        put_text(dl, f, x, cy, key ? kWhite : rest, run.c_str());
        x += text_w(f, run.c_str());
        run.clear();
    };
    for (const char *p = s; *p; ++p) {
        if (*p == '\x01') {
            flush();
            key = !key;
        } else {
            run += *p;
        }
    }
    flush();
}

// shapes (the fallback face lacks the arrows): a chevron pointing left (dir -1) or right (+1) centred on (cx, cy)
void chevron(ImDrawList *dl, float cx, float cy, float hw, float hh, float dir, ImU32 col, float thick)
{
    const ImVec2 pts[3] = {ImVec2(cx - dir * hw, cy - hh), ImVec2(cx + dir * hw, cy), ImVec2(cx - dir * hw, cy + hh)};
    dl->AddPolyline(pts, 3, col, 0, thick);
}

// a filled triangle pointing down (or up)
void triangle(ImDrawList *dl, float cx, float cy, float w, float h, bool up, ImU32 col)
{
    const float d = up ? -1.f : 1.f;
    dl->AddTriangleFilled(ImVec2(cx - w / 2, cy - d * h / 2), ImVec2(cx + w / 2, cy - d * h / 2), ImVec2(cx, cy + d * h / 2), col);
}

}

// THE LAYOUT PASS (SIZES): one function gives, for a page and a rule for which rows count, the y and height of every
// card, row, the Return row, the help and the hint. The measure runs it with every row, the draw with the shown
// ones, so they cannot disagree. Positions are in layer pixels from the panel's top; x follows from the panel's width.
struct Menu::Plan {
    struct Row {
        float y = 0, h = 0;
        bool drawn = false;
        int card = -1;                      // its card (-1: the Return row)
    };
    struct Card {
        int first = -1;                     // the page index it starts at
        bool titled = false, drawn = false;
        float y = 0, h = 0;
        int rows = 0;                       // rows drawn
    };
    const Page *page = nullptr;
    std::vector<Row> rows;                  // by page index
    std::vector<Card> cards;
    float label_col = 0;                    // the label column's width
    float content_w = 0;                    // the widest row's content, past the row padding
    float rule_y = -1, help_y = 0, hint_y = 0;
    size_t help_rows = 0;
    float total = 0;                        // the panel's height
    float drop_bottom = 0;                  // the lowest an open dropdown reaches, as a panel height
};

// what control() draws with
struct Menu::Ctl {
    ImDrawList *dl;
    bool selected, open, focus;
    int hot;                                // the Zone under the ray on this row
};

bool Menu::init(gpu::Renderer &r, const Hooks &hooks, std::vector<Box> boxes)
{
    hooks_ = hooks;
    boxes_ = std::move(boxes);
    ImGuiContext *ctx = ImGui::CreateContext();
    ctx_ = ctx;
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    // THE THEME: the colours first, then the faces. Each face is read once (SDL_LoadFile, by the assets' path as the
    // wheel is) and handed to ImGui as a copy it frees; a size is a font of its own, baked whole below.
    const Theme &th = hooks_.theme;
    g_key_rgb = th.key ? (th.key & 0xFFFFFF) : 0xFFC21A;
    const uint32_t hd = th.heading ? (th.heading & 0xFFFFFF) : 0x8CFFCC;
    g_key = IM_COL32((g_key_rgb >> 16) & 0xFF, (g_key_rgb >> 8) & 0xFF, g_key_rgb & 0xFF, 255);
    g_glow = (g_key & 0x00FFFFFF) | (16u << IM_COL32_A_SHIFT);
    g_heading = IM_COL32((hd >> 16) & 0xFF, (hd >> 8) & 0xFF, hd & 0xFF, 255);
    models_.set_ray_hot(g_key_rgb);
    struct Face {
        void *data = nullptr;
        size_t size = 0;
    };
    auto read = [](const std::string &path, const char *role) {
        Face f;
#ifdef __ANDROID__
        const std::string full = path;
#else
        const std::string full = std::string(VRCORE_ASSETS_DIR "/") + path;
#endif
        f.data = SDL_LoadFile(full.c_str(), &f.size);
        if (f.data && f.size < 100) {       // empty or truncated: not a face (ImGui asserts on it)
            SDL_free(f.data);
            f.data = nullptr;
            SDL_SetError("only %zu bytes", f.size);
        }
        if (f.data) log("ui: font %s %s (ok)", role, path.c_str());
        else log("ui: font %s %s failed: %s", role, path.c_str(), SDL_GetError());
        return f;
    };
    // `want` then `fallback`; an empty face when neither loads
    auto load = [&](const std::string &want, const std::string &fallback, const char *role) {
        Face f;
        if (!want.empty()) f = read(want, role);
        if (!f.data && !fallback.empty() && want != fallback) f = read(fallback, role);
        return f;
    };
    // ImGui keeps this array: the middle dot and the single guillemets, the triangles
    static const ImWchar kRanges[] = {0x20, 0xFF, 0x2039, 0x203A, 0x25B2, 0x25B2, 0x25BE, 0x25BE, 0};
    auto add_face = [&](const Face &f, float px, const char *name) -> ImFont * {
        ImFontConfig cfg;
        cfg.SizePixels = px;
        if (!f.data) return io.Fonts->AddFontDefaultVector(&cfg);
        void *copy = IM_ALLOC(f.size);
        std::memcpy(copy, f.data, f.size);
        cfg.FontDataOwnedByAtlas = true;
        cfg.GlyphRanges = kRanges;
        std::snprintf(cfg.Name, sizeof cfg.Name, "%s %.0f", name, px);
        ImFont *made = io.Fonts->AddFontFromMemoryTTF(copy, (int)f.size, px, &cfg);
        if (!made) {                        // a file that opened but did not parse: the built-in face
            log("ui: font %s did not parse: the built-in face", name);
            ImFontConfig bi;
            bi.SizePixels = px;
            made = io.Fonts->AddFontDefaultVector(&bi);
        }
        return made;
    };
    const bool label_default = th.label_font.empty(), value_default = th.value_font.empty();
    const std::string label_def = "fonts/BarlowCondensed-Medium.ttf", value_def = "fonts/IBMPlexMono-Regular.ttf";
    const std::string label_path = label_default ? label_def : th.label_font;
    const std::string value_path = value_default ? value_def : th.value_font;
    const std::string label_bold_path = !th.label_bold_font.empty() ? th.label_bold_font
                                         : label_default ? "fonts/BarlowCondensed-SemiBold.ttf" : label_path;
    const std::string value_bold_path = !th.value_bold_font.empty() ? th.value_bold_font
                                         : value_default ? "fonts/IBMPlexMono-SemiBold.ttf" : value_path;
    Face label = load(label_path, label_def, "label");
    Face value = load(value_path, value_def, "value");
    // a bold that is the regular's own file, or fails, is no face: the regular is drawn twice instead
    Face label_bold, value_bold;
    if (label_bold_path != label_path && label.data) label_bold = load(label_bold_path, "", "label bold");
    if (value_bold_path != value_path && value.data) value_bold = load(value_bold_path, "", "value bold");
    // each size is a font of its own; a bold with no file is the regular, drawn twice
    auto plain = [&](const Face &f, float px, const char *name) { return UiFont{add_face(f, px, name), px, false}; };
    auto bold = [&](const Face &b, const Face &reg, float px, const char *name) {
        return b.data ? UiFont{add_face(b, px, name), px, false} : UiFont{add_face(reg, px, name), px, true};
    };
    row_f_ = plain(label, kFontPx, "label");
    font_ = row_f_.f;
    head_f_ = bold(label_bold, label, kHeadPx, "label bold");
    title_f_ = bold(label_bold, label, kTitlePx, "label bold");
    ret_f_ = bold(label_bold, label, kFontPx, "label bold");
    val_f_ = plain(value, kValPx, "value");
    valb_f_ = bold(value_bold, value, kValPx, "value bold");
    help_f_ = plain(value, kHelpPx, "value");
    small_f_ = plain(value, kSmallPx, "value");
    smallb_f_ = bold(value_bold, value, kSmallPx, "value bold");
    for (Face *f : {&label, &value, &label_bold, &value_bold})
        if (f->data) SDL_free(f->data);
    ImGuiStyle &st = ImGui::GetStyle();
    st.WindowPadding = ImVec2(0, 0);
    st.WindowBorderSize = 0;
    st.WindowRounding = 0;
    st.AntiAliasedLinesUseTex = false;

    render_ = new ImguiRender;
    if (!render_->init(r)) return false;
    auto make = [&](Drawn &d, uint32_t w, uint32_t h, float x, bool left) {
        d.layer = layer_create(r, {w, h, false, 0});
        d.max_w = w;
        d.max_h = h;
        d.x = x;
        d.left = left;
        return d.layer != nullptr;
    };
    // the panel: the tallest page (cards, help, hint) is ~1500 px at kC; a page that needs more is logged (prepare)
    // the note first: layers are composited in the order they are made, so the menu's boxes cover it
    bool ok = make(note_, 1024, 64, kCanvas * 2, false);
    ok &= make(panel_, 2048, 2048, 0, false);
    if (hooks_.legend) ok &= make(legend_, 1280, 1024, kCanvas, false);
    box_layers_.resize(boxes_.size());
    for (size_t i = 0; i < boxes_.size(); ++i)
        ok &= make(box_layers_[i], boxes_[i].max_width, boxes_[i].max_height, kCanvas * (3 + i), boxes_[i].left);
    gpu_ = &r;
    models_.init(r);                        // not fatal: the menu works with no controllers drawn
    log("ui: menu %s, %.0f px a metre, layers %s", ok ? "ready" : "NOT ready", kPxPerM, ok ? "made" : "missing");
    return ok;
}

void Menu::shutdown(gpu::Renderer &r)
{
    layer_destroy(r, panel_.layer);
    layer_destroy(r, legend_.layer);
    layer_destroy(r, note_.layer);
    for (Drawn &d : box_layers_) layer_destroy(r, d.layer);
    models_.shutdown(r);
    gpu_ = nullptr;
    box_layers_.clear();
    panel_ = legend_ = note_ = Drawn();
    if (render_) {
        render_->shutdown(r);
        delete render_;
        render_ = nullptr;
    }
    if (ctx_) ImGui::DestroyContext(static_cast<ImGuiContext *>(ctx_));
    ctx_ = nullptr;
    open_ = showing_message_ = false;
    input_set_menu_up(false);
}

void Menu::set_top(Page *top, const char *title)
{
    top_ = page_ = top;
    top_title_ = title;
    page_title_.clear();
}

// ---- the pages ----

void Menu::enter(Item &it)
{
    disarm(page_);
    button_focus_ = false;
    dropdown_ = -1;
    stack_.push_back({page_, page_title_, sel_});
    page_ = it.sub;
    page_title_ = upper(it.label);
    sel_ = 0;
    restart_yes_ = false;
}

bool Menu::enter_named(const char *prefix)
{
    if (!open_ || !page_) return false;
    for (Item &it : *page_) {
        if (!it.sub || (it.shown && !it.shown())) continue;
        if (SDL_strncasecmp(it.label.c_str(), prefix, SDL_strlen(prefix)) == 0) {
            enter(it);
            return true;
        }
    }
    return false;
}

void Menu::back()
{
    if (stack_.empty()) {
        close();
        return;
    }
    disarm(page_);
    button_focus_ = false;
    dropdown_ = -1;
    page_ = stack_.back().page;
    page_title_ = stack_.back().title;
    sel_ = stack_.back().sel;
    stack_.pop_back();
    restart_yes_ = false;
}

void Menu::open()
{
    if (open_ || !top_ || (hooks_.can_open && !hooks_.can_open())) return;
    open_ = true;
    if (hooks_.on_open) hooks_.on_open();
    for (auto &a : attached_)
        if (a.second.on_open) a.second.on_open();
    disarm(page_);
    button_focus_ = false;
    dropdown_ = -1;
    stack_.clear();
    page_ = top_;
    page_title_.clear();
    restart_yes_ = false;
    sel_ = 0;
    quads_.clear();
    hit_id_ = hover_row_ = -1;
    repeat_at_ = now_ + 0.3;
    placed_ = false;                        // placed in front of the head this update
    menu_w_ = 0;                            // measured again (SIZES)
    sized_page_ = nullptr;
}

void Menu::close()
{
    if (!open_) return;
    open_ = false;
    disarm(page_);
    button_focus_ = false;
    dropdown_ = -1;
    if (hooks_.on_close) hooks_.on_close();
}

Item Menu::restart_item(std::function<void()> saved, bool gap)
{
    Item it;
    it.label = txt::menu_restart_label;
    it.value = [this] { return std::string(restart_yes_ ? txt::menu_yes : txt::menu_no); };
    it.change = [this](int) { restart_yes_ = !restart_yes_; };
    it.press = [this, saved] {
        if (!restart_yes_) return;
        close();
        if (saved) saved();
        log("ui: restart game");
        request_restart();
    };
    it.gap = gap;
    it.help = txt::menu_restart_help;
    return it;
}

Item Menu::return_item()
{
    Item it;
    it.label = txt::menu_return;
    it.kind = Kind::Return;
    it.change = [this](int d) {
        if (d > 0) back();
    };
    return it;
}

// ---- the row builders (menu.h THE ROW KINDS): the old fields filled so the d-pad does what it always did ----

Item toggle(const std::string &label, std::function<bool()> get, std::function<void(bool)> set, const char *on_text,
            const char *off_text)
{
    Item it;
    it.label = label;
    it.kind = Kind::Toggle;
    it.value = [get, on_text, off_text] { return std::string(get() ? on_text : off_text); };
    it.change = [get, set](int) { set(!get()); };
    it.choices = {on_text, off_text};       // both texts, for the picture (menu.h); not a dropdown (kind is Toggle)
    it.index = [get] { return get() ? 0 : 1; };
    return it;
}

Item choice(const std::string &label, std::vector<std::string> labels, std::function<int()> get,
            std::function<void(int)> set)
{
    Item it;
    it.label = label;
    it.kind = Kind::Choice;
    it.choices = labels;
    it.index = get;
    it.pick = set;
    const int n = (int)labels.size();
    it.value = [labels, get] {
        int i = get();
        return i >= 0 && i < (int)labels.size() ? labels[i] : std::string();
    };
    it.change = [get, set, n](int d) {
        if (n > 0) set(((get() + d) % n + n) % n);
    };
    return it;
}

Item segmented(const std::string &label, std::vector<std::string> labels, std::function<int()> get,
               std::function<void(int)> set)
{
    Item it = choice(label, labels, get, set);
    it.kind = Kind::Segmented;
    const int n = (int)labels.size();
    it.change = [get, set, n](int d) {      // stops at the ends: the strip shows where you are
        const int i = std::min(std::max(get() + d, 0), n - 1);
        if (n > 0 && i != get()) set(i);
    };
    return it;
}

Item checkbox(const std::string &label, std::function<bool()> get, std::function<void(bool)> set)
{
    // on reads nothing and off starts "Off", so the toggle draws the box alone (menu.h THE ROW KINDS)
    return toggle(label, get, set, "", txt::toggle_off);
}

Item segmented_toggle(const std::string &label, std::function<bool()> get, std::function<void(bool)> set,
                      const char *first, const char *second)
{
    Item it = segmented(label, {first, second}, [get] { return get() ? 0 : 1; }, [set](int i) { set(i == 0); });
    it.press = [get, set] { set(!get()); };     // A flips it, as a toggle's does
    return it;
}

Item stepper(const std::string &label, std::function<float()> get, std::function<void(float)> set, float step,
             float lo, float hi, std::function<std::string(float)> format, std::function<void()> reset)
{
    Item it;
    it.label = label;
    it.kind = Kind::Stepper;
    it.value = [get, format] { return format(get()); };
    it.change = [get, set, step, lo, hi](int d) {
        float v = (std::nearbyint(get() / step) + (float)d) * step;
        set(v < lo ? lo : v > hi ? hi : v);
    };
    if (reset) {
        it.reset = reset;
        it.press = reset;
    }
    // the widest text over lo..hi (capped at 2000 steps, and hi itself), for the value's slot
    // (a bad step or range samples lo and hi only)
    const bool sane = std::isfinite(step) && step > 0 && std::isfinite(lo) && std::isfinite(hi) && hi >= lo;
    const int n = sane ? (int)std::min(2000.f, std::floor((hi - lo) / step + 0.5f)) : 0;
    for (int i = 0; i <= n + 1; ++i) {
        const float v = !sane ? (i == 0 ? lo : hi) : i > n ? hi : std::min(hi, lo + (float)i * step);
        const std::string t = format(v);
        if (t.size() > it.widest_value.size()) it.widest_value = t;
    }
    return it;
}

Item action(const std::string &label, std::function<void()> run)
{
    Item it;
    it.label = label;
    it.kind = Kind::Action;
    it.change = [run](int d) {
        if (d > 0 && run) run();
    };
    return it;
}

Item page(const std::string &label, Page &sub)
{
    Item it;
    it.label = label;
    it.kind = Kind::Page;
    it.sub = &sub;
    return it;
}

Item note(const std::string &widest, std::function<std::string()> text)
{
    Item it;
    it.label = widest;
    it.kind = Kind::Note;
    it.label_fn = std::move(text);
    return it;
}

Item confirm(const std::string &label, std::function<void()> run)
{
    Item it;
    it.label = label;
    it.kind = Kind::Confirm;
    it.run = run;
    auto armed = std::make_shared<bool>(false);
    it.armed = armed;
    it.value = [armed] { return std::string(*armed ? txt::menu_yes : txt::menu_no); };
    it.change = [armed](int) { *armed = !*armed; };
    return it;
}

// Confirm rows back to No (leaving the row or the page); `except` keeps one
void Menu::disarm(Page *p, const Item *except)
{
    if (!p) return;
    for (Item &it : *p)
        if (it.armed && &it != except) *it.armed = false;
}

void Menu::run_confirm(Item &it)
{
    std::function<void()> run = it.run;     // the page may be gone when it returns
    if (it.armed) *it.armed = false;
    button_focus_ = false;
    if (run) run();
}

// ---- every update ----

void Menu::place(const Input &in)
{
    if (in.head_tracked) last_head_ = in.head;
    const Pose &h = last_head_;
    // the head's forward (-z turned by its rotation), level
    const Quat &q = h.rot;
    float fx = -(2.f * (q.x * q.z + q.w * q.y));
    float fz = -(1.f - 2.f * (q.x * q.x + q.y * q.y));
    if (fx * fx + fz * fz < 1e-8f) fz = -1.f;
    yaw_ = std::atan2(-fx, -fz);            // the turn that takes -z to the forward
    float n = std::sqrt(fx * fx + fz * fz);
    pos_[0] = h.pos[0] + fx / n * kAhead;
    pos_[1] = h.pos[1] - kBelow;
    pos_[2] = h.pos[2] + fz / n * kAhead;
    placed_ = true;
}

void Menu::attach(const char *id, Attached a)
{
    for (auto &e : attached_)
        if (e.first == id) {
            e.second = std::move(a);
            return;
        }
    attached_.emplace_back(id, std::move(a));
}

void Menu::update(const Tick &t, const Input &in)
{
    step(t, in);
    for (auto &a : attached_)
        if (a.second.update) a.second.update(t, in);
}

void Menu::step(const Tick &t, const Input &in)
{
    // whatever way this returns, the host learns whether a panel is up (the sim's fake controller: input_host.h)
    struct Report {
        const Menu *m;
        ~Report() { input_set_menu_up(m->shown()); }
    } report{this};
    now_ = steady_s();
    // the edges of this frame's buttons
    const Hand &lh = in.hands[Input::kLeft], &rh = in.hands[Input::kRight];
    bool hand_now[2][3] = {{lh.primary, lh.secondary, lh.menu}, {rh.primary, rh.secondary, rh.menu}};
    bool hand_pressed[2][3];
    for (int h = 0; h < 2; ++h)
        for (int b = 0; b < 3; ++b) {
            hand_pressed[h][b] = hand_now[h][b] && !hand_was_[h][b];
            hand_was_[h][b] = hand_now[h][b];
        }
    bool pad_south = false, pad_east = false, pad_start = false;
    std::vector<PadWas> pads_now;
    for (int i = 0; i < in.pad_count; ++i) {
        const Pad &p = in.pads[i];
        PadWas was = {p.id, false, false, false};
        for (const PadWas &w : pads_was_)
            if (w.id == p.id) was = w;
        pad_south |= p.south && !was.south;
        pad_east |= p.east && !was.east;
        pad_start |= p.start && !was.start;
        pads_now.push_back({p.id, p.south, p.east, p.start});
    }
    pads_was_ = pads_now;
    // THE POINTER's hand: the right, else the left, shown by the rule with its aim tracked (-1 none). point() casts
    // its ray, and the controllers drawn (prepare) give it the beam
    pointing_ = in.controller_shown(Input::kRight) && in.hands[Input::kRight].aim_tracked ? Input::kRight
              : in.controller_shown(Input::kLeft) && in.hands[Input::kLeft].aim_tracked ? Input::kLeft : -1;
    for (int h = 0; h < 2; ++h) {
        ControllerModels::Draw &c = ctl_[h];
        c.shown = in.controller_shown(h);
        c.grip = in.hands[h].grip;
        c.aim = in.hands[h].aim;
        c.ray = h == pointing_;
    }
    // the triggers' edges (on at 0.6, off under 0.4)
    for (int h = 0; h < 2; ++h) {
        float v = in.hands[h].trigger;
        bool now = trigger_was_[h] ? v > 0.4f : v > 0.6f;
        trigger_pressed_[h] = now && !trigger_was_[h];
        trigger_was_[h] = now;
    }
    bool select = in.pressed[SDL_SCANCODE_RETURN] || in.pressed[SDL_SCANCODE_KP_ENTER] ||
                  in.pressed[SDL_SCANCODE_SPACE] || pad_south || hand_pressed[0][0] || hand_pressed[1][0];
    bool back_pressed = in.pressed[SDL_SCANCODE_BACKSPACE] || pad_east || hand_pressed[0][1] || hand_pressed[1][1];
    bool toggle = in.pressed[SDL_SCANCODE_ESCAPE] || pad_start || hand_pressed[0][2];

    // FOCUS LOSS: open when the headset takes the focus away
    if (focused_was_ && !t.focused && hooks_.open_on_focus_loss) {
        log("ui: focus lost: the menu opens");
        open();
    }
    focused_was_ = t.focused;

    // the game could not start: the message in place of the menu
    std::string msg = hooks_.message ? hooks_.message() : std::string();
    if (!msg.empty()) {
        // the pointer works the picker's row: the trigger's edge on it picks as A does (once a press); not on the
        // first frame, when the hit test would still see the menu's rows
        bool click = false;
        if (showing_message_) point(in, click);
        else hit_id_ = -1, msg_hover_ = false;
        click = click && msg_hover_ && hooks_.pick;
        if (hooks_.pick) hooks_.pick(select || click);
        if (click) haptic(pointing_, 0.6f, 0.05f);
        if (!showing_message_ || !placed_) place(in);
        if (!showing_message_) msg_w_ = msg_h_ = 0;     // sized afresh for this showing (prepare)
        showing_message_ = true;
        hover_row_ = drop_hover_ = -1;                  // only the \x01 row (msg_hover_) is under the pointer
        dropdown_ = -1;
        return;
    }
    showing_message_ = false;

    if (toggle) {
        if (open_) close();
        else open();
    }
    if (!open_) {
        hit_id_ = hover_row_ = pointing_ = -1;
        pointer_hint_ = false;
        return;
    }
    if (!placed_) place(in);

    if (!list_visible()) {                  // every row hidden: nothing to select, but B still leaves the page
        drop_hover_ = -1;
        if (back_pressed) back();
        return;
    }

    // THE POINTER: hover selects the row under the ray; the trigger's edge clicks it (THE ROW KINDS)
    if (dropdown_ >= (int)visible_.size() || (dropdown_ >= 0 && !(visible_[dropdown_]->kind == Kind::Choice && !visible_[dropdown_]->choices.empty()))) dropdown_ = -1;
    {
        bool click = false;
        point(in, click);
        if (pointing_ < 0) dropdown_ = -1;  // the controller went (a pad or key took over): no list left hanging
        bool acted = false, hold = false;
        if (pointing_ >= 0 && dropdown_ >= 0) {
            if (click) {
                Item &d = *visible_[dropdown_];
                // the value it already has is not set again: a setter may save or count it as a change by hand
                const bool same = d.index && d.index() == drop_hover_;
                if (drop_hover_ >= 0 && drop_hover_ < (int)d.choices.size() && d.pick && !same) d.pick(drop_hover_);
                dropdown_ = -1;
                acted = true;
            }
        } else if (pointing_ >= 0 && hover_row_ >= 0 && hover_row_ < (int)visible_.size()) {
            Item &it = *visible_[hover_row_];
            sel_ = hover_row_;
            const bool half = hover_zone_ == kDec || hover_zone_ == kInc;
            if (it.kind == Kind::Stepper) {
                bool step = false;
                if (click && half) {
                    step = true;
                    ptr_hold_ = true;
                    ptr_repeat_at_ = now_ + 0.35;
                } else if (click && hover_zone_ == kReset && it.reset) {
                    it.reset();
                    acted = true;
                } else if (!click && half && ptr_hold_ && trigger_was_[pointing_] && now_ >= ptr_repeat_at_) {
                    step = true;
                    hold = true;
                    ptr_repeat_at_ = now_ + 0.12;
                }
                if (step && it.change) {
                    it.change(hover_zone_ == kInc ? +1 : -1);
                    acted = true;
                }
            } else if (it.kind == Kind::Segmented) {
                const int i = hover_zone_ - kSeg0;
                if (click && hover_zone_ >= kSeg0 && i < (int)it.choices.size()) {
                    if (it.pick && !(it.index && it.index() == i)) it.pick(i);
                    acted = true;
                }
            } else if (click) {
                acted = true;
                if (it.kind == Kind::Choice && !it.choices.empty()) {
                    dropdown_ = hover_row_;
                } else if (it.kind == Kind::Confirm) {
                    if (hover_zone_ == kConfirm && armed(it)) run_confirm(it);
                    else if (it.change) it.change(+1);
                } else {
                    select = true;          // a toggle, action, page, Return, or an old-style row: A's path
                }
            }
        }
        if (pointing_ < 0 || !trigger_was_[pointing_]) ptr_hold_ = false;
        if (acted) haptic(pointing_, hold ? 0.3f : 0.6f, hold ? 0.02f : 0.05f);
    }
    if (!open_) return;                     // a Confirm's button closed the menu
    // the click may have gone back a page (a Confirm's button), or hidden rows: the list again before it is read
    if (!list_visible()) return;
    {
        // a Confirm row left puts it back to No; its button's focus goes with the Yes
        disarm(page_, visible_[sel_]);
        if (!armed(*visible_[sel_])) button_focus_ = false;
    }

    // one direction from any source, with key repeat
    float sx = 0, sy = 0;
    auto take = [&](float x, float y) {
        if (x * x + y * y > sx * sx + sy * sy) {
            sx = x;
            sy = y;
        }
    };
    take(lh.stick[0], lh.stick[1]);
    take(rh.stick[0], rh.stick[1]);
    for (int i = 0; i < in.pad_count; ++i) {
        const Pad &p = in.pads[i];
        take(p.left[0], p.left[1]);
        if (p.up) sy = 1;
        if (p.down) sy = -1;
        if (p.left_dpad) sx = -1;
        if (p.right_dpad) sx = 1;
    }
    if (in.keys[SDL_SCANCODE_UP]) sy = 1;
    if (in.keys[SDL_SCANCODE_DOWN]) sy = -1;
    if (in.keys[SDL_SCANCODE_LEFT]) sx = -1;
    if (in.keys[SDL_SCANCODE_RIGHT]) sx = 1;
    int dir = std::fabs(sy) > 0.6f ? (sy > 0 ? 1 : 2) : std::fabs(sx) > 0.6f ? (sx > 0 ? 3 : 4) : 0;
    bool fire = dir != 0 && (dir != last_dir_ || now_ >= repeat_at_);
    if (dir != last_dir_) repeat_at_ = now_ + 0.35;
    else if (fire) repeat_at_ = now_ + 0.12;
    last_dir_ = dir;
    int n = (int)visible_.size();
    // the d-pad, the keys and A never touch a dropdown: it closes, and the press goes on as it always did
    if (dropdown_ >= 0 && (fire || select)) dropdown_ = -1;
    if (fire) {
        Item &it = *visible_[sel_];
        const bool conf = it.kind == Kind::Confirm;
        const bool scrolled = (dir == 1 || dir == 2) && it.edge && it.edge(dir == 1 ? -1 : 1);
        if (dir == 1 && !scrolled) sel_ = (sel_ + n - 1) % n, off_notes(-1);
        if (dir == 2 && !scrolled) sel_ = (sel_ + 1) % n, off_notes(+1);
        if (dir == 3 && it.sub) enter(it);
        else if (dir == 3 && conf && armed(it)) button_focus_ = true;
        else if (dir == 3 && it.value && it.change) it.change(+1);
        if (dir == 4 && conf && button_focus_) button_focus_ = false;
        else if (dir == 4 && it.value && it.change) it.change(-1);
    }
    // the d-pad may have entered a page or changed a row this frame: A reads the list as it is now
    if (open_ && select && list_visible()) {
        Item &it = *visible_[sel_];
        if (it.kind == Kind::Confirm) {
            if (button_focus_) run_confirm(it);
            else if (armed(it)) button_focus_ = true;
        } else if (it.sub) {
            enter(it);
        } else if (it.press) {
            it.press();
        } else if (it.change) {
            it.change(+1);
        }
    } else if (open_ && back_pressed) {
        if (dropdown_ >= 0) dropdown_ = -1;     // B leaves the dropdown, not the page
        else back();
    }
    // a row's change may have hidden rows or changed page: the list again for the draw
    if (open_ && list_visible()) {
        disarm(page_, visible_[sel_]);
        if (!armed(*visible_[sel_])) button_focus_ = false;
    }
}

// The page's shown rows into visible_, the selection kept on one of them (off a Note, as off_notes) and an open
// dropdown dropped if its row is gone. False when the page shows no row: then nothing may read visible_[sel_].
bool Menu::list_visible()
{
    visible_.clear();
    for (Item &it : *page_)
        if (!it.shown || it.shown()) visible_.push_back(&it);
    if (dropdown_ >= (int)visible_.size()) dropdown_ = -1;
    if (visible_.empty()) {
        sel_ = 0;
        return false;
    }
    sel_ = std::clamp(sel_, 0, (int)visible_.size() - 1);
    off_notes(+1);
    return true;
}

// The selection moved off a Note (text, not a control) to the next row in `step`'s direction, wrapping; it stays
// when the page is all notes.
void Menu::off_notes(int step)
{
    const int n = (int)visible_.size();
    for (int i = 0; i < n && visible_[sel_]->kind == Kind::Note; ++i) sel_ = (sel_ + n + step) % n;
}

// ---- the picture ----

namespace {

bool has_control(const Item &it)
{
    return it.value || it.sub || it.kind == Kind::Page;
}

}

// One row's control (THE ROW KINDS) at x, vertically centred on cy: drawn when `d` is given, and its width returned
// either way; `rr` gets the x edges the pointer needs. `worst`: for SIZES, the widest it can be: a reserved value with
// its reserve, a choice's longest label, a Confirm with its button. The measure and the draw both come through here.
float Menu::control(const Item &it, bool worst, float x, float cy, const Ctl *d, RowRect *rr) const
{
    ImDrawList *dl = d ? d->dl : nullptr;
    const bool sel = d && d->selected;
    const ImU32 ok = on_key(g_key_rgb);
    const ImU32 ink = sel ? ok : kWhite, outline = sel ? ok : kOutline;
    const ImU32 ok60 = (ok & 0x00FFFFFF) | (153u << IM_COL32_A_SHIFT);
    // the selected row's values drawn twice, a hair apart: dark text on the key bar reads lighter than white on the
    // card, so it gets the weight back (the widths are measured in the plain bold face)
    UiFont vb = valb_f_;
    vb.fake = vb.fake || sel;
    const float top = cy - kBoxH / 2, bot = cy + kBoxH / 2;
    auto box = [&](float x0, float x1, float y0, float y1, ImU32 fill, bool filled) {
        if (!dl) return;
        if (filled) dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), fill);
        else dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), outline, 0, 0, kLine);
    };
    auto value_text = [&](const std::string &t) { return (it.reserve && worst) ? t + hooks_.value_reserve : t; };

    if (it.kind == Kind::Choice) {
        float best = 0;
        for (const std::string &c : it.choices) best = std::max(best, text_w(valb_f_, c.c_str()));
        if (it.choices.empty()) best = text_w(valb_f_, it.value().c_str());
        const float bw = kBoxPadX + best + kBoxGap + kTriW + kBoxPadX;
        if (rr) rr->val0 = x, rr->val1 = x + bw;
        if (dl) {
            box(x, x + bw, top, bot, ok, sel);
            put_text(dl, vb, x + kBoxPadX, cy, sel ? g_key : kWhite, it.value().c_str());
            triangle(dl, x + bw - kBoxPadX - kTriW / 2, cy, kTriW, kTriH, d->open, sel ? g_key : kMuted);
        }
        return bw;
    }
    if (it.kind == Kind::Segmented) {
        // one strip, a rule between segments; the current one filled (the key colour, or the bar's ink on the
        // selected row), the one under the ray lit like a stepper's arrow
        const int cur = it.index ? it.index() : -1;
        float sx = x;
        if (rr) rr->seg.assign(1, x);
        for (size_t i = 0; i < it.choices.size(); ++i) {
            const char *t = it.choices[i].c_str();
            const float w = 2 * kBoxPadX + text_w(valb_f_, t);
            if (dl) {
                const bool on = (int)i == cur, hot = d->hot == kSeg0 + (int)i;
                if (on) dl->AddRectFilled(ImVec2(sx, top), ImVec2(sx + w, bot), sel ? ok : g_key);
                const ImU32 ink_on = sel ? g_key : ok;
                put_text(dl, on ? vb : valb_f_, sx + kBoxPadX, cy, on ? ink_on : hot ? (sel ? ok : g_key) : sel ? ok60 : kMuted, t);
                if (i > 0) dl->AddLine(ImVec2(sx, top), ImVec2(sx, bot), outline, kLine);
            }
            sx += w;
            if (rr) rr->seg.push_back(sx);
        }
        if (dl) dl->AddRect(ImVec2(x, top), ImVec2(sx, bot), outline, 0, 0, kLine);
        // a front's own value that says more than the label ("MPH (Requires Restart)"): the rest follows the strip,
        // muted, and `reserve` keeps room for Hooks::value_reserve as it does for any value
        auto trim = [](const std::string &t) { return t.substr(std::min(t.find_first_not_of(' '), t.size())); };
        std::string note;
        if (it.value && cur >= 0 && cur < (int)it.choices.size()) {
            const std::string v = it.value(), &l = it.choices[cur];
            if (v.size() > l.size() && v.compare(0, l.size(), l) == 0) note = trim(v.substr(l.size()));
        }
        float nw = text_w(val_f_, note.c_str());
        if (it.reserve && worst) nw = std::max(nw, text_w(val_f_, trim(hooks_.value_reserve).c_str()));
        if (dl && !note.empty()) put_text(dl, val_f_, sx + kToggleGap, cy, sel ? ok60 : kMuted, note.c_str());
        return sx - x + (nw > 0 ? kToggleGap + nw : 0);
    }
    if (it.kind == Kind::Stepper) {
        const std::string v = it.value();
        const float slot = std::max({kSlotMin, text_w(valb_f_, it.widest_value.c_str()), text_w(valb_f_, v.c_str())});
        const float sx = x + kArrowW + kArrowGap, x2 = sx + slot + kArrowGap;
        float end = x2 + kArrowW;
        if (rr) rr->dec0 = x, rr->inc1 = end;
        if (dl) {
            for (int side = 0; side < 2; ++side) {
                const float bx = side ? x2 : x;
                const bool hot = d->hot == (side ? kInc : kDec);
                box(bx, bx + kArrowW, top, bot, sel ? ok : g_key, hot);
                chevron(dl, bx + kArrowW / 2, cy, 3 * kC, 5 * kC, side ? 1.f : -1.f, hot ? (sel ? g_key : ok) : (sel ? ok : kRow), 2 * kC);
            }
            put_text(dl, vb, sx + (slot - text_w(valb_f_, v.c_str())) / 2, cy, ink, v.c_str());
        }
        if (it.reset) {
            const float rx = end + kArrowGap, rw = text_w(smallb_f_, txt::menu_reset) + 2 * kResetPadX;
            if (rr) rr->reset0 = rx, rr->reset1 = rx + rw;
            if (dl) {
                const bool hot = d->hot == kReset;
                box(rx, rx + rw, cy - kResetH / 2, cy + kResetH / 2, sel ? ok : g_key, hot);
                put_text(dl, smallb_f_, rx + kResetPadX, cy, hot ? (sel ? g_key : ok) : sel ? ok : kMuted, txt::menu_reset);
            }
            end = rx + rw;
        }
        return end - x;
    }
    if (it.kind == Kind::Toggle) {
        // one box, filled while on. An on / off pair (the off text starts "Off") always reads its on text, so the
        // box alone says the state; other pairs read the current text. Whatever a front's value adds
        // after either text ("Off (Requires Restart)") follows in muted ink; a value matching neither is drawn alone.
        const bool two = it.choices.size() >= 2;
        const bool on = (it.index ? it.index() : 0) == 0;
        auto starts = [](const std::string &s, const std::string &p) { return s.compare(0, p.size(), p) == 0; };
        const bool onoff = two && starts(it.choices[1], txt::toggle_off);
        // the word and note a value is drawn as
        auto split = [&](const std::string &v, bool state, std::string &word, std::string &note) {
            word = v, note.clear();
            if (!two) return;
            const std::string &mine = it.choices[state ? 0 : 1], &other = it.choices[state ? 1 : 0];
            const std::string *m = starts(v, mine) ? &mine : starts(v, other) ? &other : nullptr;
            if (!m) return;
            word = onoff ? it.choices[0] : *m;
            note = v.substr(m->size());
        };
        auto width = [&](const std::string &v, bool state) {
            std::string w, n;
            split(v, state, w, n);
            return text_w(valb_f_, w.c_str()) + text_w(val_f_, n.c_str());
        };
        const float sq = kCheck + kToggleGap;
        const std::string cur = it.value();
        float tw = 0;
        if (worst) {
            tw = width(cur, on);
            for (int a = 0; two && a < 2; ++a) {
                tw = std::max(tw, width(it.choices[a], a == 0));
                if (it.reserve) tw = std::max(tw, width(it.choices[a] + hooks_.value_reserve, a == 0));
            }
            if (it.reserve) tw = std::max(tw, width(cur + hooks_.value_reserve, on));
        } else {
            tw = width(cur, on);
        }
        if (dl) {
            const ImVec2 a(x, cy - kCheck / 2), b(x + kCheck, cy + kCheck / 2);
            if (on) dl->AddRectFilled(a, b, sel ? ok : g_heading);
            dl->AddRect(a, b, sel ? ok : on ? g_heading : kMuted, 0, 0, kLine);
            std::string w, n;
            split(cur, on, w, n);
            put_text(dl, vb, x + sq, cy, ink, w.c_str());
            if (!n.empty()) put_text(dl, val_f_, x + sq + text_w(valb_f_, w.c_str()), cy, sel ? ok60 : kMuted, n.c_str());
        }
        return sq + tw;
    }
    if (it.kind == Kind::Page || (it.sub && !it.value)) {
        if (dl) chevron(dl, x + kPageW / 2, cy, 3.5f * kC, 6 * kC, 1.f, sel ? ok : kMuted, 2 * kC);
        return kPageW;
    }
    if (it.kind == Kind::Confirm) {
        const float slot = std::max(text_w(valb_f_, txt::menu_no), text_w(valb_f_, txt::menu_yes));
        if (rr) rr->val0 = x;
        if (dl) put_text(dl, vb, x, cy, ink, it.value().c_str());
        float end = x + slot;
        if (worst || (it.armed && *it.armed)) {
            const std::string t = fmt(txt::menu_confirm, {{"label", it.label}});
            const float bx = x + slot + kBoxGap, bw = 2 * kBoxPadX + text_w(val_f_, t.c_str());
            if (rr) rr->conf0 = bx, rr->conf1 = bx + bw;
            if (dl) {
                const bool lit = d->focus || d->hot == kConfirm;
                box(bx, bx + bw, top, bot, sel ? ok : g_key, lit);
                put_text(dl, val_f_, bx + kBoxPadX, cy, lit ? (sel ? g_key : ok) : ink, t.c_str());
            }
            end = bx + bw;
        }
        return end - x;
    }
    if (it.value) {                         // an old-style row with a value: plain text
        const std::string v = it.value();
        if (rr) rr->val0 = x;
        if (dl) put_text(dl, vb, x, cy, ink, v.c_str());
        return text_w(valb_f_, value_text(v).c_str());
    }
    return 0;
}

// The layout pass (Plan): `counts` says which rows take room. Cards are worked out over every row of the page, the
// hidden too; a card with no counted row takes none. SIZES: the draw passes the shown rows, the measure all of them.
Menu::Plan Menu::make_plan(const Page &p, const std::function<bool(const Item &)> &counts, bool worst, int help_rows) const
{
    Plan pl;
    pl.page = &p;
    pl.rows.resize(p.size());
    // the label column: the widest label of a row with a control, hidden ones too, so nothing moves
    for (const Item &it : p)
        if (has_control(it) && !it.label_fn) pl.label_col = std::max(pl.label_col, text_w(row_f_, it.label.c_str()));
    int ret = -1;
    for (size_t i = 0; i < p.size(); ++i) {
        const Item &it = p[i];
        if (it.kind == Kind::Return) {
            ret = (int)i;
            continue;
        }
        if (pl.cards.empty() || it.gap || it.group) {
            Plan::Card c;
            c.first = (int)i;
            c.titled = it.group != nullptr;
            pl.cards.push_back(c);
        }
        pl.rows[i].card = (int)pl.cards.size() - 1;
        if (counts(it)) {
            pl.rows[i].drawn = true;
            ++pl.cards.back().rows;
            pl.cards.back().drawn = true;
        }
    }
    // the widest content: a row's label and control, a window's row in the value face, a card's title
    auto content = [&](const Item &it) {
        if (it.label_fn || it.kind == Kind::Note) {
            float w = text_w(val_f_, it.label.c_str());
            if (!worst && it.label_fn) w = std::max(w, text_w(val_f_, it.label_fn().c_str()));
            return w;
        }
        if (has_control(it)) return pl.label_col + kColGap + control(it, worst, 0, 0, nullptr, nullptr);
        return text_w(row_f_, it.label.c_str());
    };
    for (size_t i = 0; i < p.size(); ++i)
        if (pl.rows[i].drawn) pl.content_w = std::max(pl.content_w, content(p[i]));
    for (const Plan::Card &c : pl.cards) {
        if (!c.drawn || !c.titled) continue;
        const Item &h = p[c.first];
        float w = text_w(title_f_, upper(h.group).c_str(), kTitleTrack);
        if (h.group_value) w += text_w(title_f_, VR_MID, kTitleTrack) + text_w(title_f_, upper(h.group_value()).c_str(), kTitleTrack);
        pl.content_w = std::max(pl.content_w, w);
    }
    // the y of everything, from the top
    float y = kPanelBorder + kPadTop + kHeadH + kHeadGap;
    bool any = false;
    for (size_t ci = 0; ci < pl.cards.size(); ++ci) {
        Plan::Card &c = pl.cards[ci];
        if (!c.drawn) continue;
        any = true;
        c.y = y;
        float ry = y + kCardBorder + (c.titled ? kTitleH : kCardPadTop);
        for (size_t i = 0; i < p.size(); ++i) {
            Plan::Row &r = pl.rows[i];
            if (r.card != (int)ci || !r.drawn) continue;
            r.y = ry;
            r.h = kRowH;
            ry += kRowH;
        }
        c.h = ry + kCardPadBottom + kCardBorder - y;
        y += c.h + kCardGap;
    }
    if (ret >= 0 && counts(p[ret])) {
        pl.rows[ret] = {y, kReturnH, true, -1};
        y += kReturnH;
    } else if (any) {
        y -= kCardGap;
    }
    bool has_help = false;
    for (const Item &it : p) {
        if (!it.help) continue;
        has_help = true;
        if (help_rows < 0) pl.help_rows = std::max(pl.help_rows, wrap_help(it.help, menu_w_).size());
    }
    if (has_help) {
        if (help_rows >= 0) pl.help_rows = (size_t)help_rows;
        y += kHelpAbove;
        pl.rule_y = y;
        y += kRuleH + kHelpPad;
        pl.help_y = y;
        y += (float)pl.help_rows * kHelpPx * kHelpLead + kHelpPad;
    }
    y += kHintGap;
    pl.hint_y = y;
    y += kHintH + kPadBottom + kPanelBorder;
    pl.total = y;
    for (size_t i = 0; i < p.size(); ++i)
        if (pl.rows[i].drawn && p[i].kind == Kind::Choice)
            pl.drop_bottom = std::max(pl.drop_bottom, pl.rows[i].y + pl.rows[i].h + (float)p[i].choices.size() * kRowH + kPadBottom + kPanelBorder);
    return pl;
}

// HELP's words wrapped to `width` pixels
std::vector<std::string> Menu::wrap_help(const char *text, float width) const
{
    std::vector<std::string> lines;
    std::string cur;
    float space = text_w(help_f_, " "), w = 0;
    const char *p = text;
    while (*p) {
        const char *e = p;
        while (*e && *e != ' ' && *e != '\n') ++e;
        std::string word(p, e);
        float ww = text_w(help_f_, word.c_str());
        if (!cur.empty() && w + space + ww > width) {
            lines.push_back(cur);
            cur.clear();
            w = 0;
        }
        if (!cur.empty()) {
            cur += ' ';
            w += space;
        }
        cur += word;
        w += ww;
        if (*e == '\n') {
            lines.push_back(cur);
            cur.clear();
            w = 0;
        }
        p = *e ? e + 1 : e;
    }
    if (!cur.empty()) lines.push_back(cur);
    return lines;
}

// the header's width: "TOP", and for a sub page a chevron and its name, the version at the right
float Menu::header_width(const std::string &page) const
{
    float w = text_w(head_f_, upper(top_title_).c_str(), kHeadTrack);
    if (!page.empty()) w += 2 * kSepGap + kSepW + text_w(head_f_, page.c_str(), kHeadTrack);
    if (!hooks_.version.empty()) w += kVersionGap + text_w(small_f_, fmt(txt::menu_version, {{"version", hooks_.version}}).c_str());
    return w;
}

// SIZES (menu.h): the widest any page reached from the top can be, every row shown, a `reserve` row's value with
// the front's value_reserve after it: the panel's inner width, between its paddings
float Menu::widest() const
{
    float w = std::max(hint_w(small_f_, kKeysLine.c_str()), hint_w(small_f_, (kPointerHead + VR_MID + kPointerTail).c_str()));
    std::vector<std::pair<const Page *, std::string>> todo = {{top_, std::string()}};
    std::vector<const Page *> seen;
    const auto all = [](const Item &) { return true; };
    while (!todo.empty()) {
        auto [p, name] = todo.back();
        todo.pop_back();
        if (!p || std::find(seen.begin(), seen.end(), p) != seen.end()) continue;
        seen.push_back(p);
        w = std::max(w, header_width(name));
        w = std::max(w, make_plan(*p, all, true).content_w + 2 * kRowPadX + 2 * (kCardBorder + kCardPadX));
        for (const Item &it : *p)
            if (it.sub) todo.push_back({it.sub, upper(it.label)});
    }
    return w;
}

// The header line, between the panel's paddings `in0` and `in1`: the top title upper case in the heading colour, for
// a sub page a chevron and its name in white, the version small and muted at the right. `page` empty: no page part.
void Menu::draw_header(ImDrawList *dl, float in0, float in1, const std::string &page) const
{
    const float y0 = kPanelBorder + kPadTop, cy = y0 + kHeadH / 2;
    float hx = in0;
    const std::string top = upper(top_title_);
    put_text(dl, head_f_, hx, cy, g_heading, top.c_str(), kHeadTrack);
    hx += text_w(head_f_, top.c_str(), kHeadTrack);
    if (!page.empty()) {
        hx += kSepGap;
        chevron(dl, hx + kSepW / 2, cy, kSepW / 2, kSepH / 2, 1.f, g_heading, 2 * kC);
        hx += kSepW + kSepGap;
        put_text(dl, head_f_, hx, cy, kWhite, page.c_str(), kHeadTrack);
    }
    if (!hooks_.version.empty()) {
        const std::string v = fmt(txt::menu_version, {{"version", hooks_.version}});
        put_text(dl, small_f_, in1 - text_w(small_f_, v.c_str()), y0 + kHeadH - line_h(small_f_) / 2 - 2 * kC, kMuted, v.c_str());
    }
}

// THE MESSAGE PANEL: the menu's ground, border and header, then the lines, each a row high: kind \x01 the picker's
// row as a selected row (the key bar across the inner width), \x02 in the note colour, else a plain row.
// message_row gives line i's rectangle (x0..x1 across the inner width of a panel w wide, y down, kRowH high):
// the drawing and the pointer's hit test both read it.
static void message_row(size_t i, float w, float &x0, float &x1, float &y)
{
    x0 = kPanelBorder + kPadSide;
    x1 = w - kPanelBorder - kPadSide;
    y = kPanelBorder + kPadTop + kHeadH + kHeadGap + (float)i * kRowH;
}

void Menu::draw_message(ImDrawList *dl, float ox, const std::vector<std::pair<std::string, char>> &lines, float w, float h)
{
    dl->AddRectFilled(ImVec2(ox, 0), ImVec2(ox + w, h), kBack);
    dl->AddRect(ImVec2(ox + kPanelBorder / 2, kPanelBorder / 2), ImVec2(ox + w - kPanelBorder / 2, h - kPanelBorder / 2), kBorder, 0, 0, kPanelBorder);
    const float in0 = ox + kPanelBorder + kPadSide, in1 = ox + w - kPanelBorder - kPadSide;
    draw_header(dl, in0, in1, std::string());
    for (size_t i = 0; i < lines.size(); ++i) {
        const auto &l = lines[i];
        float x0, x1, y;
        message_row(i, w, x0, x1, y);
        if (l.second == '\x01') dl->AddRectFilled(ImVec2(in0, y), ImVec2(in1, y + kRowH), g_key);
        const ImU32 c = l.second == '\x01' ? on_key(g_key_rgb) : l.second == '\x02' ? kNote : kRow;
        put_text(dl, row_f_, in0 + kRowPadX, y + kRowH / 2, c, l.first.c_str());
    }
}

void Menu::draw_panel(ImDrawList *dl, float ox, const Plan &pl, float w, float h)
{
    const Page &pg = *pl.page;
    const ImU32 ok = on_key(g_key_rgb);
    dl->AddRectFilled(ImVec2(ox, 0), ImVec2(ox + w, h), kBack);
    dl->AddRect(ImVec2(ox + kPanelBorder / 2, kPanelBorder / 2), ImVec2(ox + w - kPanelBorder / 2, h - kPanelBorder / 2), kBorder, 0, 0, kPanelBorder);
    const float in0 = ox + kPanelBorder + kPadSide, in1 = ox + w - kPanelBorder - kPadSide;
    const float bar0 = in0 + kCardBorder + kCardPadX, bar1 = in1 - kCardBorder - kCardPadX;
    const float label_x = bar0 + kRowPadX, ctl_x = label_x + pl.label_col + kColGap;

    draw_header(dl, in0, in1, page_title_);

    // the cards
    for (size_t ci = 0; ci < pl.cards.size(); ++ci) {
        const Plan::Card &c = pl.cards[ci];
        if (!c.drawn) continue;
        dl->AddRectFilled(ImVec2(in0, c.y), ImVec2(in1, c.y + c.h), kCard);
        dl->AddRect(ImVec2(in0 + kCardBorder / 2, c.y + kCardBorder / 2), ImVec2(in1 - kCardBorder / 2, c.y + c.h - kCardBorder / 2), kBorder, 0, 0, kCardBorder);
        if (!c.titled) continue;
        const Item &head = pg[c.first];
        const float cy = c.y + kCardBorder + kTitleH / 2;
        float tx = label_x;
        const std::string t = upper(head.group);
        put_text(dl, title_f_, tx, cy, kMuted, t.c_str(), kTitleTrack);
        tx += text_w(title_f_, t.c_str(), kTitleTrack);
        if (head.group_value) {
            put_text(dl, title_f_, tx, cy, kMuted, VR_MID, kTitleTrack);
            tx += text_w(title_f_, VR_MID, kTitleTrack);
            put_text(dl, title_f_, tx, cy, g_heading, upper(head.group_value()).c_str(), kTitleTrack);
        }
    }

    // the rows, in the visible order (the index the selection and the pointer use)
    int vi = 0;
    for (size_t i = 0; i < pg.size(); ++i) {
        const Plan::Row &r = pl.rows[i];
        if (!r.drawn) continue;
        const Item &it = pg[i];
        const bool selected = vi == sel_, hovered = vi == hover_row_ && hit_id_ == 0;
        const float cy = r.y + r.h / 2;
        ++vi;
        if (it.kind == Kind::Return) {
            if (selected) dl->AddRectFilled(ImVec2(in0, r.y), ImVec2(in1, r.y + r.h), g_key);
            else if (hovered) dl->AddRectFilled(ImVec2(in0, r.y), ImVec2(in1, r.y + r.h), g_glow);
            const float cx = in0 + kReturnPad;
            chevron(dl, cx + 3 * kC, cy, 3 * kC, 5 * kC, -1.f, selected ? ok : kMuted, 2 * kC);
            put_text(dl, ret_f_, cx + 14 * kC, cy, selected ? ok : kWhite, it.label.c_str());
            continue;
        }
        if (selected) dl->AddRectFilled(ImVec2(bar0, r.y), ImVec2(bar1, r.y + r.h), g_key);
        else if (hovered) dl->AddRectFilled(ImVec2(bar0, r.y), ImVec2(bar1, r.y + r.h), g_glow);
        const ImU32 col = it.kind == Kind::Note ? kNote : selected ? ok : kRow;
        if (it.label_fn) put_text(dl, val_f_, label_x, cy, col, it.label_fn().c_str());
        else put_text(dl, row_f_, label_x, cy, col, it.label.c_str());
        if (has_control(it)) {
            Ctl d{dl, selected, dropdown_ == vi - 1, button_focus_ && selected, hovered ? hover_zone_ : kNoZone};
            control(it, false, ctl_x, cy, &d, nullptr);
        }
    }

    // HELP and the HINT
    if (pl.rule_y >= 0) {
        dl->AddRectFilled(ImVec2(in0, pl.rule_y), ImVec2(in1, pl.rule_y + kRuleH), kBorder);
        if (sel_ >= 0 && sel_ < (int)visible_.size() && visible_[sel_]->help) {
            const std::vector<std::string> help = wrap_help(visible_[sel_]->help, menu_w_);
            for (size_t i = 0; i < help.size(); ++i)
                put_text(dl, help_f_, in0, pl.help_y + ((float)i + 0.5f) * kHelpPx * kHelpLead, kHelp, help[i].c_str());
        }
    }
    const std::string hint = pointer_hint_
        ? kPointerHead + (pointer_buttons_.empty() ? "" : VR_MID + pointer_buttons_) : kKeysLine;
    put_hint(dl, small_f_, in0, pl.hint_y + kHintH / 2, hint.c_str(), kMuted);

    // THE ROW KINDS: the open dropdown, under its box, over what lies under it; the current choice marked, the one
    // under the ray lit
    if (dropdown_ >= 0 && dropdown_ < (int)visible_.size() && drop_x1_ > drop_x0_) {
        const Item &it = *visible_[dropdown_];
        const int cur = it.index ? it.index() : -1;
        const float b = kDropBorder;
        dl->AddRectFilled(ImVec2(ox + drop_x0_, drop_y0_), ImVec2(ox + drop_x1_, drop_y1_), kCard);
        dl->AddRect(ImVec2(ox + drop_x0_ + b / 2, drop_y0_ + b / 2), ImVec2(ox + drop_x1_ - b / 2, drop_y1_ - b / 2), g_key, 0, 0, b);
        for (size_t i = 0; i < it.choices.size(); ++i) {
            const float ty = drop_y0_ + (float)i * kRowH;
            if ((int)i == drop_hover_)      // inside the border
                dl->AddRectFilled(ImVec2(ox + drop_x0_ + b, std::max(ty, drop_y0_ + b)),
                                  ImVec2(ox + drop_x1_ - b, std::min(ty + kRowH, drop_y1_ - b)), kBorder);
            put_text(dl, (int)i == cur ? valb_f_ : val_f_, ox + drop_x0_ + kBoxPadX, ty + kRowH / 2, kWhite, it.choices[i].c_str());
            if ((int)i == cur)
                dl->AddRectFilled(ImVec2(ox + drop_x1_ - kBoxPadX - 5 * kC, ty + kRowH / 2 - 2.5f * kC),
                                  ImVec2(ox + drop_x1_ - kBoxPadX, ty + kRowH / 2 + 2.5f * kC), g_heading);
        }
    }
}

// A click where the ray is would do something: a dropdown's choice, a row that is not a stepper or a strip, or a
// stepper's half or Reset, or a segment. The cursor dot and the ray take their key colour from it.
bool Menu::pointing_at_control() const
{
    if (showing_message_) return msg_hover_;
    if (drop_hover_ >= 0) return true;
    if (hover_row_ < 0 || hover_row_ >= (int)visible_.size()) return false;
    const Kind k = visible_[hover_row_]->kind;
    return (k != Kind::Stepper && k != Kind::Segmented) || hover_zone_ != kNoZone;
}

// The pointing hand's ray against the quads prepare hung (the nearest one in front of it), into that layer's
// pixels; the panel's row under it; a light tick when the row changes. `click`: the trigger went down.
void Menu::point(const Input &in, bool &click)
{
    pointer_hint_ = in.controller_shown(Input::kLeft) || in.controller_shown(Input::kRight);
    // the hint names B where either hand's profile binds it, and the menu button where the left's does
    bool sec[2], menu[2];
    for (int h = 0; h < 2; ++h) controller_buttons(controller_profile(h), sec[h], menu[h]);
    const bool b = sec[0] || sec[1], m = menu[Input::kLeft];
    pointer_buttons_ = b && m ? kPointerTail : b ? kPointerBack : m ? kPointerMenu : "";
    int was = hover_row_;
    const bool was_msg = msg_hover_;
    msg_hover_ = false;
    hit_id_ = hover_row_ = -1;
    hit_t_ = 0;
    click = false;
    if (pointing_ < 0) return;
    const Pose &a = in.hands[pointing_].aim;
    static const float kForward[3] = {0, 0, -1};
    float d[3], best = 1e9f;
    quat_rotate(a.rot, kForward, d);
    for (const Quad &q : quads_) {
        // the quad's right (cos, 0, -sin) and normal (sin, 0, cos), as panel_point turns them
        float c = std::cos(q.yaw), s = std::sin(q.yaw);
        float den = s * d[0] + c * d[2];
        if (den >= -1e-4f) continue;                // edge-on, or the ray comes from behind it
        float ox = q.c[0] - a.pos[0], oz = q.c[2] - a.pos[2];
        float t = (s * ox + c * oz) / den;
        if (t <= 0 || t >= best) continue;
        float px = a.pos[0] + d[0] * t - q.c[0], py = a.pos[1] + d[1] * t - q.c[1], pz = a.pos[2] + d[2] * t - q.c[2];
        float u = c * px - s * pz, v = py;
        if (std::fabs(u) > q.w / 2 || std::fabs(v) > q.h / 2) continue;
        best = t;
        hit_id_ = q.id;
        hit_t_ = t;
        hit_x_ = (u + q.w / 2) * kPxPerM;
        hit_y_ = (q.h / 2 - v) * kPxPerM;
    }
    if (hit_id_ == 0) {
        for (size_t i = 0; i < rows_.size() && i < visible_.size(); ++i)
            // a Note is text: the ray passes over it
            if (i < visible_.size() && visible_[i]->kind != Kind::Note && hit_y_ >= rows_[i].y &&
                hit_y_ < rows_[i].y + rows_[i].h)
                hover_row_ = (int)i;
    }
    click = trigger_pressed_[pointing_];
    if (showing_message_) {                         // the message panel: only the \x01 rows are under the pointer
        for (size_t i = 0; hit_id_ == 0 && i < msg_kinds_.size(); ++i) {
            float x0, x1, y;
            message_row(i, (float)panel_.w, x0, x1, y);
            if (msg_kinds_[i] == '\x01' && hit_x_ >= x0 && hit_x_ < x1 && hit_y_ >= y && hit_y_ < y + kRowH) msg_hover_ = true;
        }
        if (msg_hover_ && !was_msg) haptic(pointing_, 0.15f, 0.02f);
        return;
    }
    // an open dropdown takes the ray: the rows under it are not hovered, and the selection stays
    const int was_drop = drop_hover_;
    drop_hover_ = -1;
    if (dropdown_ >= 0 && dropdown_ < (int)visible_.size()) {
        if (hit_id_ == 0 && hit_x_ >= drop_x0_ && hit_x_ < drop_x1_ && hit_y_ >= drop_y0_ && hit_y_ < drop_y1_)
            drop_hover_ = (int)((hit_y_ - drop_y0_) / kRowH);
        hover_row_ = -1;
        hover_zone_ = kNoZone;
        if (drop_hover_ >= 0 && drop_hover_ != was_drop) haptic(pointing_, 0.15f, 0.02f);
        return;
    }
    // the part of the row under the ray: a stepper's halves, its Reset, a Confirm's button
    hover_zone_ = kNoZone;
    if (hover_row_ >= 0 && hover_row_ < (int)rows_.size()) {
        const RowRect &r = rows_[hover_row_];
        const Item &it = *visible_[hover_row_];
        if (it.kind == Kind::Stepper && r.dec0 >= 0 && hit_x_ >= r.dec0 && hit_x_ < r.inc1)
            hover_zone_ = hit_x_ < (r.dec0 + r.inc1) / 2 ? kDec : kInc;
        if (it.kind == Kind::Stepper && r.reset0 >= 0 && hit_x_ >= r.reset0 && hit_x_ < r.reset1) hover_zone_ = kReset;
        if (it.kind == Kind::Segmented)
            for (size_t i = 0; i + 1 < r.seg.size(); ++i)
                if (hit_x_ >= r.seg[i] && hit_x_ < r.seg[i + 1]) hover_zone_ = kSeg0 + (int)i;
        if (it.kind == Kind::Confirm && r.conf0 >= 0 && hit_x_ >= r.conf0 && hit_x_ < r.conf1) hover_zone_ = kConfirm;
    }
    if (hover_row_ >= 0 && hover_row_ != was) {
        sel_ = hover_row_;
        haptic(pointing_, 0.15f, 0.02f);
    }
}

void Menu::prepare(gpu::Frame &f)
{
    bool up = (open_ || showing_message_) && ctx_ && render_ && panel_.layer;
    // the model each hand's profile calls for (input.h: "" in the sim, so the generic pair there), asked only while
    // the panel is up and they can be seen: a profile change in the game (hands and back) loads nothing mid-race
    if (gpu_ && models_.ready()) {
        if (up)
            for (int h = 0; h < 2; ++h) models_.set_profile(*gpu_, h, controller_profile(h));
        models_.tick(*gpu_);
    }
    auto hide = [](Drawn &d) {
        static const float kNowhere[3] = {0, 0, 0};
        if (d.layer) layer_place(d.layer, kNowhere, Quat{}, 1, 1, false);
    };
    bool noting = !note_text_.empty() && ctx_ && render_ && note_.layer;
    if (!up) {
        quads_.clear();
        hit_id_ = hover_row_ = -1;
        msg_hover_ = false;
        hide(panel_);
        hide(legend_);
        for (Drawn &d : box_layers_) hide(d);
        if (!noting) {
            hide(note_);
            return;
        }
    }
    ImGui::SetCurrentContext(static_cast<ImGuiContext *>(ctx_));
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2(kCanvas * (3 + box_layers_.size()), kCanvas);     // panel, legend, note, boxes
    io.DeltaTime = 1.f / 120.f;
    ImGui::NewFrame();
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    auto begin = [&](Drawn &d, const char *name, uint32_t w, uint32_t h) {
        d.w = std::min(w, d.max_w);
        d.h = std::min(h, d.max_h);
        ImGui::SetNextWindowPos(ImVec2(d.x, 0));
        ImGui::SetNextWindowSize(ImVec2((float)d.max_w, (float)d.max_h));
        ImGui::Begin(name, nullptr, flags);
        d.list = ImGui::GetWindowDrawList();
        return d.list;
    };

    if (up) {
        // the panel
        // SIZES: the menu's width once an opening, a page's height on entering it; both only grow after (a value
        // longer than its reserve), so nothing moves while you change a row
        Plan plan;
        std::vector<std::pair<std::string, char>> msg;
        float pw_f = 0, ph_f = 0;
        if (showing_message_) {
            std::string text = hooks_.message ? hooks_.message() : std::string();
            std::string pick = hooks_.pick ? hooks_.pick(false) : std::string();
            if (!pick.empty()) text += "\n\n" + pick;
            for (size_t a = 0;;) {
                size_t b = text.find('\n', a);
                std::string l = text.substr(a, b == std::string::npos ? std::string::npos : b - a);
                char kind = 0;
                if (!l.empty() && (l[0] == '\x01' || l[0] == '\x02')) {
                    kind = l[0];
                    l.erase(0, 1);
                }
                pw_f = std::max(pw_f, text_w(row_f_, l.c_str()) + 2 * kRowPadX);
                msg.push_back({l, kind});
                if (b == std::string::npos) break;
                a = b + 1;
            }
            msg_kinds_.clear();
            for (const auto &l : msg) msg_kinds_.push_back(l.second);
            pw_f = std::max(pw_f, header_width(std::string()));
            pw_f += 2 * (kPanelBorder + kPadSide);
            ph_f = kPanelBorder + kPadTop + kHeadH + kHeadGap + (float)msg.size() * kRowH + kPadBottom + kPanelBorder;
            msg_w_ = pw_f = std::max(msg_w_, pw_f);     // only grows while it shows (SIZES)
            msg_h_ = ph_f = std::max(msg_h_, ph_f);
        } else {
            if (menu_w_ == 0) menu_w_ = widest();
            if (sized_page_ != page_) {
                sized_page_ = page_;
                const Plan all = make_plan(*page_, [](const Item &) { return true; }, true);
                page_h_ = std::max(all.total, all.drop_bottom);
                help_rows_ = (int)all.help_rows;
            }
            plan = make_plan(*page_, [this](const Item &it) { return std::find(visible_.begin(), visible_.end(), &it) != visible_.end(); }, false, help_rows_);
            menu_w_ = std::max(menu_w_, plan.content_w + 2 * kRowPadX + 2 * (kCardBorder + kCardPadX));
            page_h_ = std::max({page_h_, plan.total, plan.drop_bottom});
            // HELP and the HINT sit at the panel's bottom, not right under the shown rows
            if (const float lift = page_h_ - plan.total; lift > 0) {
                if (plan.rule_y >= 0) plan.rule_y += lift;
                plan.help_y += lift;
                plan.hint_y += lift;
            }
            pw_f = menu_w_ + 2 * (kPanelBorder + kPadSide);
            ph_f = page_h_;
            if (ph_f > (float)panel_.max_h && cropped_page_ != page_) {
                cropped_page_ = page_;
                log("ui: a page needs %.0f px, the layer has %u: the picture is cropped", ph_f, panel_.max_h);
            }
        }
        if (sized_page_ && showing_message_) sized_page_ = nullptr;
        const uint32_t pw = (uint32_t)std::ceil(pw_f), ph = (uint32_t)std::ceil(ph_f);
        rows_.clear();
        if (!showing_message_) {
            const Page &pg = *page_;
            const float label_x = kPanelBorder + kPadSide + kCardBorder + kCardPadX + kRowPadX;
            for (size_t i = 0; i < pg.size(); ++i) {
                const Plan::Row &pr = plan.rows[i];
                if (!pr.drawn) continue;
                RowRect r;
                r.y = pr.y;
                r.h = pr.h;
                if (has_control(pg[i])) control(pg[i], false, label_x + plan.label_col + kColGap, pr.y + pr.h / 2, nullptr, &r);
                rows_.push_back(r);
            }
        }
        // the open dropdown hangs under its choice's box (THE ROW KINDS), as wide as the box
        drop_x1_ = drop_x0_ = 0;
        if (dropdown_ >= 0 && dropdown_ < (int)rows_.size() && dropdown_ < (int)visible_.size() &&
            visible_[dropdown_]->kind == Kind::Choice && !visible_[dropdown_]->choices.empty()) {
            const Item &it = *visible_[dropdown_];
            drop_x0_ = rows_[dropdown_].val0;
            drop_x1_ = rows_[dropdown_].val1;
            drop_y0_ = rows_[dropdown_].y + rows_[dropdown_].h;
            drop_y1_ = drop_y0_ + (float)it.choices.size() * kRowH;
        }
        quads_.clear();
        auto cursor = [&](ImDrawList *l, const Drawn &d, int id) {
            if (hit_id_ != id) return;
            ImVec2 c(d.x + hit_x_, hit_y_);
            l->AddCircleFilled(c, kDotPx, pointing_at_control() ? g_key : kDot, 16);
            l->AddCircle(c, kDotPx, kBack, 16, 2 * kC);     // a dark ring: the key-coloured dot on the key bar
        };
        ImDrawList *dl = begin(panel_, "##panel", pw, ph);
        if (showing_message_) draw_message(dl, panel_.x, msg, (float)panel_.w, (float)panel_.h);
        else draw_panel(dl, panel_.x, plan, (float)panel_.w, (float)panel_.h);
        cursor(dl, panel_, 0);
        ImGui::End();

        // the CONTROLS box: the panel's ground and border, a letter-spaced heading, then two columns (the buttons
        // white in the label face, the actions in the value face) a row high, a rule between the rows
        legend_.w = legend_.h = 0;
        const Legend *lg = hooks_.legend && !showing_message_ ? hooks_.legend() : nullptr;
        if (lg && legend_.layer) {
            legend_shown_ = lg;
            const char *title = txt::menu_controls;
            float kw = 0, aw = 0;
            for (const auto &r : *lg) {
                kw = std::max(kw, text_w(row_f_, r.first.c_str()));
                aw = std::max(aw, text_w(val_f_, r.second.c_str()));
            }
            const float inner = std::max(text_w(head_f_, title, kTitleTrack), kw + kLegendColGap + aw);
            const float bw = inner + 2 * (kPanelBorder + kPadSide);
            const float bh = kPanelBorder + kPadTop + kHeadH + kHeadGap + (float)lg->size() * kRowH + kPadBottom + kPanelBorder;
            if ((bw > (float)legend_.max_w || bh > (float)legend_.max_h) && legend_cropped_ != lg) {
                legend_cropped_ = lg;
                log("ui: the CONTROLS box needs %.0f x %.0f px, the layer has %u x %u: the picture is cropped", bw, bh,
                    legend_.max_w, legend_.max_h);
            }
            dl = begin(legend_, "##legend", (uint32_t)std::ceil(bw), (uint32_t)std::ceil(bh));
            const float ox = legend_.x, w = (float)legend_.w, h = (float)legend_.h;
            dl->AddRectFilled(ImVec2(ox, 0), ImVec2(ox + w, h), kBack);
            dl->AddRect(ImVec2(ox + kPanelBorder / 2, kPanelBorder / 2), ImVec2(ox + w - kPanelBorder / 2, h - kPanelBorder / 2), kBorder, 0, 0, kPanelBorder);
            const float in0 = ox + kPanelBorder + kPadSide, in1 = ox + w - kPanelBorder - kPadSide;
            put_text(dl, head_f_, in0, kPanelBorder + kPadTop + kHeadH / 2, g_heading, title, kTitleTrack);
            float y = kPanelBorder + kPadTop + kHeadH + kHeadGap;
            for (size_t i = 0; i < lg->size(); ++i) {
                if (i > 0) dl->AddRectFilled(ImVec2(in0, y - kLegendRule / 2), ImVec2(in1, y + kLegendRule / 2), kLegendRuleCol);
                put_text(dl, row_f_, in0, y + kRowH / 2, kWhite, (*lg)[i].first.c_str());
                put_text(dl, val_f_, in0 + kw + kLegendColGap, y + kRowH / 2, kAction, (*lg)[i].second.c_str());
                y += kRowH;
            }
            cursor(dl, legend_, 1);
            ImGui::End();
        }

        // the front's boxes (not on the message panel)
        for (size_t i = 0; i < boxes_.size(); ++i) {
            Drawn &d = box_layers_[i];
            d.w = d.h = 0;
            float wm = 0, hm = 0;
            if (showing_message_ || !d.layer || !boxes_[i].measure || !boxes_[i].measure(wm, hm)) continue;
            char name[16];
            std::snprintf(name, sizeof name, "##box%zu", i);
            dl = begin(d, name, (uint32_t)std::ceil(wm * kPxPerM + 2 * kMargin), (uint32_t)std::ceil(hm * kPxPerM + 2 * kMargin));
            dl->AddRectFilled(ImVec2(d.x, 0), ImVec2(d.x + d.w, (float)d.h), kBack);
            dl->AddRect(ImVec2(d.x + kPanelBorder / 2, kPanelBorder / 2), ImVec2(d.x + (float)d.w - kPanelBorder / 2, (float)d.h - kPanelBorder / 2), kBorder, 0, 0, kPanelBorder);
            if (boxes_[i].draw) boxes_[i].draw(dl, d.x + kMargin, kMargin, kPxPerM);
            cursor(dl, d, 2 + (int)i);
            ImGui::End();
        }
    }

    // the note (the front's FPS counter): one line on the box's background, hung where the front says
    note_.w = note_.h = 0;
    if (noting) {
        float tw = font_->CalcTextSizeA(kFontPx, 1e9f, 0, note_text_.c_str()).x;
        ImDrawList *dl = begin(note_, "##note", (uint32_t)std::ceil(tw + kMargin), (uint32_t)std::ceil(kPitch));
        dl->AddRectFilled(ImVec2(note_.x, 0), ImVec2(note_.x + note_.w, (float)note_.h), kBack);
        dl->AddText(font_, kFontPx, ImVec2(note_.x + kMargin * 0.5f, (kPitch - kFontPx) * 0.5f), note_colour_, note_text_.c_str());
        ImGui::End();
    }
    ImGui::Render();
    render_->upload(f, ImGui::GetDrawData());

    // into the layers, then placed: the panel at pos_, each box hinged on its side
    static const float kClear[4] = {0, 0, 0, 0};
    auto paint = [&](Drawn &d) {
        if (!d.layer || !d.w || !d.h) return false;
        gpu::Target *t = layer_target(f, d.layer);
        if (!t) return false;
        f.offscreen(t, kClear);
        render_->draw_list(f, d.list, d.x, 0, d.max_w, d.max_h);
        f.end_offscreen();
        layer_crop(d.layer, d.w, d.h);
        return true;
    };
    if (noting && paint(note_)) {
        float k = note_height_ / ((float)note_.h / kPxPerM);
        layer_place(note_.layer, note_pos_, note_rot_, note_.w / kPxPerM * k, note_height_, true);
    } else {
        hide(note_);
    }
    note_text_.clear();                     // given again every frame it shows
    if (!up) return;
    // the controllers and the ray on the overlay, in front of the quads. The ray ends where it lands, or runs
    // kRayMiss; in the key colour where a click does something (pointing_at_control), as the cursor dot is.
    if (models_.ready()) {
        ControllerModels::Draw draws[2] = {ctl_[0], ctl_[1]};
        bool any = false;
        for (ControllerModels::Draw &c : draws) {
            c.ray_length = hit_id_ >= 0 && hit_t_ > 0 ? hit_t_ : ControllerModels::kRayMiss;
            c.clickable = pointing_at_control();
            any |= c.shown;
        }
        if (any) overlay_draw([this, draws](gpu::Frame &fr) { models_.draw(fr, draws); });
    }
    Quat rot = yaw_quat(yaw_);
    float panel_w = panel_.w / kPxPerM;
    if (paint(panel_)) {
        layer_place(panel_.layer, pos_, rot, panel_w, panel_.h / kPxPerM, true);
        quads_.push_back({{pos_[0], pos_[1], pos_[2]}, yaw_, panel_w, panel_.h / kPxPerM, 0});
    } else {
        hide(panel_);
    }
    // Each side's row starts kHinge off the panel's edge and curves round you: every stretch along it (a box, or a
    // box's gap) turns kTurn more than the last, the far edge coming towards you, so the first box stands at kTurn
    // and the row keeps its distance (Box). Per side: the row's end in the panel's frame (out from the edge, towards
    // you) and its turn so far.
    struct Row {
        float out, in, turn;
    } row[2] = {{panel_w / 2 + kHinge, 0, 0}, {panel_w / 2 + kHinge, 0, 0}};
    // the next stretch, `len` metres: its middle in the room, its yaw; the row's end moves past it
    auto stretch = [&](bool left, float len, float mid[3], float &yaw) {
        Row &r = row[left ? 0 : 1];
        r.turn += kTurn;
        float c = std::cos(r.turn), s = std::sin(r.turn), side = left ? -1.f : 1.f;
        panel_point(pos_, yaw_, side * (r.out + c * len / 2), 0, r.in + s * len / 2, mid);
        yaw = yaw_ + (left ? r.turn : -r.turn);
        r.out += c * len;
        r.in += s * len;
    };
    auto hinge = [&](Drawn &d, int id) {
        if (!paint(d)) {
            hide(d);
            return;
        }
        float bw = d.w / kPxPerM, c[3], yaw;
        stretch(d.left, bw, c, yaw);
        layer_place(d.layer, c, yaw_quat(yaw), bw, d.h / kPxPerM, true);
        quads_.push_back({{c[0], c[1], c[2]}, yaw, bw, d.h / kPxPerM, id});
        Row &r = row[d.left ? 0 : 1];
        r.out += std::cos(r.turn) * kHinge;     // the next one kHinge on
        r.in += std::sin(r.turn) * kHinge;
    };
    if (legend_.w) hinge(legend_, 1);
    else hide(legend_);
    for (size_t i = 0; i < box_layers_.size(); ++i) {
        Drawn &d = box_layers_[i];
        const Box &b = boxes_[i];
        if (b.gap > 0) {
            float mid[3], yaw;
            stretch(b.left, b.gap, mid, yaw);
            if (b.placed) b.placed(mid, yaw, pos_, yaw_);
        } else if (b.placed) {
            float mid[3];
            const Row &r = row[b.left ? 0 : 1];
            panel_point(pos_, yaw_, (b.left ? -1.f : 1.f) * r.out, 0, r.in, mid);
            b.placed(mid, yaw_ + (b.left ? r.turn : -r.turn), pos_, yaw_);
        }
        if (d.w) hinge(d, 2 + (int)i);
        else hide(d);
    }
}

}

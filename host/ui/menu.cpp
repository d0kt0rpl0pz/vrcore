#include "menu.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

#include <SDL3/SDL.h>
#include <imgui.h>

#include "imgui_render.h"
#include "layers.h"
#include "log.h"

namespace vrcore::ui {

namespace {

// THE PICTURE (menu.h): the base font's size, the help's (three quarters), the version's (a half); the line
// pitch for each, all at kSize (the panel was too wide at 1); the box's margin.
constexpr float kSize = 40.f / 48.f;
constexpr float kFontPx = 48.f * kSize, kHelpPx = 36.f * kSize, kSmallPx = 24.f * kSize;
constexpr float kPitch = 0.0526f * kSize * Menu::kPxPerM;   // pixels a line
constexpr float kMargin = 0.05f * Menu::kPxPerM;            // the panel's margin
constexpr float kLegendGap = 0.06f * Menu::kPxPerM;         // between the legend's two columns
constexpr float kHinge = 0.03f;                             // metres off the panel's edge
constexpr float kTurn = 30.f * 3.14159265f / 180.f;         // the boxes turned in towards you
constexpr float kAhead = 1.1f, kBelow = 0.05f;
constexpr float kCanvas = 4096.f;                           // each box's own stretch of ImGui's canvas

// The menu's colours (sRGB): the text, the selected line, a value, the help and version, the box
const ImU32 kText = IM_COL32(140, 255, 204, 255);           // (0.55, 1, 0.8)
const ImU32 kSelected = IM_COL32(255, 255, 0, 255);
const ImU32 kWhite = IM_COL32(255, 255, 255, 255);
const ImU32 kHelp = IM_COL32(0x7f, 0xa8, 0x9a, 255);
const ImU32 kNote = IM_COL32(0xff, 0x90, 0x80, 255);         // why a pick did not work
const ImU32 kBack = IM_COL32(3, 3, 10, 245);                // (0.01, 0.01, 0.04, 0.96)

// on two lines: the one line set most pages' width
constexpr const char *kControlsLine[2] = {"move: stick   change: left/right or A", "back: B   close: menu"};

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

struct Menu::Line {
    struct Seg {
        std::string text;
        ImU32 col;
        bool bold;
        float x = -1;                       // >= 0: starts at this column (the values'), past the line's start
    };
    std::vector<Seg> segs;
    ImFont *font = nullptr;
    float size = kFontPx;
    float pitch = kPitch;
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
    ImFontConfig cfg;
    cfg.SizePixels = kFontPx;
    font_ = io.Fonts->AddFontDefaultVector(&cfg);
    cfg.SizePixels = kHelpPx;
    help_font_ = io.Fonts->AddFontDefaultVector(&cfg);
    cfg.SizePixels = kSmallPx;
    small_font_ = io.Fonts->AddFontDefaultVector(&cfg);
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
    // the panel: Controls' page, the tallest, is ~27 lines (1.3 m at kSize)
    // the note first: layers are composited in the order they are made, so the menu's boxes cover it
    bool ok = make(note_, 1024, 64, kCanvas * 2, false);
    ok &= make(panel_, 2048, 1800, 0, false);
    if (hooks_.legend) ok &= make(legend_, 1280, 1024, kCanvas, false);
    box_layers_.resize(boxes_.size());
    for (size_t i = 0; i < boxes_.size(); ++i)
        ok &= make(box_layers_[i], boxes_[i].max_width, boxes_[i].max_height, kCanvas * (3 + i), boxes_[i].left);
    log("ui: menu %s, %.0f px a metre, layers %s", ok ? "ready" : "NOT ready", kPxPerM, ok ? "made" : "missing");
    return ok;
}

void Menu::shutdown(gpu::Renderer &r)
{
    layer_destroy(r, panel_.layer);
    layer_destroy(r, legend_.layer);
    layer_destroy(r, note_.layer);
    for (Drawn &d : box_layers_) layer_destroy(r, d.layer);
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
}

void Menu::set_top(Page *top, const char *title)
{
    top_ = page_ = top;
    top_title_ = page_title_ = title;
}

// ---- the pages ----

void Menu::enter(Item &it)
{
    stack_.push_back({page_, page_title_, sel_});
    page_ = it.sub;
    std::string up = it.label;
    std::transform(up.begin(), up.end(), up.begin(), [](unsigned char c) { return (char)std::toupper(c); });
    page_title_ = top_title_ + " > " + up;
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
    stack_.clear();
    page_ = top_;
    page_title_ = top_title_;
    restart_yes_ = false;
    sel_ = 0;
    repeat_at_ = now_ + 0.3;
    placed_ = false;                        // placed in front of the head this update
    menu_w_ = 0;                            // measured again (SIZES)
    sized_page_ = nullptr;
}

void Menu::close()
{
    if (!open_) return;
    open_ = false;
    if (hooks_.on_close) hooks_.on_close();
}

Item Menu::restart_item(std::function<void()> saved, bool gap)
{
    Item it;
    it.label = "Restart game";
    it.value = [this] { return std::string(restart_yes_ ? "Yes" : "No"); };
    it.change = [this](int) { restart_yes_ = !restart_yes_; };
    it.press = [this, saved] {
        if (!restart_yes_) return;
        close();
        if (saved) saved();
        log("ui: restart game");
        request_restart();
    };
    it.gap = gap;
    it.help = "Starts the game again from power-on, with the Gameplay settings applied. Set it to Yes, then press A.";
    return it;
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

void Menu::update(const Tick &t, const Input &in)
{
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
        if (hooks_.pick) hooks_.pick(select);
        if (!showing_message_ || !placed_) place(in);
        showing_message_ = true;
        return;
    }
    showing_message_ = false;

    if (toggle) {
        if (open_) close();
        else open();
    }
    if (!open_) return;
    if (!placed_) place(in);

    visible_.clear();
    for (Item &it : *page_)
        if (!it.shown || it.shown()) visible_.push_back(&it);
    if (visible_.empty()) return;
    if (sel_ >= (int)visible_.size()) sel_ = (int)visible_.size() - 1;
    if (sel_ < 0) sel_ = 0;

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
    if (fire) {
        Item &it = *visible_[sel_];
        if (dir == 1) sel_ = (sel_ + n - 1) % n;
        if (dir == 2) sel_ = (sel_ + 1) % n;
        if (dir == 3 && it.sub) enter(it);
        else if (dir == 3 && it.value && it.change) it.change(+1);
        if (dir == 4 && it.value && it.change) it.change(-1);
    }
    if (open_ && select) {
        Item &it = *visible_[std::min(sel_, n - 1)];
        if (it.sub) enter(it);
        else if (it.press) it.press();
        else if (it.change) it.change(+1);
    } else if (open_ && back_pressed) {
        back();
    }
    // a row's change may have hidden rows or changed page: the list again for the draw
    if (open_) {
        visible_.clear();
        for (Item &it : *page_)
            if (!it.shown || it.shown()) visible_.push_back(&it);
        if (sel_ >= (int)visible_.size()) sel_ = (int)visible_.size() - 1;
    }
}

// ---- the picture ----

float Menu::line_width(const Line &l) const
{
    float w = 0;
    for (const Line::Seg &s : l.segs) w = std::max(w, s.x) + l.font->CalcTextSizeA(l.size, 1e9f, 0, s.text.c_str()).x;
    return w;
}

// the title, the rows (the selected one yellow, "> "; a value bold and white, every value in one
// column so nothing moves as the selection does), the controls lines, then HELP.
void Menu::build_lines(std::vector<Line> &out, bool &has_help)
{
    auto line = [&](ImFont *f, float size, float pitch) -> Line & {
        out.emplace_back();
        out.back().font = f;
        out.back().size = size;
        out.back().pitch = pitch;
        return out.back();
    };
    line(font_, kFontPx, kPitch).segs.push_back({page_title_, kText, false});
    line(font_, kFontPx, kPitch);
    const float column = value_column(*page_);
    for (int i = 0; i < (int)visible_.size(); ++i) {
        const Item &it = *visible_[i];
        if (it.gap && i > 0) line(font_, kFontPx, kPitch);
        bool s = i == sel_;
        Line &l = line(font_, kFontPx, kPitch);
        ImU32 c = s ? kSelected : kText;
        std::string head = (s ? "> " : "  ") + it.label;      // the same width: the labels stay put
        if (it.value) {
            l.segs.push_back({head + ":", c, false});
            l.segs.push_back({it.value(), s ? kSelected : kWhite, true, column});
        } else {
            l.segs.push_back({it.sub ? head + "  >" : head, c, false});
        }
    }
    line(font_, kFontPx, kPitch);
    for (const char *c : kControlsLine) line(font_, kFontPx, kPitch).segs.push_back({c, kText, false});

    // HELP: wrapped to the page's widest line; blank lines to the page's longest help
    has_help = false;
    for (const Item *it : visible_) has_help |= it->help != nullptr;
    if (!has_help) return;
    const float max_w = menu_w_;            // the menu's width (SIZES): the same wrap on every page
    auto wrap = [&](const char *text) { return wrap_help(text, max_w); };
    size_t rows = 0;                        // room for the page's longest help, hidden rows' too
    for (const Item &it : *page_)
        if (it.help) rows = std::max(rows, wrap(it.help).size());
    std::vector<std::string> help;
    if (visible_[sel_]->help) help = wrap(visible_[sel_]->help);
    const float hp = kPitch * 0.75f;
    line(help_font_, kHelpPx, hp);
    for (size_t i = 0; i < rows; ++i) {
        Line &l = line(help_font_, kHelpPx, hp);
        if (i < help.size()) l.segs.push_back({help[i], kHelp, false});
    }
}

// HELP's words wrapped to `width` pixels
std::vector<std::string> Menu::wrap_help(const char *text, float width) const
{
    std::vector<std::string> lines;
    std::string cur;
    float space = help_font_->CalcTextSizeA(kHelpPx, 1e9f, 0, " ").x, w = 0;
    const char *p = text;
    while (*p) {
        const char *e = p;
        while (*e && *e != ' ' && *e != '\n') ++e;
        std::string word(p, e);
        float ww = help_font_->CalcTextSizeA(kHelpPx, 1e9f, 0, word.c_str()).x;
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

// the values' column: past the page's longest "  label:  ", hidden rows' too (they may show)
float Menu::value_column(const Page &p) const
{
    float column = 0;
    for (const Item &it : p)
        if (it.value) column = std::max(column, font_->CalcTextSizeA(kFontPx, 1e9f, 0, ("  " + it.label + ":  ").c_str()).x);
    return column;
}

// SIZES (menu.h): the widest any page reached from the top can be, every row shown, a `reserve` row's value with
// the front's value_reserve after it
float Menu::widest() const
{
    auto text_w = [&](const std::string &t) { return font_->CalcTextSizeA(kFontPx, 1e9f, 0, t.c_str()).x; };
    float w = 0;
    for (const char *c : kControlsLine) w = std::max(w, text_w(c));
    std::vector<std::pair<const Page *, std::string>> todo = {{top_, top_title_}};
    std::vector<const Page *> seen;
    while (!todo.empty()) {
        auto [p, title] = todo.back();
        todo.pop_back();
        if (!p || std::find(seen.begin(), seen.end(), p) != seen.end()) continue;
        seen.push_back(p);
        w = std::max(w, text_w(title));
        float column = value_column(*p);
        for (const Item &it : *p) {
            if (it.value) w = std::max(w, column + text_w(it.reserve ? it.value() + hooks_.value_reserve : it.value()));
            else w = std::max(w, text_w("  " + it.label + (it.sub ? "  >" : "")));
            if (it.sub) {
                std::string up = it.label;
                std::transform(up.begin(), up.end(), up.begin(), [](unsigned char c) { return (char)std::toupper(c); });
                todo.push_back({it.sub, top_title_ + " > " + up});
            }
        }
    }
    return w;
}

// SIZES: the page's height with every row shown and room for its longest help
float Menu::tallest(const Page &p) const
{
    float lines = 2 + 1 + 2;                // title and a blank; a blank and the controls lines
    bool help = false;
    size_t rows = 0;
    for (size_t i = 0; i < p.size(); ++i) {
        lines += 1 + (p[i].gap && i > 0 ? 1 : 0);
        if (p[i].help) {
            help = true;
            rows = std::max(rows, wrap_help(p[i].help, menu_w_).size());
        }
    }
    return lines * kPitch + (help ? (1 + rows) * kPitch * 0.75f : 0.f);
}

void Menu::build_message(std::vector<Line> &out)
{
    std::string text = hooks_.message ? hooks_.message() : std::string();
    std::string pick = hooks_.pick ? hooks_.pick(false) : std::string();
    if (!pick.empty()) text += "\n\n" + pick;
    size_t a = 0;
    for (;;) {
        size_t b = text.find('\n', a);
        std::string l = text.substr(a, b == std::string::npos ? std::string::npos : b - a);
        ImU32 c = kText;
        if (!l.empty() && (l[0] == '\x01' || l[0] == '\x02')) {
            c = l[0] == '\x01' ? kSelected : kNote;
            l.erase(0, 1);
        }
        out.emplace_back();
        out.back().font = font_;
        out.back().segs.push_back({l, c, false});
        if (b == std::string::npos) break;
        a = b + 1;
    }
}

void Menu::draw_panel(ImDrawList *dl, float ox, float oy, const std::vector<Line> &lines, float w, float h)
{
    dl->AddRectFilled(ImVec2(ox, oy), ImVec2(ox + w, oy + h), kBack);
    float y = oy + kMargin;
    for (const Line &l : lines) {
        float x = ox + kMargin;
        for (const Line::Seg &s : l.segs) {
            if (s.x >= 0) x = std::max(x, ox + kMargin + s.x);
            const char *b = s.text.c_str();
            dl->AddText(l.font, l.size, ImVec2(x, y), s.col, b);
            if (s.bold) dl->AddText(l.font, l.size, ImVec2(x + 1.5f, y), s.col, b);     // ProggyForever has no bold
            x += l.font->CalcTextSizeA(l.size, 1e9f, 0, b).x;
        }
        y += l.pitch;
    }
    if (!hooks_.version.empty() && !showing_message_) {
        std::string v = "v" + hooks_.version;
        float vw = small_font_->CalcTextSizeA(kSmallPx, 1e9f, 0, v.c_str()).x;
        dl->AddText(small_font_, kSmallPx, ImVec2(ox + w - kMargin * 0.2f - vw, oy + kMargin * 0.2f), kHelp, v.c_str());
    }
}

void Menu::prepare(gpu::Frame &f)
{
    bool up = (open_ || showing_message_) && ctx_ && render_ && panel_.layer;
    auto hide = [](Drawn &d) {
        static const float kNowhere[3] = {0, 0, 0};
        if (d.layer) layer_place(d.layer, kNowhere, Quat{}, 1, 1, false);
    };
    bool noting = !note_text_.empty() && ctx_ && render_ && note_.layer;
    if (!up) {
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
        if (!showing_message_ && menu_w_ == 0) menu_w_ = widest();
        if (sized_page_ != page_ || showing_message_) {
            sized_page_ = showing_message_ ? nullptr : page_;
            page_w_ = 0;
            page_h_ = showing_message_ ? 0 : tallest(*page_);
        }
        std::vector<Line> lines;
        bool has_help = false;
        if (showing_message_) build_message(lines);
        else build_lines(lines, has_help);
        float tw = 0, th = 0;
        for (const Line &l : lines) {
            tw = std::max(tw, line_width(l));
            th += l.pitch;
        }
        if (!showing_message_) tw = menu_w_ = std::max(menu_w_, tw);
        page_w_ = std::max(page_w_, tw);
        page_h_ = std::max(page_h_, th);
        uint32_t pw = (uint32_t)std::ceil(page_w_ + 2 * kMargin), ph = (uint32_t)std::ceil(page_h_ + 2 * kMargin);
        ImDrawList *dl = begin(panel_, "##panel", pw, ph);
        draw_panel(dl, panel_.x, 0, lines, (float)panel_.w, (float)panel_.h);
        ImGui::End();

        // the CONTROLS box: two columns, the buttons white
        legend_.w = legend_.h = 0;
        const Legend *lg = hooks_.legend && !showing_message_ ? hooks_.legend() : nullptr;
        if (lg && legend_.layer) {
            legend_shown_ = lg;
            float kw = font_->CalcTextSizeA(kFontPx, 1e9f, 0, "CONTROLS").x, aw = 0;
            for (const auto &r : *lg) {
                kw = std::max(kw, font_->CalcTextSizeA(kFontPx, 1e9f, 0, r.first.c_str()).x);
                aw = std::max(aw, font_->CalcTextSizeA(kFontPx, 1e9f, 0, r.second.c_str()).x);
            }
            float bw = kw + kLegendGap + aw + 2 * kMargin, bh = (lg->size() + 2) * kPitch + 2 * kMargin;
            dl = begin(legend_, "##legend", (uint32_t)std::ceil(bw), (uint32_t)std::ceil(bh));
            float ox = legend_.x;
            dl->AddRectFilled(ImVec2(ox, 0), ImVec2(ox + legend_.w, (float)legend_.h), kBack);
            dl->AddText(font_, kFontPx, ImVec2(ox + kMargin, kMargin), kWhite, "CONTROLS");
            float y = kMargin + 2 * kPitch;
            for (const auto &r : *lg) {
                dl->AddText(font_, kFontPx, ImVec2(ox + kMargin, y), kWhite, r.first.c_str());
                dl->AddText(font_, kFontPx, ImVec2(ox + kMargin + kw + kLegendGap, y), kText, r.second.c_str());
                y += kPitch;
            }
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
            if (boxes_[i].draw) boxes_[i].draw(dl, d.x + kMargin, kMargin, kPxPerM);
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
    Quat rot = yaw_quat(yaw_);
    float panel_w = panel_.w / kPxPerM;
    if (paint(panel_)) layer_place(panel_.layer, pos_, rot, panel_w, panel_.h / kPxPerM, true);
    else hide(panel_);
    float edge[2] = {panel_w / 2 + kHinge, panel_w / 2 + kHinge};     // the next box's hinge, left and right
    auto hinge = [&](Drawn &d) {
        if (!paint(d)) {
            hide(d);
            return;
        }
        float bw = d.w / kPxPerM, side = d.left ? -1.f : 1.f;
        float a = d.left ? kTurn : -kTurn;  // the far edge comes towards you
        float &e = edge[d.left ? 0 : 1];
        float c[3];
        panel_point(pos_, yaw_, side * (e + std::cos(kTurn) * bw / 2), 0, std::sin(kTurn) * bw / 2, c);
        layer_place(d.layer, c, yaw_quat(yaw_ + a), bw, d.h / kPxPerM, true);
    };
    if (legend_.w) hinge(legend_);
    else hide(legend_);
    for (Drawn &d : box_layers_) {
        if (d.w) hinge(d);
        else hide(d);
    }
}

}

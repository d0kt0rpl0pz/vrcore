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

// the same two lines for a pointer (a controller shows): widest() counts both, so nothing resizes with the switch
constexpr const char *kPointerLine[2] = {"point and pull the trigger", "B back \xc2\xb7 menu close"};

const ImU32 kGlow = IM_COL32(255, 255, 0, 16);              // a hovered row's wash
const ImU32 kDot = IM_COL32(255, 255, 255, 255);
constexpr float kDotPx = 0.008f * Menu::kPxPerM;            // the cursor's radius

// THE ROW KINDS (menu.h): a glyph's width (the `<` `>` `v` and the page's `>`, drawn as shapes: the font has none), a
// button's padding, and the parts of a row the pointer tells apart
constexpr float kGlyphW = 0.42f * kFontPx;
constexpr float kBtnPad = 0.28f * kFontPx;
enum Zone { kNoZone = 0, kDec, kInc, kReset, kConfirm, kValue };
const ImU32 kBtn = IM_COL32(30, 70, 64, 255);               // a button, at rest
const ImU32 kBtnHot = IM_COL32(60, 120, 108, 255);          // under the ray
const ImU32 kDropBack = IM_COL32(14, 24, 28, 255);

double steady_s()
{
    return SDL_GetTicksNS() * 1e-9;
}

Quat yaw_quat(float yaw)
{
    return {0.f, std::sin(yaw * 0.5f), 0.f, std::cos(yaw * 0.5f)};
}

// a point in the panel's own space (x right, y up, +z towards you) to the room
// a vector turned by a unit quaternion
void rotate(const Quat &q, float x, float y, float z, float out[3])
{
    float tx = 2.f * (q.y * z - q.z * y), ty = 2.f * (q.z * x - q.x * z), tz = 2.f * (q.x * y - q.y * x);
    out[0] = x + q.w * tx + (q.y * tz - q.z * ty);
    out[1] = y + q.w * ty + (q.z * tx - q.x * tz);
    out[2] = z + q.w * tz + (q.x * ty - q.y * tx);
}

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
        int glyph = 0;                      // 1 `<`, 2 `>`, 3 `v`, drawn as a shape of width w (text empty)
        int zone = 0;                       // Zone: what a click on it does (menu.cpp), and what lights under the ray
        bool button = false;                // drawn as a button round its text
        float w = -1;                       // >= 0: its width (else the text's)
        float lead = 0;                     // empty room before it
    };
    std::vector<Seg> segs;
    ImFont *font = nullptr;
    float size = kFontPx;
    float pitch = kPitch;
    int row = -1;                           // the visible row it is (-1: not one)
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
}

void Menu::set_top(Page *top, const char *title)
{
    top_ = page_ = top;
    top_title_ = page_title_ = title;
}

// ---- the pages ----

void Menu::enter(Item &it)
{
    disarm(page_);
    button_focus_ = false;
    dropdown_ = -1;
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
    disarm(page_);
    button_focus_ = false;
    dropdown_ = -1;
    stack_.clear();
    page_ = top_;
    page_title_ = top_title_;
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

Item Menu::return_item()
{
    Item it;
    it.label = "Return";
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

Item confirm(const std::string &label, std::function<void()> run)
{
    Item it;
    it.label = label;
    it.kind = Kind::Confirm;
    it.run = run;
    auto armed = std::make_shared<bool>(false);
    it.armed = armed;
    it.value = [armed] { return std::string(*armed ? "Yes" : "No"); };
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
    // THE CONTROLLERS drawn (prepare): the rule's shown hands; the ray from the pointing one (as point picks it)
    {
        int ray = in.controller_shown(Input::kRight) && in.hands[Input::kRight].aim_tracked ? Input::kRight
                : in.controller_shown(Input::kLeft) && in.hands[Input::kLeft].aim_tracked ? Input::kLeft : -1;
        for (int h = 0; h < 2; ++h) {
            ControllerModels::Draw &c = ctl_[h];
            c.shown = in.controller_shown(h);
            c.grip = in.hands[h].grip;
            c.aim = in.hands[h].aim;
            c.ray = h == ray;
        }
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
        if (hooks_.pick) hooks_.pick(select);
        if (!showing_message_ || !placed_) place(in);
        showing_message_ = true;
        hit_id_ = hover_row_ = -1;          // no pointer on it: the ray runs free
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

    visible_.clear();
    for (Item &it : *page_)
        if (!it.shown || it.shown()) visible_.push_back(&it);
    if (visible_.empty()) return;
    if (sel_ >= (int)visible_.size()) sel_ = (int)visible_.size() - 1;
    if (sel_ < 0) sel_ = 0;

    // THE POINTER: hover selects the row under the ray; the trigger's edge clicks it (THE ROW KINDS)
    if (dropdown_ >= (int)visible_.size() || (dropdown_ >= 0 && visible_[dropdown_]->choices.empty())) dropdown_ = -1;
    {
        bool click = false;
        point(in, click);
        if (pointing_ < 0) dropdown_ = -1;  // the controller went (a pad or key took over): no list left hanging
        bool acted = false, hold = false;
        if (pointing_ >= 0 && dropdown_ >= 0) {
            if (click) {
                Item &d = *visible_[dropdown_];
                if (drop_hover_ >= 0 && drop_hover_ < (int)d.choices.size() && d.pick) d.pick(drop_hover_);
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
            } else if (click) {
                acted = true;
                if (it.kind == Kind::Choice) {
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
        if (dir == 1) sel_ = (sel_ + n - 1) % n;
        if (dir == 2) sel_ = (sel_ + 1) % n;
        if (dir == 3 && it.sub) enter(it);
        else if (dir == 3 && conf && armed(it)) button_focus_ = true;
        else if (dir == 3 && it.value && it.change) it.change(+1);
        if (dir == 4 && conf && button_focus_) button_focus_ = false;
        else if (dir == 4 && it.value && it.change) it.change(-1);
    }
    if (open_ && select) {
        Item &it = *visible_[std::min(sel_, n - 1)];
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
    if (open_) {
        visible_.clear();
        for (Item &it : *page_)
            if (!it.shown || it.shown()) visible_.push_back(&it);
        if (sel_ >= (int)visible_.size()) sel_ = (int)visible_.size() - 1;
        if (sel_ >= 0 && !visible_.empty()) {
            disarm(page_, visible_[sel_]);
            if (!armed(*visible_[sel_])) button_focus_ = false;
        }
    }
}

// ---- the picture ----

// Where each seg of the line starts from its left edge (`xs`, if given) and the line's width.
float Menu::layout(const Line &l, std::vector<float> *xs) const
{
    float x = 0;
    if (xs) xs->clear();
    for (const Line::Seg &s : l.segs) {
        x = std::max(x, s.x) + s.lead;
        if (xs) xs->push_back(x);
        x += s.w >= 0 ? s.w : l.font->CalcTextSizeA(l.size, 1e9f, 0, s.text.c_str()).x;
    }
    return x;
}

// One row's segs by its kind (THE ROW KINDS). `worst`: for SIZES, the widest it can be: a reserved value with its
// reserve, a choice's longest label, a Confirm with its button.
void Menu::item_segs(const Item &it, bool selected, float column, bool worst, Line &l) const
{
    using Seg = Line::Seg;
    const float sw = font_->CalcTextSizeA(kFontPx, 1e9f, 0, " ").x;
    const ImU32 c = selected ? kSelected : kText;
    const std::string head = (selected ? "> " : "  ") + it.label;      // the same width: the labels stay put
    auto glyph = [&](int g, int zone, float lead, float x = -1) {
        Seg q{"", c, false, x};
        q.glyph = g;
        q.zone = zone;
        q.w = kGlyphW;
        q.lead = lead;
        l.segs.push_back(q);
    };
    auto button = [&](const std::string &text, int zone) {
        Seg q{text, c, false};
        q.button = true;
        q.zone = zone;
        q.lead = sw;
        q.w = font_->CalcTextSizeA(kFontPx, 1e9f, 0, text.c_str()).x + 2 * kBtnPad;
        l.segs.push_back(q);
    };
    if (!it.value) {
        if (it.kind == Kind::Page) {
            l.segs.push_back({head, c, false});
            glyph(2, 0, 2 * sw);
        } else {
            l.segs.push_back({it.sub ? head + "  >" : head, c, false});
        }
        return;
    }
    l.segs.push_back({head + ":", c, false});
    std::string v = it.value();
    if (worst && it.kind == Kind::Choice) {
        float best = 0;
        for (const std::string &s : it.choices) {
            float w = font_->CalcTextSizeA(kFontPx, 1e9f, 0, s.c_str()).x;
            if (w > best) {
                best = w;
                v = s;
            }
        }
    } else if (worst && it.reserve) {
        v += hooks_.value_reserve;
    }
    if (it.kind == Kind::Stepper) glyph(1, kDec, 0, column - kGlyphW - 0.3f * sw);
    Seg val{v, selected ? kSelected : kWhite, true, column};
    val.zone = kValue;
    l.segs.push_back(val);
    if (it.kind == Kind::Stepper) {
        glyph(2, kInc, 0.3f * sw);
        if (it.reset) button("Reset", kReset);
    } else if (it.kind == Kind::Choice) {
        glyph(3, 0, 0.4f * sw);
    } else if (it.kind == Kind::Confirm && (worst || armed(it))) {
        button("Confirm " + it.label, kConfirm);
    }
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
        Line &l = line(font_, kFontPx, kPitch);
        l.row = i;
        item_segs(it, i == sel_, column, false, l);
    }
    line(font_, kFontPx, kPitch);
    for (const char *c : pointer_hint_ ? kPointerLine : kControlsLine)
        line(font_, kFontPx, kPitch).segs.push_back({c, kText, false});

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
    for (const char *c : kPointerLine) w = std::max(w, text_w(c));
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
            Line l;
            l.font = font_;
            item_segs(it, false, column, true, l);
            w = std::max(w, layout(l));
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
    float drop = 0;                         // the lowest a dropdown reaches, in lines: THE ROW KINDS
    float at = 2;                           // lines above the row
    for (size_t i = 0; i < p.size(); ++i) {
        lines += 1 + (p[i].gap && i > 0 ? 1 : 0);
        at += 1 + (p[i].gap && i > 0 ? 1 : 0);
        if (!p[i].choices.empty()) drop = std::max(drop, at + (float)p[i].choices.size());
        if (p[i].help) {
            help = true;
            rows = std::max(rows, wrap_help(p[i].help, menu_w_).size());
        }
    }
    return std::max(lines * kPitch + (help ? (1 + rows) * kPitch * 0.75f : 0.f), drop * kPitch);
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
    std::vector<float> xs;
    for (const Line &l : lines) {
        const bool hovered = l.row >= 0 && l.row == hover_row_ && hit_id_ == 0;
        if (hovered) dl->AddRectFilled(ImVec2(ox + kMargin * 0.4f, y), ImVec2(ox + w - kMargin * 0.4f, y + l.pitch), kGlow);
        layout(l, &xs);
        for (size_t i = 0; i < l.segs.size(); ++i) {
            const Line::Seg &s = l.segs[i];
            const float x = ox + kMargin + xs[i];
            const bool lit = hovered && s.zone && s.zone == hover_zone_;
            if (s.glyph) {
                // a shape: `<` `>` as a chevron, `v` as a triangle, in the row's colour (white under the ray)
                const float cx = x + s.w / 2, cy = y + l.size * 0.5f, hh = l.size * 0.25f, hw = s.w * 0.25f;
                const ImU32 col = lit ? kWhite : s.col;
                if (s.glyph == 3) {
                    dl->AddTriangleFilled(ImVec2(cx - s.w / 2, cy - hh * 0.5f), ImVec2(cx + s.w / 2, cy - hh * 0.5f),
                                          ImVec2(cx, cy + hh), col);
                } else {
                    const float d = s.glyph == 1 ? -1.f : 1.f;
                    const ImVec2 pts[3] = {ImVec2(cx - d * hw, cy - hh), ImVec2(cx + d * hw, cy), ImVec2(cx - d * hw, cy + hh)};
                    dl->AddPolyline(pts, 3, col, 0, lit ? 4.f : 3.f);
                }
                continue;
            }
            const char *b = s.text.c_str();
            float tx = x;
            ImU32 col = s.col;
            if (s.button) {
                const bool focus = s.zone == kConfirm && button_focus_ && l.row == sel_;
                dl->AddRectFilled(ImVec2(x, y + 1), ImVec2(x + s.w, y + l.pitch - 3), focus ? kSelected : lit ? kBtnHot : kBtn,
                                  kBtnPad * 0.5f);
                tx = x + kBtnPad;
                col = focus ? kBack : kWhite;
            }
            dl->AddText(l.font, l.size, ImVec2(tx, y), col, b);
            if (s.bold) dl->AddText(l.font, l.size, ImVec2(tx + 1.5f, y), col, b);     // ProggyForever has no bold
        }
        y += l.pitch;
    }
    // THE ROW KINDS: the open dropdown, over what lies under it; the current choice marked, the one under the ray lit
    if (dropdown_ >= 0 && dropdown_ < (int)visible_.size() && drop_x1_ > drop_x0_) {
        const Item &it = *visible_[dropdown_];
        const int cur = it.index ? it.index() : -1;
        dl->AddRectFilled(ImVec2(ox + drop_x0_, oy + drop_y0_), ImVec2(ox + drop_x1_, oy + drop_y1_), kDropBack, kBtnPad * 0.5f);
        dl->AddRect(ImVec2(ox + drop_x0_, oy + drop_y0_), ImVec2(ox + drop_x1_, oy + drop_y1_), kText, kBtnPad * 0.5f, 0, 2.f);
        for (size_t i = 0; i < it.choices.size(); ++i) {
            const float ty = oy + drop_y0_ + (float)i * kPitch;
            if ((int)i == drop_hover_)
                dl->AddRectFilled(ImVec2(ox + drop_x0_ + 3, ty + 1), ImVec2(ox + drop_x1_ - 3, ty + kPitch - 1), kBtnHot);
            const ImU32 col = (int)i == cur ? kSelected : kWhite;
            dl->AddText(font_, kFontPx, ImVec2(ox + drop_x0_ + kBtnPad, ty + (kPitch - kFontPx) * 0.5f), col,
                        it.choices[i].c_str());
            if ((int)i == cur)
                dl->AddCircleFilled(ImVec2(ox + drop_x1_ - kBtnPad * 0.6f, ty + kPitch * 0.5f), kDotPx * 0.6f, kSelected, 12);
        }
    }
    if (!hooks_.version.empty() && !showing_message_) {
        std::string v = "v" + hooks_.version;
        float vw = small_font_->CalcTextSizeA(kSmallPx, 1e9f, 0, v.c_str()).x;
        dl->AddText(small_font_, kSmallPx, ImVec2(ox + w - kMargin * 0.2f - vw, oy + kMargin * 0.2f), kHelp, v.c_str());
    }
}

// The pointing hand's ray against the quads prepare hung (the nearest one in front of it), into that layer's
// pixels; the panel's row under it; a light tick when the row changes. `click`: the trigger went down.
void Menu::point(const Input &in, bool &click)
{
    pointer_hint_ = in.controller_shown(Input::kLeft) || in.controller_shown(Input::kRight);
    pointing_ = in.controller_shown(Input::kRight) && in.hands[Input::kRight].aim_tracked ? Input::kRight
              : in.controller_shown(Input::kLeft) && in.hands[Input::kLeft].aim_tracked ? Input::kLeft : -1;
    int was = hover_row_;
    hit_id_ = hover_row_ = -1;
    hit_t_ = 0;
    click = false;
    if (pointing_ < 0) return;
    const Pose &a = in.hands[pointing_].aim;
    float d[3], best = 1e9f;
    rotate(a.rot, 0, 0, -1, d);
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
            if (hit_y_ >= rows_[i].y && hit_y_ < rows_[i].y + rows_[i].h) hover_row_ = (int)i;
    }
    click = trigger_pressed_[pointing_];
    // an open dropdown takes the ray: the rows under it are not hovered, and the selection stays
    const int was_drop = drop_hover_;
    drop_hover_ = -1;
    if (dropdown_ >= 0 && dropdown_ < (int)visible_.size()) {
        if (hit_id_ == 0 && hit_x_ >= drop_x0_ && hit_x_ < drop_x1_ && hit_y_ >= drop_y0_ && hit_y_ < drop_y1_)
            drop_hover_ = (int)((hit_y_ - drop_y0_) / kPitch);
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
        const float sw = font_->CalcTextSizeA(kFontPx, 1e9f, 0, " ").x;
        if (it.kind == Kind::Stepper && r.dec0 >= 0 && hit_x_ >= r.dec0 - sw && hit_x_ < r.inc1 + sw)
            hover_zone_ = hit_x_ < (r.dec0 + r.inc1) / 2 ? kDec : kInc;
        if (it.kind == Kind::Stepper && r.reset0 >= 0 && hit_x_ >= r.reset0 && hit_x_ < r.reset1) hover_zone_ = kReset;
        if (it.kind == Kind::Confirm && r.conf0 >= 0 && hit_x_ >= r.conf0 && hit_x_ < r.conf1) hover_zone_ = kConfirm;
    }
    if (hover_row_ >= 0 && hover_row_ != was) {
        sel_ = hover_row_;
        haptic(pointing_, 0.15f, 0.02f);
    }
}

void Menu::prepare(gpu::Frame &f)
{
    // the model each hand's profile calls for (input.h: "" in the sim, so the generic pair there)
    if (gpu_ && models_.ready()) {
        for (int h = 0; h < 2; ++h) models_.set_profile(*gpu_, h, controller_profile(h));
        models_.tick(*gpu_);
    }
    bool up = (open_ || showing_message_) && ctx_ && render_ && panel_.layer;
    auto hide = [](Drawn &d) {
        static const float kNowhere[3] = {0, 0, 0};
        if (d.layer) layer_place(d.layer, kNowhere, Quat{}, 1, 1, false);
    };
    bool noting = !note_text_.empty() && ctx_ && render_ && note_.layer;
    if (!up) {
        quads_.clear();
        hit_id_ = hover_row_ = -1;
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
            tw = std::max(tw, layout(l));
            th += l.pitch;
        }
        if (!showing_message_) tw = menu_w_ = std::max(menu_w_, tw);
        page_w_ = std::max(page_w_, tw);
        page_h_ = std::max(page_h_, th);
        uint32_t pw = (uint32_t)std::ceil(page_w_ + 2 * kMargin), ph = (uint32_t)std::ceil(page_h_ + 2 * kMargin);
        rows_.clear();
        float ly = kMargin;
        std::vector<float> xs;
        for (const Line &l : lines) {
            if (l.row >= 0) {
                RowRect r{ly, l.pitch};
                layout(l, &xs);
                for (size_t i = 0; i < l.segs.size(); ++i) {
                    const Line::Seg &s = l.segs[i];
                    const float x0 = kMargin + xs[i], x1 = x0 + (s.w >= 0 ? s.w : l.font->CalcTextSizeA(l.size, 1e9f, 0, s.text.c_str()).x);
                    if (s.zone == kDec) r.dec0 = x0;
                    if (s.zone == kInc) r.inc1 = x1;
                    if (s.zone == kReset) {
                        r.reset0 = x0;
                        r.reset1 = x1;
                    }
                    if (s.zone == kConfirm) {
                        r.conf0 = x0;
                        r.conf1 = x1;
                    }
                    if (s.zone == kValue) r.val0 = x0;
                }
                rows_.push_back(r);
            }
            ly += l.pitch;
        }
        // the open dropdown hangs under its value (THE ROW KINDS), as wide as its longest choice
        drop_x1_ = drop_x0_ = 0;
        if (dropdown_ >= 0 && dropdown_ < (int)rows_.size() && dropdown_ < (int)visible_.size() &&
            !visible_[dropdown_]->choices.empty()) {
            const Item &it = *visible_[dropdown_];
            float dw = 0;
            for (const std::string &c : it.choices) dw = std::max(dw, font_->CalcTextSizeA(kFontPx, 1e9f, 0, c.c_str()).x);
            drop_x0_ = rows_[dropdown_].val0 - kBtnPad;
            drop_x1_ = drop_x0_ + dw + 3 * kBtnPad;
            drop_y0_ = rows_[dropdown_].y + rows_[dropdown_].h;
            drop_y1_ = drop_y0_ + (float)it.choices.size() * kPitch;
        }
        quads_.clear();
        auto cursor = [&](ImDrawList *l, const Drawn &d, int id) {
            if (hit_id_ != id) return;
            ImVec2 c(d.x + hit_x_, hit_y_);
            l->AddCircleFilled(c, kDotPx, hover_row_ >= 0 ? kSelected : kDot, 16);
        };
        ImDrawList *dl = begin(panel_, "##panel", pw, ph);
        draw_panel(dl, panel_.x, 0, lines, (float)panel_.w, (float)panel_.h);
        cursor(dl, panel_, 0);
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
    // kRayMiss; yellow over a row, a dropdown's choice or a Confirm button, as the cursor dot is.
    if (models_.ready()) {
        ControllerModels::Draw draws[2] = {ctl_[0], ctl_[1]};
        bool any = false;
        for (ControllerModels::Draw &c : draws) {
            c.ray_length = hit_id_ >= 0 && hit_t_ > 0 ? hit_t_ : ControllerModels::kRayMiss;
            c.clickable = hover_row_ >= 0 || drop_hover_ >= 0;
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

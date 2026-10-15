/*
 * menu.h -- a settings menu in the headset: pages of items, the navigation, a panel in
 * front of you, the CONTROLS box hinged on its right edge, and a front's own boxes (Box) hinged on either side.
 * A game's front builds the pages (set_top) and says what opening does to its game (Hooks).
 *
 *   open/close   Quest: left menu button   keyboard: Esc   gamepad: Start
 *   move         thumbstick / d-pad / arrows up-down (repeats when held: 0.35 s, then every 0.12 s)
 *   change       thumbstick / d-pad / arrows left-right, or A / Enter (also runs an action)
 *   back         B / Backspace / gamepad B (or the Return row)
 *
 * THE PICTURE. Each box is its own quad layer (app/layers.h: the compositor draws it, so it costs the eye pass
 * nothing), drawn with Dear ImGui (ui/imgui_render) in ImGui's built-in font, ProggyClean, baked at the pixel
 * size it is drawn at. Sizes: capitals 30 mm, line pitch 52.6 mm. ProggyClean's capitals are
 * 0.69 of its size, so its size is 43.5 mm, at kPxPerM pixels a metre. All of it is
 * drawn at 40/48 of that (menu.cpp kSize: too wide otherwise in ProggyClean's wide letters).
 *
 * PLACEMENT: on opening, 1.1 m ahead of the head, level (its yaw alone), 5 cm under the eyes,
 * facing you; the boxes hinged 3 cm off the panel's edges, turned 30 degrees in towards you, centred on its
 * middle's height. A box is sized to its text on every draw; the panel's width only grows while on one page, and
 * its height is held on a page with help (HELP), so the box does not jump as the selection moves.
 *
 * SIZES (the user: nothing may resize while you change a row): on opening, the panel's width is the widest any page
 * can be (every row shown, a value marked `reserve` with Hooks::value_reserve after it), the same on every page; on entering a page,
 * its height has room for every row, hidden ones too, and its longest help. Both only grow after that, should a
 * value outgrow its reserve.
 *
 * HELP and VERSION: the selected item's help under the page, three quarters the size, dimmer, wrapped
 * to the menu's width, with room kept for the page's longest; "v" + version in the top right margin.
 *
 * THE MESSAGE PANEL: while Hooks::message gives a text (the game could not start), the panel shows it in place
 * of the menu, with the pick hook's lines under it (the file picker's row), and no boxes beside.
 *
 * THE POINTER. While Input::controller_shown, the right hand's aim ray (the left's only while the right is not
 * tracked) is cast at the panel and the boxes' quads (the room's, as prepare placed them). Hovering a row selects it
 * (the same selection the d-pad moves: the last used wins); the trigger's rising edge does what A does on it. The
 * CONTROLS and front boxes stop the ray but are not clickable. The cursor dot and the row's glow are drawn into the
 * quad's own texture; a light haptic tick when the hovered row changes, a stronger one on a click. The controls
 * line reads the pointer wording while a controller shows.
 *
 * THE ROW KINDS. An Item built the old way (value + change, press, sub) has Kind::Legacy: it looks and navigates as it
 * always did, and the pointer's click on it is A. The builders below set a kind (and its data) on top of the same
 * value / change / press / sub fields, so the d-pad, stick, keyboard and A / B do exactly what they did for every
 * kind; only the pointer and the picture differ:
 *   toggle    a click anywhere on the row flips it
 *   choice    `v` beside the value; a click opens a dropdown under it (pointer only; the d-pad cycles as before);
 *             click a choice, or outside, or B, to leave it. The room for the list is reserved when the page is
 *             entered (SIZES), the list hangs over the rows below it.
 *   stepper   `<` `>` either side of the value; a click on its left or right half steps by one, a held trigger
 *             repeats (0.35 s, then every 0.12 s). With a reset: a small Reset button after it, which does what A does.
 *   action    a click runs it         page   `>` at the end, a click enters it         return   a click is B
 *   confirm   No / Yes; at Yes a "Confirm <label>" button appears after the value. Pointer: click it. D-pad: right
 *             again (or A) lands on it, A runs it. Leaving the row or the page puts it back to No.
 *
 * RESTART (Menu::restart_item, the old way): the row reads No until moved to Yes, then A restarts; No again on every
 * page change. The game starts again from power-on: app.h's request_restart.
 *
 * FOCUS LOSS: the headset took the app's focus away (the system menu, the
 * headset off): the menu opens, which pauses the game. Android only (Hooks::open_on_focus_loss).
 *
 * Main thread: update and prepare are the front's (its update and prepare).
 */
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "app.h"
#include "controllers.h"
#include "gpu/render.h"
#include "input.h"

struct ImDrawList;
struct ImFont;

namespace vrcore {
struct Layer;
}

namespace vrcore::ui {

struct Item;
using Page = std::vector<Item>;

// How an Item behaves under the pointer and how it is drawn (THE ROW KINDS). Legacy: built the old way.
enum class Kind : uint8_t { Legacy, Toggle, Choice, Stepper, Action, Page, Return, Confirm };

struct Item {
    std::string label;
    std::function<std::string()> value;     // empty: an action
    std::function<void(int)> change;        // -1 / +1 (actions: +1 runs it)
    std::function<void()> press;            // A / Enter, when it does something else than change(+1)
    Page *sub = nullptr;                    // a sub page (the front's, kept alive): A / Enter / right opens it
    std::function<bool()> shown;            // empty: always
    bool gap = false;                       // a blank line above it: a new group starts here
    const char *help = nullptr;             // shown at the bottom while selected (HELP)
    bool reserve = false;                   // its value may gain Hooks::value_reserve: room kept for it (SIZES)
    // Set by the builders (THE ROW KINDS), at the end so a front's own field-by-field items are untouched.
    Kind kind = Kind::Legacy;
    std::vector<std::string> choices;       // Choice: the labels the dropdown lists
    std::function<int()> index;             // Choice: the current one
    std::function<void(int)> pick;          // Choice: the dropdown chose this one
    std::function<void()> reset;            // Stepper: the Reset button (and A); empty: no button
    std::function<void()> run;              // Confirm: what the button does
    std::shared_ptr<bool> armed;            // Confirm: Yes (shared by the copies pages make of the item)
};

// The row builders. Each returns an Item to set `gap` and `help` on and push into a Page.
Item toggle(const std::string &label, std::function<bool()> get, std::function<void(bool)> set,
            const char *on_text = "On", const char *off_text = "Off");
Item choice(const std::string &label, std::vector<std::string> labels, std::function<int()> get,
            std::function<void(int)> set);
// A number from `lo` to `hi` in steps of `step` (left / right and the pointer's halves move it one step); `format`
// writes it. `reset` (optional) is what A does and what the Reset button runs.
Item stepper(const std::string &label, std::function<float()> get, std::function<void(float)> set, float step,
             float lo, float hi, std::function<std::string(float)> format, std::function<void()> reset = nullptr);
Item action(const std::string &label, std::function<void()> run);
Item page(const std::string &label, Page &sub);
// Yes / No, and a "Confirm <label>" button at Yes that calls `run` (which closes the menu itself, if it should).
Item confirm(const std::string &label, std::function<void()> run);


// A front's box beside the panel (Daytona's records), drawn on the menu's
// background with the menu's margin round it. The boxes of a side run outwards in the order given, each kHinge
// (3 cm) past the last, the row curving round you: the first turned 30 degrees in (PLACEMENT), each further stretch
// (a box, or a box's gap) 30 more. The right side's start after CONTROLS.
struct Box {
    bool left = true;                       // hinged on the panel's left edge (else its right, after CONTROLS)
    uint32_t max_width = 1024, max_height = 1024;   // its layer's pixels: the largest it can be, margins included
    // Metres left empty before it in its row, a stretch of its own (turned as a box would be), kept also while it
    // is not shown: room for something of the front's own (Rave Racer's Reiko stands there).
    float gap = 0;
    // Its contents' size in metres this frame; false: not shown (nothing to show yet).
    std::function<bool(float &width_m, float &height_m)> measure;
    // Its contents into `dl`, their top left at ImGui's pixel (x, y), `px_per_m` pixels a metre.
    std::function<void(ImDrawList *dl, float x, float y, float px_per_m)> draw;
    // Every prepare while the menu is up, shown or not: the middle of its gap (at the panel's middle height; no gap:
    // where the box starts), that stretch's yaw, and the panel's middle and yaw. Empty: not told.
    std::function<void(const float gap_mid[3], float line_yaw, const float panel_pos[3], float panel_yaw)> placed;
};

using Legend = std::vector<std::pair<std::string, std::string>>;    // { button, what it does }

struct Hooks {
    std::function<bool()> can_open;         // empty: always
    std::function<void()> on_open;          // the game pauses and gives up its controls
    std::function<void()> on_close;
    std::function<std::string()> message;   // the "could not start" text; empty: none (the menu as usual)
    // The message panel's extra lines (the picker's row) and what A / Enter does to it: called every frame while a
    // message shows, `select` true on the press. Empty: none. A line starting with \x01 is drawn yellow (the row),
    // \x02 red (why the last pick did not work).
    std::function<std::string(bool select)> pick;
    std::function<const Legend *()> legend; // the CONTROLS box's rows; another pointer redraws it. Empty: no box
    std::string version;                    // "v" + this in the top right corner; empty: none
    std::string value_reserve;              // what a value may gain (the front's " (Requires Restart)"): SIZES
#ifdef __ANDROID__
    bool open_on_focus_loss = true;
#else
    bool open_on_focus_loss = false;        // a desktop's focus moves with every click elsewhere
#endif
};

class Menu {
public:
    static constexpr float kPxPerM = 1104.f;        // the layers' pixels a metre (THE PICTURE: 48 px a 43.5 mm size)

    // The ImGui context, its fonts, the layers. `boxes`: the front's, made with it.
    bool init(gpu::Renderer &r, const Hooks &hooks, std::vector<Box> boxes = {});
    void shutdown(gpu::Renderer &r);
    // The top page and its title; each sub page shows "TITLE > PAGE".
    void set_top(Page *top, const char *title);

    // Every update: the buttons, open / close, the navigation, focus loss. `in` the frame's snapshot.
    void update(const Tick &t, const Input &in);
    // Every prepare: the boxes drawn into their layers and placed (hidden when closed).
    void prepare(gpu::Frame &f);

    bool is_open() const { return open_; }
    // The panel is up (open, or the message panel).
    bool shown() const { return open_ || showing_message_; }
    void open();
    void close();
    void enter(Item &it);
    void back();
    // The visible sub page whose label starts with `prefix` (case-insensitive) entered; false: none (tests).
    bool enter_named(const char *prefix);

    // A line of text hung in the room this frame, open or closed (the front's FPS counter over its HUD): its centre
    // at `pos`, turned `rot`, `height_m` tall, in `colour` (ImGui's IM_COL32: 0xAABBGGRR). Given again every frame it
    // shows (before prepare).
    void note(const std::string &text, const float pos[3], const Quat &rot, float height_m, uint32_t colour = 0xffffffffu)
    {
        note_text_ = text;
        note_colour_ = colour;
        for (int i = 0; i < 3; ++i) note_pos_[i] = pos[i];
        note_rot_ = rot;
        note_height_ = height_m;
    }

    // `saved` runs before the restart (the front's saves). Shared by the rows made from it.
    Item restart_item(std::function<void()> saved, bool gap = false);
    // The Return row (Kind::Return): a click or A goes back a page (the top page: closes the menu).
    Item return_item();

private:
    struct Line;
    void place(const Input &in);
    void build_lines(std::vector<Line> &out, bool &has_help);
    void build_message(std::vector<Line> &out);
    void draw_panel(ImDrawList *dl, float ox, float oy, const std::vector<Line> &lines, float w, float h);
    float layout(const Line &l, std::vector<float> *xs = nullptr) const;
    void item_segs(const Item &it, bool selected, float column, bool worst, Line &l) const;
    void disarm(Page *p, const Item *except = nullptr);
    void run_confirm(Item &it);
    bool armed(const Item &it) const { return it.armed && *it.armed; }
    std::vector<std::string> wrap_help(const char *text, float width) const;
    float value_column(const Page &p) const;
    float widest() const;
    float tallest(const Page &p) const;
    void point(const Input &in, bool &click);

    Hooks hooks_;
    Page *top_ = nullptr, *page_ = nullptr;
    std::string top_title_, page_title_;
    struct Level {
        Page *page;
        std::string title;
        int sel;
    };
    std::vector<Level> stack_;
    std::vector<Item *> visible_;
    int sel_ = 0;
    bool open_ = false, showing_message_ = false, restart_yes_ = false;

    // the buttons' last state (the snapshot has levels; the keyboard's edges are the host's)
    bool hand_was_[2][3] = {};              // primary, secondary, menu
    struct PadWas {
        uint32_t id;
        bool south, east, start;
    };
    std::vector<PadWas> pads_was_;
    int last_dir_ = 0;
    double repeat_at_ = 0, now_ = 0;
    bool focused_was_ = false;

    // where the panel hangs (placed on opening)
    float pos_[3] = {0, 0, 0};
    float yaw_ = 0;
    bool placed_ = false;
    Pose last_head_;

    // sizes held per page (DrawPanel)
    const Page *sized_page_ = nullptr;
    float page_w_ = 0, page_h_ = 0;
    float menu_w_ = 0;                      // every page's text width this opening (SIZES; 0: not measured)

    // THE POINTER: the quads as prepare hung them (the room's metres; id 0 panel, 1 CONTROLS, 2 + i box i), where
    // the ray lands, and the rows' top and height in the panel's pixels
    struct Quad {
        float c[3];
        float yaw, w, h;
        int id;
    };
    std::vector<Quad> quads_;
    struct RowRect {
        float y, h;
        // x edges in the panel's pixels (-1: none): the stepper's `<` start and `>` end, its Reset button, the
        // Confirm button, the value's start (the dropdown hangs from it)
        float dec0 = -1, inc1 = -1, reset0 = -1, reset1 = -1, conf0 = -1, conf1 = -1, val0 = -1;
    };
    std::vector<RowRect> rows_;
    int hit_id_ = -1;                       // the quad under the ray (-1 none)
    float hit_t_ = 0;                       // metres along the ray to it
    // THE CONTROLLERS: each hand as update saw it (shown by the rule, its grip and aim), drawn on the overlay
    ControllerModels models_;
    ControllerModels::Draw ctl_[2];
    float hit_x_ = 0, hit_y_ = 0;           // the point on it, in that layer's own pixels from its top left
    int hover_row_ = -1;                    // the visible row under the ray (-1 none; only on the panel)
    int pointing_ = -1;                     // the hand that points (Input::kLeft / kRight), -1 none
    bool trigger_was_[2] = {}, trigger_pressed_[2] = {};
    bool pointer_hint_ = false;             // a controller shows: the controls line reads the pointer wording
    int hover_zone_ = 0;                    // the part of the hovered row under the ray (menu.cpp Zone)
    int dropdown_ = -1;                     // the visible row whose dropdown is open (-1 none)
    int drop_hover_ = -1;                   // the dropdown's choice under the ray
    float drop_x0_ = 0, drop_y0_ = 0, drop_x1_ = 0, drop_y1_ = 0;   // its rectangle in the panel's pixels
    bool button_focus_ = false;             // d-pad: the selected Confirm row's button has the focus
    bool ptr_hold_ = false;                 // a stepper's half is held with the trigger (repeats)
    double ptr_repeat_at_ = 0;

    // the picture
    void *ctx_ = nullptr;                   // ImGuiContext
    ImFont *font_ = nullptr, *help_font_ = nullptr, *small_font_ = nullptr;
    struct Drawn {
        Layer *layer = nullptr;
        uint32_t max_w = 0, max_h = 0;
        ImDrawList *list = nullptr;
        float x = 0;                        // its place on ImGui's canvas
        uint32_t w = 0, h = 0;              // this frame's pixels (0: not shown)
        bool left = false;
    };
    Drawn panel_, legend_, note_;
    std::string note_text_;
    uint32_t note_colour_ = 0xffffffffu;
    float note_pos_[3] = {0, 0, 0}, note_height_ = 0;
    Quat note_rot_;
    std::vector<Drawn> box_layers_;
    std::vector<Box> boxes_;
    class ImguiRender *render_ = nullptr;
    gpu::Renderer *gpu_ = nullptr;          // init's, for the controller models a profile change loads
    const Legend *legend_shown_ = nullptr;
};

}

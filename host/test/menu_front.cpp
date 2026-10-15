/*
 * menu_front.cpp -- vrcore_menu: a front with no game, only the menu (ui/menu.h), so the menu's input and look can
 * be tried in the sim without any game's ROMs. It has the page structure a game's front builds, with neutral
 * labels: the top rows (Resume, the pages, Recenter, Restart), Graphics through ui/graphics.h, Audio, UI, Comfort,
 * Gameplay with a Restart row, help lines, a CONTROLS legend, and a Box on each side.
 *
 * THE ROWS. One of each kind (ui/menu.h THE ROW KINDS), through the builders: toggle, choice, stepper (a percentage,
 * tenths), stepper with a reset, action, page, Return and confirm (the top page's Restart). Two stay the old way, as
 * a front that has not moved over yet: the Graphics profile rows (ui/graphics.h) and the Gameplay page's
 * "Restart game" (Menu::restart_item), so both can be seen to look and navigate as ever.
 *
 * THE CONTROLLERS. The menu draws them itself (ui/controllers.h): a controller and its ray for each hand the rule
 * shows (input.h LAST DEVICE: tracked, and newer than the last pad or key press), so there is nothing here to draw.
 * In the sim the mouse is the right controller (sim/sim.cpp).
 *
 * --menu opens the menu at the start (a headless --shot of it); --page NAME opens it on that sub page. Nothing is a game's: the choices are saved under
 * the keys `ms.` in a settings file of their own.
 */
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <imgui.h>

#include "../app/app.h"
#include "../app/input.h"
#include "../app/log.h"
#include "../app/settings.h"
#include "../gpu/render.h"
#include "../ui/graphics.h"
#include "../ui/menu.h"

namespace vrcore {

namespace {

namespace st = settings;
using ui::Item;
using ui::Legend;
using ui::Page;

// the choices, as a game's front keeps its Args
struct Args {
    bool free_play = true, glass = true, hud = true, fps = false;
    int car_detail = 2, difficulty = 1, country = 0;
    int volume = 100, music = 100, effects = 100;
    int draw = 7;
    float gamma = 1.f, brightness = 1.f;
    float scale = 1.f;
    int dash_cm = -30;
} args;

struct Pages {
    Page top, graphics, audio, ui, comfort, gameplay;
};

struct Front {
    ui::Menu menu;
    Pages *pages = nullptr;
    Options options;
    bool open_at_start = false;
    std::string start_page;                 // --page: a sub page entered as the menu opens
} front;

const char *const kDifficulty[] = {"Easy", "Normal", "Hard", "Hardest"};
const char *const kCountry[] = {"USA", "Japan", "Export"};

const Legend kControls = {
    {"Right trigger", "Accelerate"},
    {"Left trigger", "Brake"},
    {"Left Stick", "Steering"},
    {"A", "Start"},
    {"B", "Change view"},
    {"Menu button (left)", "Settings menu"},
};

float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

std::string fmt(const char *f, double v)
{
    char b[64];
    std::snprintf(b, sizeof b, f, v);
    return b;
}

// a builder's row with its help and gap
Item with(Item it, const char *help = nullptr, bool gap = false)
{
    it.help = help;
    it.gap = gap;
    return it;
}

Item flag(const char *label, bool &field, const char *key, const char *help = nullptr, bool gap = false)
{
    return with(ui::toggle(
                    label, [&field] { return field; },
                    [&field, key](bool v) {
                        field = v;
                        st::set_bool(key, v);
                    }),
                help, gap);
}

Item level(const char *label, int &field, const char *key)
{
    return ui::stepper(
        label, [&field] { return (float)field; },
        [&field, key](float v) {
            field = (int)v;
            st::set_int(key, field);
        },
        10.f, 0.f, 100.f, [](float v) { return std::to_string((int)v) + "%"; });
}

Item tenths(const char *label, float &field, const char *key, float lo, float hi, const char *help, bool gap)
{
    return with(ui::stepper(
                    label, [&field] { return field; },
                    [&field, key](float v) {
                        field = v;
                        st::set_float(key, v);
                    },
                    0.1f, lo, hi, [](float v) { return fmt("%.1f", v); }),
                help, gap);
}

Item option(const char *label, std::vector<std::string> names, int &field, const char *key, const char *help = nullptr)
{
    return with(ui::choice(
                    label, std::move(names), [&field] { return field; },
                    [&field, key](int v) {
                        field = v;
                        st::set_int(key, v);
                    }),
                help);
}

// the draw distances, a long ordered list read best as a stepper
const int kDraws[] = {0, 6, 7, 8, 9, 10};
constexpr int kDrawCount = sizeof kDraws / sizeof *kDraws;

std::string draw_name(int d)
{
    if (d == 10) return "Unlimited";
    if (d == 0) return "Arcade";
    return std::to_string(d) + " x " + std::to_string(d);
}

const ui::GraphicsHooks &graphics_hooks()
{
    static const ui::GraphicsHooks h = [] {
        ui::GraphicsHooks g;
        g.prefix = "ms.";
        g.start_profile = ui::kFidelity;
        g.host_profile = [](HeadsetKind, int p) {
            return p == ui::kPerformance ? ui::HostProfile{1.0f, 90.f, 1} : ui::HostProfile{1.3f, 120.f, 2};
        };
        g.help_rate = "Frames per second the headset shows.";
        g.help_eye = "More pixels for a smoother picture, at a cost to speed.";
        return g;
    }();
    return h;
}

void build_pages(Pages &p, const Options &o)
{
    // Graphics
    ui::GraphicsRows rows = ui::graphics_rows(o);
    for (const Item &it : rows.profile) p.graphics.push_back(it);
    p.graphics.push_back(with(
        ui::stepper(
            "Draw distance",
            [] {
                for (int k = 0; k < kDrawCount; ++k)
                    if (kDraws[k] == args.draw) return (float)k;
                return 2.f;
            },
            [](float v) {
                args.draw = kDraws[clampi((int)v, 0, kDrawCount - 1)];
                st::set_int("ms.draw", args.draw);
                ui::graphics_by_hand();
            },
            1.f, 0.f, (float)(kDrawCount - 1), [](float v) { return draw_name(kDraws[clampi((int)v, 0, kDrawCount - 1)]); }),
        "How far the scenery is drawn. Medium cost."));
    p.graphics.push_back(option("Car detail", {"Low", "Medium", "High"}, args.car_detail, "ms.cardetail",
                                "Models in the distance."));
    p.graphics.push_back(rows.sharpen);
    p.graphics.push_back(flag("Glass shader", args.glass, "ms.glass", "A depth texture on glass.", !rows.sharpen_shown));
    p.graphics.push_back(tenths("Gamma", args.gamma, "ms.gamma", 0.7f, 1.5f, "Brightens or darkens the middle tones.", true));
    p.graphics.push_back(tenths("Brightness", args.brightness, "ms.brightness", 0.5f, 1.5f, "Scales every colour.", false));
    p.graphics.push_back(flag("FPS counter", args.fps, "ms.fps", "Over the HUD.", true));

    // Audio
    p.audio.push_back(level("Volume", args.volume, "ms.volume"));
    p.audio.push_back(level("Music", args.music, "ms.music"));
    p.audio.push_back(level("Sound Effects", args.effects, "ms.effects"));

    // UI
    p.ui.push_back(flag("HUD", args.hud, "ms.hud"));
    p.ui.push_back(ui::action("Reset to default", [] {
        args.hud = true;
        st::set_bool("ms.hud", true);
    }));

    // Comfort: steppers with a Reset
    p.comfort.push_back(with(ui::stepper(
        "World scale", [] { return args.scale; },
        [](float v) {
            args.scale = v;
            st::set_float("ms.scale", v);
        },
        0.1f, 0.1f, 3.f, [](float v) { return "x" + fmt("%.1f", v); },
        [] {
            args.scale = 1.f;
            st::set_float("ms.scale", 1.f);
        }),
        "Left or right changes it; A resets it."));
    p.comfort.push_back(with(ui::stepper(
        "Dashboard view offset", [] { return (float)args.dash_cm; },
        [](float v) {
            args.dash_cm = (int)v;
            st::set_int("ms.dash", args.dash_cm);
        },
        10.f, -60.f, 60.f,
        [](float v) { return (int)v == 0 ? std::string("Centre") : std::to_string((int)v) + " cm"; },
        [] {
            args.dash_cm = 0;
            st::set_int("ms.dash", 0);
        }),
        "Moves the view along the car. Left or right changes it; A resets it."));

    // Gameplay
    p.gameplay.push_back(flag("Free play", args.free_play, "ms.freeplay"));
    p.gameplay.push_back(option("Difficulty", {kDifficulty[0], kDifficulty[1], kDifficulty[2], kDifficulty[3]},
                                args.difficulty, "ms.difficulty"));
    p.gameplay.push_back(option("Country", {kCountry[0], kCountry[1], kCountry[2]}, args.country, "ms.country"));
    p.gameplay.push_back(front.menu.restart_item(nullptr));

    for (Page *pg : {&p.graphics, &p.audio, &p.ui, &p.comfort, &p.gameplay}) pg->push_back(front.menu.return_item());

    p.top.push_back(ui::action("Resume", [] { front.menu.close(); }));
    p.top.push_back(ui::page("Graphics", p.graphics));
    p.top.push_back(ui::page("Audio", p.audio));
    p.top.push_back(ui::page("UI", p.ui));
    p.top.push_back(ui::page("Comfort", p.comfort));
    p.top.push_back(ui::page("Gameplay", p.gameplay));
    p.top.push_back(ui::action("Recenter", [] { front.menu.close(); }));
    p.top.push_back(with(ui::confirm("Restart",
                                     [] {
                                         front.menu.close();
                                         request_restart();
                                     }),
                         "Starts again from power-on. Set it to Yes, then Confirm Restart.", true));
}

// A box beside the panel: a few lines of text, one on each side
struct Note {
    const char *const *lines;
    int count;
};
const char *const kLeftLines[] = {"RECORDS", "Best lap  1:23.456", "Top speed  210 km/h", "Races  12"};
const char *const kRightLines[] = {"STATUS", "Connected", "Frame  steady"};
constexpr float kLinePx = 30.f;

ui::Box make_box(bool left, const Note &n)
{
    ui::Box b;
    b.left = left;
    b.max_width = 512;
    b.max_height = 256;
    b.measure = [n](float &w, float &h) {
        w = 0.35f;
        h = (float)n.count * kLinePx * 1.3f / ui::Menu::kPxPerM;
        return true;
    };
    b.draw = [n](ImDrawList *dl, float x, float y, float) {
        for (int i = 0; i < n.count; ++i)
            dl->AddText(ImGui::GetFont(), kLinePx, ImVec2(x, y + (float)i * kLinePx * 1.3f),
                        i == 0 ? IM_COL32(255, 255, 255, 255) : IM_COL32(140, 255, 204, 255), n.lines[i]);
    };
    return b;
}

int front_option(int argc, char **argv, int i)
{
    std::string a = argv[i];
    if (a == "--menu") {
        front.open_at_start = true;
        return 1;
    }
    if (a == "--page" && i + 1 < argc) {
        front.open_at_start = true;
        front.start_page = argv[i + 1];
        return 2;
    }
    return 0;
}

void front_load_settings(Options &o)
{
    st::open("vrcore", "menu-sample");
    ui::graphics_load(graphics_hooks(), o);
    args.draw = clampi(st::get_int("ms.draw", 7), 0, 10);
    args.volume = clampi(st::get_int("ms.volume", 100), 0, 100);
    args.music = clampi(st::get_int("ms.music", 100), 0, 100);
    args.effects = clampi(st::get_int("ms.effects", 100), 0, 100);
    args.gamma = clampf(st::get_float("ms.gamma", 1.f), 0.7f, 1.5f);
    args.brightness = clampf(st::get_float("ms.brightness", 1.f), 0.5f, 1.5f);
    args.scale = std::fmax(0.1f, st::get_float("ms.scale", 1.f));
    args.dash_cm = clampi(st::get_int("ms.dash", -30), -60, 60);
    args.difficulty = clampi(st::get_int("ms.difficulty", 1), 0, 3);
    args.country = clampi(st::get_int("ms.country", 0), 0, 2);
    args.car_detail = clampi(st::get_int("ms.cardetail", 2), 0, 2);
    args.free_play = st::get_bool("ms.freeplay", true);
    args.glass = st::get_bool("ms.glass", true);
    args.hud = st::get_bool("ms.hud", true);
    args.fps = st::get_bool("ms.fps", false);
}

void front_headset(const Headset &h, Options &o)
{
    ui::graphics_start(h, o);
}

bool front_init(gpu::Renderer &r, const Options &o)
{
    front.options = o;
    ui::Hooks h;
    h.legend = []() -> const Legend * { return &kControls; };
    h.version = "sample";
    std::vector<ui::Box> boxes = {make_box(true, {kLeftLines, 4}), make_box(false, {kRightLines, 3})};
    bool ok = front.menu.init(r, h, boxes);
    front.pages = new Pages;
    build_pages(*front.pages, o);
    front.menu.set_top(&front.pages->top, "MENU SAMPLE");
    return ok;
}

void front_update(const Tick &t)
{
    const Input &in = input();
    if (front.open_at_start && in.head_tracked) {
        front.open_at_start = false;
        front.menu.open();
        if (!front.start_page.empty() && !front.menu.enter_named(front.start_page.c_str()))
            log("menu: no page starting with \"%s\"", front.start_page.c_str());
    }
    front.menu.update(t, in);
}

void front_prepare(gpu::Frame &f)
{
    front.menu.prepare(f);
}

void front_draw(gpu::Frame &) {}

void front_shutdown(gpu::Renderer &r)
{
    front.menu.shutdown(r);
    delete front.pages;
    front = Front();
}

}

const AppInfo &front_info()
{
    static const AppInfo info = {"vrcore menu", front_init, front_draw, front_shutdown, front_option, front_update,
                                 front_prepare, nullptr, front_load_settings, front_headset};
    return info;
}

}

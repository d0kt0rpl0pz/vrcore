#include "graphics.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "settings.h"

namespace vrcore::ui {

namespace {

namespace st = settings;

const char *const kProfileNames[] = {"Performance", "Fidelity", "Custom"};
const char *const kFoveationNames[] = {"Off", "Low", "Medium", "High"};
const char *const kSharpenNames[] = {"Off", "Normal", "Quality"};

GraphicsHooks g_hooks;
int g_saved_profile = -1;           // the saved profile (-1: none saved); a menu made again (a restart) shows it

// what the rows chose
struct Gfx {
    int profile = kFidelity;
    std::vector<float> rates;       // the list in use (rate_list); rate indexes it
    int rate = 0;
    float eye = 1.3f;
    int foveation = 2;
    int sharpen = 0;
} gfx;

std::string key(const char *name) { return std::string(g_hooks.prefix) + name; }

int clamp_int(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

std::string fmt(const char *f, double v)
{
    char b[64];
    std::snprintf(b, sizeof b, f, v);
    return b;
}

int wrap(int v, int d, int n) { return ((v + d) % n + n) % n; }

// the listed rate nearest hz (a tie: the lower)
int rate_index_for(float hz)
{
    int best = 0;
    const std::vector<float> &r = gfx.rates;
    for (int i = 0; i < (int)r.size(); ++i) {
        float e = std::fabs(r[i] - hz), b = std::fabs(r[best] - hz);
        if (e < b - 0.01f || (e < b + 0.01f && r[i] < r[best])) best = i;
    }
    return best;
}

// profile p's host graphics on this headset (no hook: the values in force, so nothing changes)
HostProfile host_profile(int p)
{
    if (!g_hooks.host_profile) return {gfx.eye, gfx.rates.empty() ? Options().hz : gfx.rates[gfx.rate], gfx.foveation};
    return g_hooks.host_profile(match_headset(headset()), p);
}

// the choices are profile p's
bool matches(int p)
{
    HostProfile hp = host_profile(p);
    return (!g_hooks.game_matches || g_hooks.game_matches(p)) && std::fabs(gfx.eye - hp.eye) < 0.05f &&
           gfx.foveation == hp.foveation && gfx.rate == rate_index_for(hp.hz);
}

// the Graphics in force, asked of the host (it applies what changed)
void apply()
{
    Graphics g;
    g.hz = gfx.rates[gfx.rate];
    g.eye_scale = gfx.eye;
    g.foveation = gfx.foveation;
    g.sharpen = gfx.sharpen;
    request_graphics(g);
}

// ---- the saves ----

void save_rate() { st::set_int(key("rate").c_str(), gfx.rate); }
void save_eye() { st::set_float(key("eyescale").c_str(), gfx.eye); }
void save_foveation() { st::set_int(key("foveation").c_str(), gfx.foveation); }

// the player's own choices kept aside
void save_custom()
{
    if (g_hooks.game_save_custom) g_hooks.game_save_custom();
    st::set_float(key("custom.eyescale").c_str(), gfx.eye);
    st::set_int(key("custom.rate").c_str(), gfx.rate);
    st::set_int(key("custom.foveation").c_str(), gfx.foveation);
}

// a row changed by hand: the profile is Custom
void changed_by_hand()
{
    gfx.profile = g_saved_profile = kCustom;
    st::set_int(key("gfxprofile").c_str(), kCustom);
    save_custom();
}

// profile p chosen, its values in force and saved
void set_profile(int p)
{
    if (gfx.profile == kCustom && p != kCustom && !st::has(key("custom.eyescale").c_str()))
        save_custom();                                  // Custom kept aside at start
    gfx.profile = p;
    if (p == kCustom) {
        if (st::has(key("custom.eyescale").c_str())) {  // back to the player's own
            if (g_hooks.game_load_custom) g_hooks.game_load_custom();
            gfx.eye = clampf(st::get_float(key("custom.eyescale").c_str(), gfx.eye), 1.f, 3.f);
            int r = st::get_int(key("custom.rate").c_str(), gfx.rate);
            if (r >= 0 && r < (int)gfx.rates.size()) gfx.rate = r;
            gfx.foveation = clamp_int(st::get_int(key("custom.foveation").c_str(), gfx.foveation), 0, 3);
        } else {
            save_custom();                              // none yet: what is in force
        }
    } else {
        if (g_hooks.game_apply) g_hooks.game_apply(p);
        HostProfile hp = host_profile(p);
        gfx.eye = hp.eye;
        gfx.rate = rate_index_for(hp.hz);
        gfx.foveation = hp.foveation;
    }
    st::set_int(key("gfxprofile").c_str(), gfx.profile);
    g_saved_profile = gfx.profile;
    if (g_hooks.game_save) g_hooks.game_save();
    save_eye();
    save_rate();
    save_foveation();
    apply();
}

Item item(const char *label, std::function<std::string()> value, std::function<void(int)> change, const char *help,
          bool gap = false)
{
    Item it;
    it.label = label;
    it.value = std::move(value);
    it.change = std::move(change);
    it.help = help;
    it.gap = gap;
    return it;
}

}

void graphics_load(const GraphicsHooks &hooks, Options &o)
{
    g_hooks = hooks;
    g_saved_profile = st::has(key("gfxprofile").c_str()) ? clamp_int(st::get_int(key("gfxprofile").c_str(), 0), 0, 2) : -1;
    o.sharpen = clamp_int(st::get_int(key("sharpen").c_str(), 0), 0, 2);
    // a key never saved leaves the host's own default
    if (st::has(key("rate").c_str())) {
        int i = st::get_int(key("rate").c_str(), -1);
        if (i >= 0 && i < kRateCount) o.hz = kRates[i];
    }
    if (st::has(key("eyescale").c_str())) {
        float e = st::get_float(key("eyescale").c_str(), o.eye_scale);
        o.eye_scale = clampf(e, 1.f, 3.f);
    }
    if (st::has(key("foveation").c_str()))
        o.foveation = clamp_int(st::get_int(key("foveation").c_str(), o.foveation), 0, 3);
}

GraphicsStart graphics_start(const Headset &h, Options &o)
{
    if (!(h.sharpen && h.foveation)) o.sharpen = 0;    // no Sharpening row: a saved value cannot be undone
    int p = g_saved_profile;
    bool fresh = p < 0 && !st::has(key("eyescale").c_str());
    if (fresh) p = g_hooks.start_profile;
    if ((p != kPerformance && p != kFidelity) || !g_hooks.host_profile) {
        if (st::has(key("rate").c_str())) {             // graphics_load read the index through kRates; it is this list's
            std::vector<float> list = rate_list(h);
            int i = st::get_int(key("rate").c_str(), -1);
            o.hz = i >= 0 && i < (int)list.size() ? list[i] : Options().hz;
        }
        return {p, false};
    }
    HostProfile hp = g_hooks.host_profile(match_headset(h), p);
    o.eye_scale = hp.eye;
    o.hz = hp.hz;
    o.foveation = hp.foveation;
    return {p, fresh};
}

GraphicsRows graphics_rows(const Options &o)
{
    gfx = Gfx();
    const Headset &hs = headset();
    gfx.rates = rate_list(hs);
    gfx.sharpen = o.sharpen;            // the flags are applied after the headset hook: `o` is what is in force
    gfx.rate = rate_index_for(o.hz);
    gfx.eye = o.eye_scale;
    gfx.foveation = clamp_int(o.foveation, 0, 3);
    // the saved profile, else the start profile when the choices are its, else Custom
    int start = g_hooks.start_profile;
    gfx.profile = g_saved_profile >= 0 ? g_saved_profile : matches(start) ? start : kCustom;

    GraphicsRows rows;
    rows.sharpen_shown = hs.sharpen && hs.foveation;   // the headset offers sharpening
    rows.profile.push_back(item(
        "Graphics profile", [] { return std::string(kProfileNames[gfx.profile]); },
        [](int d) { set_profile(wrap(gfx.profile, d, 3)); }, g_hooks.help_profile));
    rows.profile.push_back(item(
        "Refresh rate", [] { return fmt("%.0f Hz", gfx.rates[gfx.rate]); },
        [](int d) {
            gfx.rate = wrap(gfx.rate, d, (int)gfx.rates.size());
            save_rate();
            changed_by_hand();
            apply();
        },
        g_hooks.help_rate));
    rows.profile.push_back(item(
        "Eye resolution", [] { return "x" + fmt("%.1f", gfx.eye); },
        [](int d) {
            gfx.eye = clampf((float)std::nearbyint(gfx.eye * 10.f + d) / 10.f, 1.f, 3.f);
            save_eye();
            changed_by_hand();
            apply();
        },
        g_hooks.help_eye));
    rows.profile.push_back(item(
        "Foveated rendering", [] { return std::string(kFoveationNames[gfx.foveation]); },
        [](int d) {
            gfx.foveation = wrap(gfx.foveation, d, 4);
            save_foveation();
            changed_by_hand();
            apply();
        },
        g_hooks.help_foveation));
    rows.sharpen = item(
        "Sharpening", [] { return std::string(kSharpenNames[gfx.sharpen]); },
        [](int d) {
            gfx.sharpen = wrap(gfx.sharpen, d, 3);
            st::set_int(key("sharpen").c_str(), gfx.sharpen);
            apply();
        },
        g_hooks.help_sharpen, true);
    bool shown = rows.sharpen_shown;
    rows.sharpen.shown = [shown] { return shown; };
    return rows;
}

void graphics_by_hand() { changed_by_hand(); }

}

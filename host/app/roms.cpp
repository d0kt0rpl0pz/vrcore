#include "roms.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <set>

#include <SDL3/SDL.h>

#include "app.h"
#include "log.h"
#include "ui/pick.h"
#include "ui/text.h"
#include "ui/textstrings.h"

namespace vrcore::roms {

namespace {

struct Chip {
    std::string name;
    int64_t size;
    uint32_t crc;
};
using Files = std::map<std::string, std::pair<int64_t, uint32_t>>;     // name -> size, CRC

struct Run {
    const Set *set = nullptr;
    std::string where;
    std::vector<Chip> chips;
    bool listed = false;            // the plugin gave a list
} g;

std::vector<Chip> chips_of(const char *s)
{
    std::vector<Chip> out;
    if (!s) return out;
    std::string all = s;
    size_t a = 0;
    while (a < all.size()) {
        size_t b = all.find('\n', a);
        std::string line = all.substr(a, b == std::string::npos ? std::string::npos : b - a);
        size_t t1 = line.find('\t'), t2 = t1 == std::string::npos ? t1 : line.find('\t', t1 + 1);
        if (t2 != std::string::npos)
            out.push_back({line.substr(0, t1), std::strtoll(line.c_str() + t1 + 1, nullptr, 10),
                           (uint32_t)std::strtoul(line.c_str() + t2 + 1, nullptr, 16)});
        if (b == std::string::npos) break;
        a = b + 1;
    }
    return out;
}

uint32_t u16(const uint8_t *b) { return b[0] | b[1] << 8; }
uint32_t u32(const uint8_t *b) { return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24; }

// A zip's files by name, size and CRC from its central directory, as the plugin's reader takes them (a file in a
// folder by its own name, the last of a name kept). False: not a zip that reader can read.
bool zip_files(SDL_IOStream *io, Files &files)
{
    Sint64 len = SDL_GetIOSize(io);
    if (len < 22) return false;
    int tail = (int)std::min<Sint64>(len, 22 + 0xffff);
    std::vector<uint8_t> b(tail);
    if (SDL_SeekIO(io, len - tail, SDL_IO_SEEK_SET) < 0 || SDL_ReadIO(io, b.data(), tail) != (size_t)tail) return false;
    int e = -1;
    for (int i = tail - 22; i >= 0 && e < 0; --i)
        if (u32(&b[i]) == 0x06054b50) e = i;
    if (e < 0) return false;
    uint32_t count = u16(&b[e + 10]);
    int64_t size = u32(&b[e + 12]), at = u32(&b[e + 16]);
    if (at + size > len) return false;
    std::vector<uint8_t> cd(size);
    if (SDL_SeekIO(io, at, SDL_IO_SEEK_SET) < 0 || (size && SDL_ReadIO(io, cd.data(), size) != (size_t)size))
        return false;
    files.clear();
    for (uint32_t k = 0, p = 0; k < count; ++k) {
        if (p + 46 > cd.size() || u32(&cd[p]) != 0x02014b50) return false;
        uint32_t nl = u16(&cd[p + 28]), xl = u16(&cd[p + 30]), cl = u16(&cd[p + 32]);
        if (p + 46 + nl > cd.size()) return false;
        std::string name(reinterpret_cast<const char *>(&cd[p + 46]), nl);
        name = name.substr(name.find_last_of('/') + 1);
        if (!name.empty()) files[name] = {(int64_t)u32(&cd[p + 24]), u32(&cd[p + 16])};
        p += 46 + nl + xl + cl;
    }
    return true;
}

bool exists(const std::string &path)
{
    return SDL_GetPathInfo(path.c_str(), nullptr);
}

[[maybe_unused]] bool is_file(const std::string &path)
{
    SDL_PathInfo info;
    return SDL_GetPathInfo(path.c_str(), &info) && info.type == SDL_PATHTYPE_FILE;
}

// a zip in the folder (null name: none): not there (false, bad false), read (true), or there but unreadable
// (false, bad true)
bool zip_at(const char *name, Files &files, bool &bad)
{
    bad = false;
    if (!name) return false;
    std::string path = g.where + "/" + name;
    if (!exists(path)) return false;
    SDL_IOStream *io = SDL_IOFromFile(path.c_str(), "rb");
    bool ok = io && zip_files(io, files);
    if (io) SDL_CloseIO(io);
    bad = !ok;
    return ok;
}

// the chips held, each taken as the plugin takes it: from the first zip with its name, right only with the list's
// size and CRC
std::set<std::string> held(const Files *a, const Files *b = nullptr)
{
    std::set<std::string> h;
    for (const Chip &c : g.chips)
        for (const Files *z : {a, b}) {
            if (!z) continue;
            auto f = z->find(c.name);
            if (f == z->end()) continue;
            if (f->second.first == c.size && f->second.second == c.crc) h.insert(c.name);
            break;
        }
    return h;
}

bool has(const std::set<std::string> &h, const char *chip) { return chip && h.count(chip); }

#ifdef __ANDROID__
// each pick adopted and copied into the folder; why not, for the player, in `note`. True when one was copied.
bool adopt_all(const std::vector<ui::Pick> &picks, std::string &note)
{
    std::string why;
    bool any = false;
    for (const ui::Pick &p : picks) {
        std::string dest, e = take(p.uri, p.name, dest);
        if (e.empty()) {
            any = true;
            log("roms: %s copied to %s", p.name.c_str(), dest.c_str());
            continue;
        }
        why += (why.empty() ? "" : "\n") + e;
    }
    note = why;
    return any;
}
#endif

}

void use(const Set &set, const std::string &where, const char *list)
{
    g.set = &set;
    g.where = where;
    g.chips = chips_of(list);
    g.listed = !g.chips.empty();        // an empty list (or none with a name, size and CRC) checks nothing
    if (list && !g.listed) log("roms: vr_roms lists no files: the set is not checked");
}

std::string folder()
{
#ifdef __ANDROID__
    const char *files = SDL_GetAndroidExternalStoragePath();
    if (!files) return std::string();
    std::string app = files;
    return app.substr(0, app.find_last_of('/')) + "/roms";
#else
    return std::string();
#endif
}

std::string check()
{
    if (!g.set) return std::string();
    const Set &s = *g.set;
#ifdef __ANDROID__
    SDL_CreateDirectory(g.where.c_str());
    auto in = [](const char *name) { return name && is_file(g.where + "/" + name); };
    if (is_file(g.where) || in(s.seven_z) || in(s.zip) || in(s.parent_zip)) return std::string();
    return message();
#else
    if (exists(g.where)) return std::string();
    return ui::fmt(txt::roms_desktop_missing, {{"set", s.name}, {"path", g.where}});
#endif
}

std::string message()
{
    if (!g.set) return std::string();
    const Set &s = *g.set;
    Files game, parent;
    bool game_bad = false, parent_bad = false;
    bool has_game = zip_at(s.zip, game, game_bad);
    bool has_parent = zip_at(s.parent_zip, parent, parent_bad);
    if (game_bad || parent_bad)
        return ui::fmt(txt::roms_zip_damaged, {{"zip", game_bad ? s.zip : s.parent_zip}});
    if (s.seven_z && exists(g.where + "/" + s.seven_z))
        return ui::fmt(txt::roms_seven_z_unreadable, {{"file", s.seven_z}, {"player", s.player}});
    if (!has_game && !has_parent) {
        if (!s.parent_zip) return ui::fmt(txt::roms_required_one, {{"player", s.player}, {"zip", s.zip}});
        return ui::fmt(txt::roms_required_split, {{"player", s.player}, {"zip", s.zip}, {"parent", s.parent_zip}});
    }
    std::set<std::string> h = held(has_game ? &game : nullptr, has_parent ? &parent : nullptr);
    std::vector<std::string> missing;
    for (const Chip &c : g.chips)
        if (!h.count(c.name)) missing.push_back(c.name);
    if (missing.empty())
        return txt::roms_unreadable;
    if (!has_game && !has(h, s.marker))
        return ui::fmt(txt::roms_parent_only, {{"zip", s.zip}, {"parent", s.parent_zip}, {"title", s.title}});
    for (const Note &n : s.notes)
        if (has(h, n.held) && n.missing && !h.count(n.missing) && n.text) return n.text;
    if (s.parent_zip && !has_parent)
        return ui::fmt(txt::roms_parent_missing,
                       {{"parent", s.parent_zip}, {"zip", s.zip}, {"parent_title", s.parent_title}});
    size_t n = missing.size();
    if (n > 2)
        return ui::fmt(txt::roms_wrong_many, {{"file", missing[0]}, {"others", std::to_string(n - 1)},
                                              {"player", s.player}, {"title_aside", s.title_aside}});
    return ui::fmt(n == 1 ? txt::roms_wrong_one : txt::roms_wrong_two,
                   {{"file", missing[0]}, {"player", s.player}, {"title_aside", s.title_aside}});
}

std::string adopt(const std::string &uri, const std::string &name, std::string &dest,
                  std::vector<std::string> &stale)
{
    dest.clear();
    stale.clear();
    if (!g.set || !g.listed) return ui::fmt(txt::roms_unchecked, {{"file", name}});
    const Set &s = *g.set;
    Files z;
    SDL_IOStream *io = SDL_IOFromFile(uri.c_str(), "rb");
    if (!io) return ui::fmt(txt::roms_open_failed, {{"file", name}});
    bool zip = zip_files(io, z);
    // a 7z by its signature, or by its name: a picked file's name can be a media id with no extension
    uint8_t head[6] = {};
    bool seven = !zip && SDL_SeekIO(io, 0, SDL_IO_SEEK_SET) == 0 && SDL_ReadIO(io, head, 6) == 6 &&
                 !SDL_memcmp(head, "7z\xbc\xaf\x27\x1c", 6);
    SDL_CloseIO(io);
    if (!zip) {
        size_t n = name.size();
        if (!seven && n >= 3 && !SDL_strcasecmp(name.c_str() + n - 3, ".7z")) seven = true;
        if (!s.seven_z || !seven) return ui::fmt(txt::roms_not_zip, {{"file", name}});
        dest = g.where + "/" + s.seven_z;
        return std::string();
    }
    std::set<std::string> h = held(&z);
    auto stands = [&](const char *f) {
        if (f) stale.push_back(g.where + "/" + f);
    };
    if (s.parent_zip && s.parent_marker && z.count(s.parent_marker)) {
        dest = g.where + "/" + s.parent_zip;
        if (h.size() == g.chips.size()) {       // the whole set: nothing else in the folder stands before it
            stands(s.seven_z);
            stands(s.zip);
        }
    } else if (has(h, s.marker)) {
        dest = g.where + "/" + s.zip;
        stands(s.seven_z);                       // the plugin would read it first
    } else if (s.parent_zip) {
        return ui::fmt(txt::roms_not_set_split, {{"file", name}, {"zip", s.zip}, {"parent", s.parent_zip}});
    } else {
        return ui::fmt(txt::roms_not_set, {{"file", name}, {"zip", s.zip}});
    }
    return std::string();
}

std::string take(const std::string &uri, const std::string &name, std::string &dest)
{
    std::vector<std::string> stale;
    std::string e = adopt(uri, name, dest, stale);
    if (!e.empty()) return e;
    std::string c = copy(uri, dest);
    if (!c.empty()) return ui::fmt(txt::roms_copy_failed, {{"file", name}, {"error", c}});
    for (const std::string &f : stale) SDL_RemovePath(f.c_str());     // only once the new file is in place
    return std::string();
}

std::string copy(const std::string &uri, const std::string &dest)
{
    std::string part = dest + ".part";
    SDL_IOStream *in = SDL_IOFromFile(uri.c_str(), "rb");
    if (!in) return SDL_GetError();
    SDL_IOStream *out = SDL_IOFromFile(part.c_str(), "wb");
    if (!out) {
        SDL_CloseIO(in);
        return SDL_GetError();
    }
    std::vector<uint8_t> buf(1 << 20);
    std::string err;
    for (;;) {
        size_t n = SDL_ReadIO(in, buf.data(), buf.size());
        if (n == 0) {
            if (SDL_GetIOStatus(in) != SDL_IO_STATUS_EOF) err = SDL_GetError();
            break;
        }
        if (SDL_WriteIO(out, buf.data(), n) != n) {
            err = SDL_GetError();
            break;
        }
    }
    SDL_CloseIO(in);
    if (!SDL_CloseIO(out) && err.empty()) err = SDL_GetError();
    if (err.empty() && !SDL_RenamePath(part.c_str(), dest.c_str())) err = SDL_GetError();
    if (!err.empty()) SDL_RemovePath(part.c_str());
    return err;
}

std::string pick_row(bool select)
{
#ifdef __ANDROID__
    if (!g.listed) return std::string();    // nothing to know a picked file by: no row
    static std::string note;
    std::vector<ui::Pick> picks;
    std::string err;
    if (ui::pick_take(picks, err)) {
        if (!err.empty()) note = err;
        else if (picks.empty()) note = txt::roms_pick_none;
        else if (adopt_all(picks, note)) {
            log("roms: the set copied: the game starts again");
            request_restart();
        }
    }
    if (select && !ui::pick_busy()) {
        note.clear();
        if (!ui::pick_open(true)) note = txt::roms_pick_open_failed;
    }
    if (ui::pick_busy()) return txt::roms_pick_busy;
    std::string s;
    for (size_t a = 0; !note.empty() && a <= note.size();) {
        size_t b = note.find('\n', a);
        s += "\x02" + note.substr(a, b == std::string::npos ? std::string::npos : b - a) + "\n";
        if (b == std::string::npos) break;
        a = b + 1;
    }
    return (s.empty() ? s : s + "\n") + "\x01" + txt::roms_pick_row;
#else
    (void)select;
    return std::string();
#endif
}

}

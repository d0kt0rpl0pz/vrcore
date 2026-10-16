/*
 * text.cpp -- fmt(), the slot filler for the headset's wording (text.h). One pass over the text; nothing is
 * allocated beyond the result but a logged problem's key. A missing or unused slot is logged once per text and
 * slot name, since a label is filled every frame the menu is open; the set of those already logged is behind a
 * mutex, as a front might fill text from another thread.
 */
#include "text.h"

#include <mutex>
#include <set>

#include "../app/log.h"

namespace vrcore::ui {

namespace {

std::mutex logged_mutex;
std::set<std::string> logged;   // "<problem>\n<slot>\n<text>", each logged once

void log_once(const char *problem, std::string_view slot, const char *text)
{
    std::string key = std::string(problem) + '\n' + std::string(slot) + '\n' + text;
    {
        std::lock_guard<std::mutex> lock(logged_mutex);
        if (!logged.insert(std::move(key)).second) return;
    }
    log("text: %s {%.*s} in \"%s\"", problem, (int)slot.size(), slot.data(), text);
}

}

std::string fmt(const char *text, std::initializer_list<Slot> slots)
{
    if (!text) return {};
    std::string out;
    // which slots the text used, a bit each, for the unused ones (checked up to 32 slots; a front passes a few)
    unsigned used = 0;
    const char *p = text;
    while (*p) {
        if ((p[0] == '{' && p[1] == '{') || (p[0] == '}' && p[1] == '}')) {
            out += p[0];
            p += 2;
            continue;
        }
        if (p[0] != '{') {
            out += *p++;
            continue;
        }
        const char *end = p + 1;
        while ((*end >= 'a' && *end <= 'z') || *end == '_') ++end;
        if (*end != '}' || end == p + 1) {  // not {name}: the { is copied, and the scan goes on after it
            out += *p++;
            continue;
        }
        std::string_view name(p + 1, (size_t)(end - p - 1));
        unsigned bit = 1;
        const Slot *found = nullptr;
        for (const Slot &s : slots) {
            if (s.name == name) {
                found = &s;
                used |= bit;
                break;
            }
            bit <<= 1;
        }
        if (found) {
            out += found->value;
        } else {
            out += '{';
            out += name;
            out += "?}";
            log_once("no value for", name, text);
        }
        p = end + 1;
    }
    unsigned bit = 1;
    for (const Slot &s : slots) {
        if (slots.size() <= 32 && !(used & bit)) log_once("unused value for", s.name, text);
        bit <<= 1;
    }
    return out;
}

int fmt_logged()
{
    std::lock_guard<std::mutex> lock(logged_mutex);
    return (int)logged.size();
}

}

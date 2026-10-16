/*
 * text.h -- fills the named slots in the headset's wording: fmt(txt::x, {{"file", name}}). The wording comes from a
 * textstrings.csv as constants (vrcore_textstrings() in CMakeLists.txt; the host's are vrcore::txt in
 * ui/textstrings.h), whole sentences with slots in them, so the code never joins pieces of English together.
 *
 * THE RULES (the same ones tools/textstrings.py checks the CSV by): {name} is a slot, its name lower case letters
 * and underscores; {{ and }} are a literal { and }. A slot with no value passed is drawn as {name?}, so it shows in
 * the headset, and logged once; a value passed for a slot the text does not have is logged once too. Anything
 * else (a lone brace, or braces round something that is not a slot name, which only a literal written in code
 * can hold) is copied as it is. Nothing here fails.
 *
 * The result is a std::string: keep it (std::string label, a label_fn's return), never pass its c_str() where a
 * const char * is stored (Item::help, group), which needs a constant from the CSV.
 */
#pragma once

#include <initializer_list>
#include <string>
#include <string_view>

namespace vrcore::ui {

// One slot's value. Both views only need to last for the fmt() call, so {"v", std::to_string(n)} is fine.
struct Slot {
    std::string_view name;
    std::string_view value;
};

// `text` with each {name} replaced by its slot's value (nullptr: "").
std::string fmt(const char *text, std::initializer_list<Slot> slots = {});

// How many problems fmt() has logged since the start (each missing or unused slot once, per text): for the test.
int fmt_logged();

}

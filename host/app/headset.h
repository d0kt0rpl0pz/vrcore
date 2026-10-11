/*
 * headset.h -- which headset this is, and the refresh rates its menu offers. A front keys its own per-headset
 * profiles by HeadsetKind (ui/graphics.h's host_profile).
 */
#pragma once

#include <vector>

#include "app.h"

namespace vrcore {

enum class HeadsetKind { Quest3, Quest2, QuestPro, Other };

// By maker and GPU, the first rule that holds: make Oculus or Meta (any case), then Adreno 650 (a Quest Pro when the
// model says Pro, else a Quest 2) or Adreno 740 (Quest 3, and the 3S); anything else is Other.
HeadsetKind match_headset(const Headset &h);
const char *headset_name(HeadsetKind k);
// A Quest 2, 3 or Pro: the menu offers the fixed rate list (kRates).
bool is_quest(HeadsetKind k);

// The rates a Quest lists (a Quest 2 the same; its 60 Hz is not offered)
inline constexpr float kRates[] = {72.f, 80.f, 90.f, 120.f};
constexpr int kRateCount = sizeof kRates / sizeof *kRates;
// The list a saved rate index means and the Refresh rate row steps: a Quest 2, 3 or Pro: kRates less any the runtime
// does not list; any other headset: the runtime's own from 72 Hz up; kRates when none are known (the sim).
std::vector<float> rate_list(const Headset &h);

}

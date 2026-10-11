#include "headset.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <string>

namespace vrcore {

namespace {

bool contains(const std::string &s, const char *part) { return s.find(part) != std::string::npos; }

// (?i)^(Oculus|Meta)
bool quest_make(const std::string &make)
{
    auto starts = [&](const char *w) {
        size_t n = std::strlen(w);
        if (make.size() < n) return false;
        for (size_t i = 0; i < n; ++i)
            if (std::tolower((unsigned char)make[i]) != w[i]) return false;
        return true;
    };
    return starts("oculus") || starts("meta");
}

}

HeadsetKind match_headset(const Headset &h)
{
    if (!quest_make(h.make)) return HeadsetKind::Other;
    if (contains(h.gpu, "Adreno (TM) 650")) return contains(h.model, "Pro") ? HeadsetKind::QuestPro : HeadsetKind::Quest2;
    if (contains(h.gpu, "Adreno (TM) 740")) return HeadsetKind::Quest3;
    return HeadsetKind::Other;
}

const char *headset_name(HeadsetKind k)
{
    switch (k) {
    case HeadsetKind::Quest3: return "Quest 3";
    case HeadsetKind::Quest2: return "Quest 2";
    case HeadsetKind::QuestPro: return "Quest Pro";
    default: return "other";
    }
}

bool is_quest(HeadsetKind k) { return k != HeadsetKind::Other; }

std::vector<float> rate_list(const Headset &h)
{
    std::vector<float> out;
    if (is_quest(match_headset(h))) {
        for (float r : kRates) {
            bool listed = h.rates.empty();
            for (float l : h.rates) listed = listed || std::fabs(l - r) < 0.5f;
            if (listed) out.push_back(r);
        }
    } else {
        for (float l : h.rates)
            if (l >= 71.5f) out.push_back(l);
        std::sort(out.begin(), out.end());
    }
    if (out.empty()) out.assign(kRates, kRates + kRateCount);
    return out;
}

}

/*
 * xr_actions.cpp -- the controllers through OpenXR actions: one action set, each action on both hands
 * (/user/hand/left, /right), into app/input.h's Hands.
 *
 * THE PROFILES. Bindings are suggested for each profile the runtime may pick: oculus/touch_controller (always),
 * meta/touch_controller_plus (XR_META_touch_controller_plus offered: the
 * Quest 3's own), htc/vive_controller, bytedance/pico4_controller and pico_neo3_controller (the Pico two with
 * XR_BD_controller_interaction offered), khr/simple_controller (trigger as select, menu, grip pose), valve/index_controller (SteamVR),
 * valve/frame_controller_valve (XR_VALVE_frame_controller_interaction offered; its paths are not in the pinned
 * registry, 1.1.63, so Index's are suggested and a refusal is only logged). One profile refusing its bindings
 * does not stop the rest. The profile in force is logged when the runtime says it changed.
 *
 * EACH FRAME (after xrWaitFrame, before the front's update): xrSyncActions, the grip and aim poses and the head
 * located in LOCAL at the predicted display time (the time the eyes are drawn for). Unfocused, every hand reads
 * inactive and zero. The aim pose is the one a ray is cast from (the menu's pointer); every suggested profile
 * has /input/aim/pose. The grip "pressed" (`squeezed`) is the Input System's: on at 0.5, off under 0.375.
 * `tracked`: position and orientation both TRACKED (not only valid: a controller out of the cameras' sight
 * keeps an estimated pose for a while, which the wheel must treat as lost).
 */
#include "xr_host.h"

#include <cstring>

#include <SDL3/SDL.h>

#include "../app/input_host.h"
#include "../app/log.h"

namespace vrcore::xr {

struct Actions {
    XrActionSet set = XR_NULL_HANDLE;
    XrAction grip = XR_NULL_HANDLE, aim = XR_NULL_HANDLE, trigger = XR_NULL_HANDLE, squeeze = XR_NULL_HANDLE, stick = XR_NULL_HANDLE,
             stick_click = XR_NULL_HANDLE, primary = XR_NULL_HANDLE, secondary = XR_NULL_HANDLE,
             menu = XR_NULL_HANDLE, haptic = XR_NULL_HANDLE;
    XrPath hand[2] = {XR_NULL_PATH, XR_NULL_PATH};
    XrSpace grip_space[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
    XrSpace aim_space[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
    XrSpace view = XR_NULL_HANDLE;              // the head
};

namespace {

Host *g_host = nullptr;                         // the haptics' way back (input_set_haptics)

XrPath path(Host &h, const char *s)
{
    XrPath p = XR_NULL_PATH;
    if (XR_FAILED(xrStringToPath(h.instance, s, &p))) log("openxr: bad path %s", s);
    return p;
}

bool make_action(Actions &a, XrAction &out, XrActionType type, const char *name, const char *label)
{
    XrActionCreateInfo ci = {XR_TYPE_ACTION_CREATE_INFO};
    ci.actionType = type;
    SDL_strlcpy(ci.actionName, name, XR_MAX_ACTION_NAME_SIZE);
    SDL_strlcpy(ci.localizedActionName, label, XR_MAX_LOCALIZED_ACTION_NAME_SIZE);
    ci.countSubactionPaths = 2;
    ci.subactionPaths = a.hand;
    VRCORE_XR(xrCreateAction(a.set, &ci, &out), false);
    return true;
}

// One binding a hand: the action and the component under /user/hand/<side>.
struct Bind {
    XrAction Actions::*action;
    const char *left, *right;                   // null: none on that hand
};

void suggest(Host &h, Actions &a, const char *profile, const Bind *binds, size_t n)
{
    std::vector<XrActionSuggestedBinding> s;
    char buf[160];
    for (size_t i = 0; i < n; ++i) {
        const char *sub[2] = {binds[i].left, binds[i].right};
        for (int side = 0; side < 2; ++side) {
            if (!sub[side]) continue;
            SDL_snprintf(buf, sizeof buf, "/user/hand/%s%s", side ? "right" : "left", sub[side]);
            s.push_back({a.*(binds[i].action), path(h, buf)});
        }
    }
    XrInteractionProfileSuggestedBinding sb = {XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    sb.interactionProfile = path(h, profile);
    sb.countSuggestedBindings = (uint32_t)s.size();
    sb.suggestedBindings = s.data();
    XrResult r = xrSuggestInteractionProfileBindings(h.instance, &sb);
    log("xr: bindings for %s: %s", profile, XR_SUCCEEDED(r) ? "suggested" : "refused");
    if (XR_FAILED(r)) log("openxr: xrSuggestInteractionProfileBindings(%s): %d", profile, (int)r);
}

void apply_haptic(int hand, float amplitude, float seconds)
{
    Host *h = g_host;
    if (!h || !h->actions || !h->running) return;
    XrHapticActionInfo hi = {XR_TYPE_HAPTIC_ACTION_INFO};
    hi.action = h->actions->haptic;
    hi.subactionPath = h->actions->hand[hand];
    XrHapticVibration v = {XR_TYPE_HAPTIC_VIBRATION};
    v.amplitude = amplitude < 0 ? 0.f : amplitude > 1 ? 1.f : amplitude;
    v.duration = (XrDuration)(seconds * 1e9);
    v.frequency = XR_FREQUENCY_UNSPECIFIED;
    if (v.amplitude <= 0 || v.duration <= 0) {
        xrStopHapticFeedback(h->session, &hi);
        return;
    }
    xrApplyHapticFeedback(h->session, &hi, reinterpret_cast<const XrHapticBaseHeader *>(&v));
}

void stop_haptic(int hand)
{
    Host *h = g_host;
    if (!h || !h->actions || !h->running) return;
    XrHapticActionInfo hi = {XR_TYPE_HAPTIC_ACTION_INFO};
    hi.action = h->actions->haptic;
    hi.subactionPath = h->actions->hand[hand];
    xrStopHapticFeedback(h->session, &hi);
}

float read_float(Host &h, XrAction act, XrPath hand, bool *active = nullptr)
{
    XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = act;
    gi.subactionPath = hand;
    XrActionStateFloat s = {XR_TYPE_ACTION_STATE_FLOAT};
    if (XR_FAILED(xrGetActionStateFloat(h.session, &gi, &s))) return 0;
    if (active) *active |= s.isActive;
    return s.isActive ? s.currentState : 0.f;
}

bool read_bool(Host &h, XrAction act, XrPath hand)
{
    XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = act;
    gi.subactionPath = hand;
    XrActionStateBoolean s = {XR_TYPE_ACTION_STATE_BOOLEAN};
    if (XR_FAILED(xrGetActionStateBoolean(h.session, &gi, &s))) return false;
    return s.isActive && s.currentState;
}

void to_pose(const XrPosef &p, Pose &out)
{
    out.pos[0] = p.position.x;
    out.pos[1] = p.position.y;
    out.pos[2] = p.position.z;
    out.rot = {p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w};
}

constexpr XrSpaceLocationFlags kValid = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
constexpr XrSpaceLocationFlags kTracked =
    XR_SPACE_LOCATION_POSITION_TRACKED_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;

}

bool actions_create(Host &h)
{
    if (!h.actions) h.actions = new Actions;
    Actions &a = *h.actions;
    g_host = &h;
    a.hand[Input::kLeft] = path(h, "/user/hand/left");
    a.hand[Input::kRight] = path(h, "/user/hand/right");

    XrActionSetCreateInfo si = {XR_TYPE_ACTION_SET_CREATE_INFO};
    SDL_strlcpy(si.actionSetName, "gameplay", XR_MAX_ACTION_SET_NAME_SIZE);
    SDL_strlcpy(si.localizedActionSetName, "Gameplay", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE);
    VRCORE_XR(xrCreateActionSet(h.instance, &si, &a.set), false);
    if (!make_action(a, a.grip, XR_ACTION_TYPE_POSE_INPUT, "grip", "Grip pose") ||
        !make_action(a, a.aim, XR_ACTION_TYPE_POSE_INPUT, "aim", "Aim pose") ||
        !make_action(a, a.trigger, XR_ACTION_TYPE_FLOAT_INPUT, "trigger", "Trigger") ||
        !make_action(a, a.squeeze, XR_ACTION_TYPE_FLOAT_INPUT, "squeeze", "Grip") ||
        !make_action(a, a.stick, XR_ACTION_TYPE_VECTOR2F_INPUT, "thumbstick", "Thumbstick") ||
        !make_action(a, a.stick_click, XR_ACTION_TYPE_BOOLEAN_INPUT, "thumbstick_click", "Thumbstick click") ||
        !make_action(a, a.primary, XR_ACTION_TYPE_BOOLEAN_INPUT, "primary", "A / X") ||
        !make_action(a, a.secondary, XR_ACTION_TYPE_BOOLEAN_INPUT, "secondary", "B / Y") ||
        !make_action(a, a.menu, XR_ACTION_TYPE_BOOLEAN_INPUT, "menu", "Menu") ||
        !make_action(a, a.haptic, XR_ACTION_TYPE_VIBRATION_OUTPUT, "haptic", "Vibration"))
        return false;

    // Which of these bind B / Y and the menu button is also in ui/controllers.cpp (THE BUTTONS, for the menu's hint):
    // change both together.
    // Touch, Touch Plus: the same components (the right menu button is the system's)
    static const Bind kTouch[] = {
        {&Actions::grip, "/input/grip/pose", "/input/grip/pose"},
        {&Actions::aim, "/input/aim/pose", "/input/aim/pose"},
        {&Actions::trigger, "/input/trigger/value", "/input/trigger/value"},
        {&Actions::squeeze, "/input/squeeze/value", "/input/squeeze/value"},
        {&Actions::stick, "/input/thumbstick", "/input/thumbstick"},
        {&Actions::stick_click, "/input/thumbstick/click", "/input/thumbstick/click"},
        {&Actions::primary, "/input/x/click", "/input/a/click"},
        {&Actions::secondary, "/input/y/click", "/input/b/click"},
        {&Actions::menu, "/input/menu/click", nullptr},
        {&Actions::haptic, "/output/haptic", "/output/haptic"},
    };
    static const Bind kSimple[] = {
        {&Actions::grip, "/input/grip/pose", "/input/grip/pose"},
        {&Actions::aim, "/input/aim/pose", "/input/aim/pose"},
        {&Actions::trigger, "/input/select/click", "/input/select/click"},
        {&Actions::menu, "/input/menu/click", nullptr},
        {&Actions::haptic, "/output/haptic", "/output/haptic"},
    };
    // Index (and the Frame's controller, until its own paths are in the registry): no menu button
    static const Bind kIndex[] = {
        {&Actions::grip, "/input/grip/pose", "/input/grip/pose"},
        {&Actions::aim, "/input/aim/pose", "/input/aim/pose"},
        {&Actions::trigger, "/input/trigger/value", "/input/trigger/value"},
        {&Actions::squeeze, "/input/squeeze/value", "/input/squeeze/value"},
        {&Actions::stick, "/input/thumbstick", "/input/thumbstick"},
        {&Actions::stick_click, "/input/thumbstick/click", "/input/thumbstick/click"},
        {&Actions::primary, "/input/a/click", "/input/a/click"},
        {&Actions::secondary, "/input/b/click", "/input/b/click"},
        {&Actions::haptic, "/output/haptic", "/output/haptic"},
    };
    // Vive wand: the grip button is a click only (read as a float of 0 or 1), the trackpad is the stick; no A / B.
    static const Bind kVive[] = {
        {&Actions::grip, "/input/grip/pose", "/input/grip/pose"},
        {&Actions::aim, "/input/aim/pose", "/input/aim/pose"},
        {&Actions::trigger, "/input/trigger/value", "/input/trigger/value"},
        {&Actions::stick, "/input/trackpad", "/input/trackpad"},
        {&Actions::stick_click, "/input/trackpad/click", "/input/trackpad/click"},
        {&Actions::squeeze, "/input/squeeze/click", "/input/squeeze/click"},
        {&Actions::menu, "/input/menu/click", nullptr},
        {&Actions::haptic, "/output/haptic", "/output/haptic"},
    };
    // Pico 4 and Neo 3: the Touch layout (A / B and X / Y, thumbstick, menu on the left)
    static const Bind kPico[] = {
        {&Actions::grip, "/input/grip/pose", "/input/grip/pose"},
        {&Actions::aim, "/input/aim/pose", "/input/aim/pose"},
        {&Actions::trigger, "/input/trigger/value", "/input/trigger/value"},
        {&Actions::squeeze, "/input/squeeze/value", "/input/squeeze/value"},
        {&Actions::stick, "/input/thumbstick", "/input/thumbstick"},
        {&Actions::stick_click, "/input/thumbstick/click", "/input/thumbstick/click"},
        {&Actions::primary, "/input/x/click", "/input/a/click"},
        {&Actions::secondary, "/input/y/click", "/input/b/click"},
        {&Actions::menu, "/input/menu/click", nullptr},
        {&Actions::haptic, "/output/haptic", "/output/haptic"},
    };
    constexpr size_t kViveN = sizeof kVive / sizeof kVive[0], kPicoN = sizeof kPico / sizeof kPico[0];
    constexpr size_t kTouchN = sizeof kTouch / sizeof kTouch[0], kSimpleN = sizeof kSimple / sizeof kSimple[0],
                     kIndexN = sizeof kIndex / sizeof kIndex[0];
    suggest(h, a, "/interaction_profiles/oculus/touch_controller", kTouch, kTouchN);
    if (h.ext.touch_plus) suggest(h, a, "/interaction_profiles/meta/touch_controller_plus", kTouch, kTouchN);
    suggest(h, a, "/interaction_profiles/khr/simple_controller", kSimple, kSimpleN);
    suggest(h, a, "/interaction_profiles/valve/index_controller", kIndex, kIndexN);
    // Vive: a core profile. Pico's two belong to XR_BD_controller_interaction (without it the call is refused).
    // Neither was checked on hardware: the log line says "suggested" or "refused".
    suggest(h, a, "/interaction_profiles/htc/vive_controller", kVive, kViveN);
    if (h.ext.pico) {
        suggest(h, a, "/interaction_profiles/bytedance/pico4_controller", kPico, kPicoN);
        suggest(h, a, "/interaction_profiles/bytedance/pico_neo3_controller", kPico, kPicoN);
    }
    if (h.ext.frame_controller) suggest(h, a, "/interaction_profiles/valve/frame_controller_valve", kIndex, kIndexN);
    return true;
}

// After the session: the action set attached, the grip and aim spaces and the head's.
bool actions_attach(Host &h)
{
    Actions &a = *h.actions;
    XrSessionActionSetsAttachInfo ai = {XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    ai.countActionSets = 1;
    ai.actionSets = &a.set;
    VRCORE_XR(xrAttachSessionActionSets(h.session, &ai), false);
    for (int i = 0; i < 2; ++i) {
        XrActionSpaceCreateInfo ci = {XR_TYPE_ACTION_SPACE_CREATE_INFO};
        ci.action = a.grip;
        ci.subactionPath = a.hand[i];
        ci.poseInActionSpace.orientation.w = 1.f;
        VRCORE_XR(xrCreateActionSpace(h.session, &ci, &a.grip_space[i]), false);
        ci.action = a.aim;
        VRCORE_XR(xrCreateActionSpace(h.session, &ci, &a.aim_space[i]), false);
    }
    XrReferenceSpaceCreateInfo vi = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    vi.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    vi.poseInReferenceSpace.orientation.w = 1.f;
    VRCORE_XR(xrCreateReferenceSpace(h.session, &vi, &a.view), false);
    input_set_haptics({apply_haptic, stop_haptic});
    return true;
}

// Each frame after xrWaitFrame: the hands and the head into the snapshot, at `t`. Unfocused: zero.
void actions_sync(Host &h, XrTime t)
{
    Input &in = input_fill();
    Hand was[2] = {in.hands[0], in.hands[1]};
    in.hands[0] = Hand();
    in.hands[1] = Hand();
    in.head_tracked = false;
    Actions *a = h.actions;
    if (!a || !h.running) return;

    XrSpaceLocation hl = {XR_TYPE_SPACE_LOCATION};
    if (XR_SUCCEEDED(xrLocateSpace(a->view, h.local, t, &hl)) && (hl.locationFlags & kValid) == kValid) {
        to_pose(hl.pose, in.head);
        in.head_tracked = true;
    }
    if (h.state != XR_SESSION_STATE_FOCUSED) return;
    XrActiveActionSet as = {a->set, XR_NULL_PATH};
    XrActionsSyncInfo si = {XR_TYPE_ACTIONS_SYNC_INFO};
    si.countActiveActionSets = 1;
    si.activeActionSets = &as;
    if (xrSyncActions(h.session, &si) != XR_SUCCESS) return;    // XR_SESSION_NOT_FOCUSED too

    for (int i = 0; i < 2; ++i) {
        Hand &hd = in.hands[i];
        XrPath p = a->hand[i];
        bool active = false;
        hd.trigger = read_float(h, a->trigger, p, &active);
        hd.squeeze = read_float(h, a->squeeze, p, &active);
        hd.squeezed = was[i].squeezed ? hd.squeeze >= 0.375f : hd.squeeze >= 0.5f;

        XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = a->stick;
        gi.subactionPath = p;
        XrActionStateVector2f v = {XR_TYPE_ACTION_STATE_VECTOR2F};
        if (XR_SUCCEEDED(xrGetActionStateVector2f(h.session, &gi, &v)) && v.isActive) {
            hd.stick[0] = v.currentState.x;
            hd.stick[1] = v.currentState.y;
            active = true;
        }
        hd.stick_click = read_bool(h, a->stick_click, p);
        hd.primary = read_bool(h, a->primary, p);
        hd.secondary = read_bool(h, a->secondary, p);
        hd.menu = read_bool(h, a->menu, p);

        gi.action = a->grip;
        XrActionStatePose ps = {XR_TYPE_ACTION_STATE_POSE};
        if (XR_SUCCEEDED(xrGetActionStatePose(h.session, &gi, &ps)) && ps.isActive) {
            active = true;
            XrSpaceLocation l = {XR_TYPE_SPACE_LOCATION};
            if (XR_SUCCEEDED(xrLocateSpace(a->grip_space[i], h.local, t, &l)) && (l.locationFlags & kValid) == kValid) {
                to_pose(l.pose, hd.grip);
                hd.tracked = (l.locationFlags & kTracked) == kTracked;
            }
        }
        gi.action = a->aim;
        ps = {XR_TYPE_ACTION_STATE_POSE};
        if (XR_SUCCEEDED(xrGetActionStatePose(h.session, &gi, &ps)) && ps.isActive) {
            XrSpaceLocation l = {XR_TYPE_SPACE_LOCATION};
            if (XR_SUCCEEDED(xrLocateSpace(a->aim_space[i], h.local, t, &l)) && (l.locationFlags & kValid) == kValid) {
                to_pose(l.pose, hd.aim);
                hd.aim_tracked = (l.locationFlags & kTracked) == kTracked;
            }
        }
        hd.active = active;
    }
}

// XrEventDataInteractionProfileChanged: the profile now in force on each hand, logged.
void actions_profile_changed(Host &h)
{
    if (!h.actions) return;
    for (int i = 0; i < 2; ++i) {
        XrInteractionProfileState s = {XR_TYPE_INTERACTION_PROFILE_STATE};
        char name[XR_MAX_PATH_LENGTH] = "none";
        if (XR_SUCCEEDED(xrGetCurrentInteractionProfile(h.session, h.actions->hand[i], &s)) &&
            s.interactionProfile != XR_NULL_PATH) {
            uint32_t n = 0;
            xrPathToString(h.instance, s.interactionProfile, sizeof name, &n, name);
        }
        static const char kPrefix[] = "/interaction_profiles/";
        const char *bare = SDL_strncmp(name, kPrefix, sizeof kPrefix - 1) == 0 ? name + sizeof kPrefix - 1
                         : s.interactionProfile == XR_NULL_PATH ? "" : name;
        input_set_profile(i, bare);
        log("xr: %s hand profile %s", i ? "right" : "left", name);
    }
}

void actions_destroy(Host &h)
{
    if (!h.actions) return;
    input_set_haptics({});
    Actions &a = *h.actions;
    for (XrSpace s : a.grip_space)
        if (s) xrDestroySpace(s);
    for (XrSpace s : a.aim_space)
        if (s) xrDestroySpace(s);
    if (a.view) xrDestroySpace(a.view);
    if (a.set) xrDestroyActionSet(a.set);      // its actions with it
    delete h.actions;
    h.actions = nullptr;
    g_host = nullptr;
}

}

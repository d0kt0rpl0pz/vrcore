/*
 * stick.h -- steering with a thumbstick, shaped: a deadzone, a response curve, a range and a wheel's damping, as
 * five menu choices (ported as it is). A game's option, for a game whose own steering maps the stick
 * straight onto the cabinet's wheel (for example the whole of a 270-degree wheel, lock to lock, on about 13 mm of
 * thumb travel, linearly). The hand wheel (wheel.h) is a separate path and is never shaped here.
 *
 * THE CHAIN, the stick's X only (-1..1), in this order:
 *   deadzone  |x| at or under it is 0; past it rescaled, so the output still starts from 0
 *   response  |v|^gamma, the sign kept: fine control near centre, full lock still reached. The words are the
 *             choice and the gammas are behind them: Linear 1.0, Slight 1.3, Medium 1.7, Strong 2.2, Very Strong 3.0
 *   range     times range: the stick's full push gives that share of full lock
 *   damping   the output moves towards the shaped value no faster than a self-centring wheel: `turn` when moving
 *             out from centre, `snapback` when moving back to it. Each is the time from centre to full lock (half
 *             a turn takes half); Instant (0) is no limit. A swing through centre goes back at the snapback rate,
 *             then out at the turn rate.
 *
 * THE GAME'S OWN CURVE. A game's steering can have a dead band at centre and go flat before 32767. The game gives
 * both (gameDead, fullLock: steering values, as the wheel's fullLock), and the output is laid between them: anything
 * past 0 starts just outside the game's dead band, and 1 is the game's full lock. So this deadzone and range are
 * the only ones in force, and Deadzone Off really is off. For example: dead band +-5 ADC counts, flat past 80 of 96
 * (steer 27,306).
 *
 * THE DEFAULTS: Medium, 2%, 80%, 150 ms each way (Modelizer's range and damping, played on a pad). Damping is in
 * milliseconds of real time, so it feels the same at any refresh rate and for any board.
 *
 * THE GAME'S SIDE. Once a frame call steer() with the stick's X (the strongest of the game's sticks, before any
 * deadzone of its own) to get the value to send, every frame, even at rest, so the snapback can run. A non-zero
 * result is analogue steering. Call reset() when the stick is not steering (the hands have it, the menu holds the
 * controls). The choices apply as soon as they change. Saving them comes with the settings (M5), not here.
 */
#pragma once

namespace vrcore {
namespace rig {

// A short text for a menu row (no heap).
struct Label {
    char s[32] = {0};
    const char *c_str() const { return s; }
};

class Stick {
public:
    static constexpr int kResponses = 5;
    static constexpr int kDeadzoneSteps = 7, kRangeSteps = 5, kTimeSteps = 6;
    // Response's value is its index here: add new ones at the end
    static const char *const ResponseNames[kResponses];
    static const int DeadzoneSteps[kDeadzoneSteps];     // % of the stick's travel
    static const int RangeSteps[kRangeSteps];           // % of full lock
    static const int TimeSteps[kTimeSteps];             // ms, centre to full lock; 0 instant
    static constexpr int DefaultResponse = 2, DefaultDeadzone = 2, DefaultRange = 80, DefaultTurn = 150,
                         DefaultSnapback = 150;

    // the choices in force: an index into ResponseNames, then percentages and milliseconds from the step lists
    int response = DefaultResponse, deadzone = DefaultDeadzone, range = DefaultRange, turn = DefaultTurn,
        snapback = DefaultSnapback;

    // the game's steering: the value where it goes flat, and the largest that its centre dead band still reads
    // as straight ahead (0: none)
    int fullLock = 32767, gameDead = 0;

    // the rows' changes
    void step_response(int d);
    void step_deadzone(int d);
    void step_range(int d);
    void step_turn(int d);
    void step_snapback(int d);

    const char *response_name() const { return ResponseNames[response]; }
    Label deadzone_name() const;
    Label range_name() const;
    Label turn_name() const { return time_name(turn); }
    Label snapback_name() const { return time_name(snapback); }

    // Once a frame: the stick's X (-1..1, + = right) and the frame's time (s). The steering to send,
    // -fullLock..fullLock; 0 when the stick is at rest and back at centre.
    int steer(float x, float dt) { return steer(x, dt, range, fullLock); }
    // The same with this frame's range (percent) and full lock in place of the settings' (the selects' full travel).
    int steer(float x, float dt, int range, int fullLock);
    // The stick is not steering: back to centre at once.
    void reset() { now_ = 0.f; }
    // the output now, -1..1 (as last returned by steer)
    float value() const { return now_; }

    // ---- the maths: device-free ----
    // -1..1 onto the game's steering: 0 stays 0, anything else from just past its dead band (dead + 1) to its
    // full lock
    static int to_game(float v, int dead, int full);
    // deadzone, response and range, one stick position; dz and range 0..1
    static float shape(float x, float dz, float gamma, float range);
    // From s towards target over dt seconds: back towards centre at `back`, out from it at `turn` (each full
    // lock per second, 0 instant). Through centre: back to 0, then the rest of dt out.
    static float damp(float s, float target, float dt, float turn, float back);
    // the gamma behind a response index
    static float gamma(int response);

private:
    static Label time_name(int ms);
    float now_ = 0.f;                                   // the damped output, -1..1
};

}
}

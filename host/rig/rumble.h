/*
 * rumble.h -- a strength as vibration, on the controller being driven with (ported as it is).
 *
 * A game with a force the hands should feel (a cabinet's wheel motor, a hit) gives its size, 0..1, once a frame
 * (update), and which controller is in use: the gamepad (USB or Bluetooth) that last gave an input, or 0 for the
 * headset's controllers. Which one that is comes out of the game's own control mapping, so the game keeps it.
 * Only that controller vibrates; on a switch the old one goes quiet first.
 *
 * THE DEVICES (input.h's outputs). A pad's motors hold a speed until told otherwise, so they are set only when
 * the strength changes (pad_rumble's low motor; the other stays 0; renewing it for SDL is the host's job). A
 * Touch controller's impulse lasts its duration, so it is renewed every frame (0.1 s each, both hands) and ends
 * by itself when the frames stop. With a side, the hands share it: -1 the left hand alone, 1 the right alone, 0
 * both at the full strength; a pad has one strength for both. A pad that is gone (unplugged, Bluetooth lost) is
 * dropped, for the game too (`used` comes back 0): it does not vibrate again when it returns until it gives an
 * input.
 *
 * THE SPRING (optional, off unless `spring`). A cabinet's centring torque, felt as vibration, for a game whose
 * board sends no steering torque (another drive board sends only contacts and a coarse weight). The cabinet's
 * own torque is a centring spring: 0 straight ahead, rising quickly with the steering and saturating at 27 of 63
 * at either lock, stiffer with speed. Here: SpringMax * (1 - (1 - |steer|)^2) * (SpringFloor + (1 - SpringFloor)
 * * speed), the steering's share easing out (0.75 at half lock). The game gives the steering it sent (steer,
 * -1..1, the hand wheel's or a pad's) and the car's speed (speed, 0..1 of its top; negative: full; 0 outside a
 * race keeps the attract quiet). It is felt in both hands and layered on the game's strength: each hand (or the
 * pad) gets the stronger of the two, so a contact still stands out above it. A game with that torque leaves it off: its
 * strength is the torque itself.
 *
 * THE PULSE (pulse): a one-off strength for a moment, on a side (a lever going into gear, the shifter's tick).
 * Layered as the spring is, each hand (or the pad) the strongest; only while driving.
 *
 * THE GAME'S SIDE. The game hands update() its state as a plain struct each
 * frame (Inputs), and the time in seconds (any steady clock). The strength is used only
 * while `driving` (the game has the controls: its menu closed, no free camera); otherwise it is 0. The game
 * calls stop() when it shuts down: a pad's motors would run on.
 */
#pragma once

#include <cstdint>

namespace vrcore {
namespace rig {

class Rumble {
public:
    // What the game gives each frame.
    struct Inputs {
        bool driving = true;            // the game has the controls; the strength and the spring count only then
        float strength = 0.f;           // the game's, 0..1
        float side = 0.f;               // where it is felt: -1 left hand .. 1 right hand, 0 both
        float steer = 0.f;              // the steering sent, -1..1 (the spring's)
        float speed = -1.f;             // the car's speed 0..1 of its top; negative: full
    };

    // Where the vibration goes. The defaults are input.h's (the host's); a test replaces them.
    struct Out {
        bool (*pad_there)(uint32_t id);                         // the pad is still connected
        void (*pad)(uint32_t id, float low, float high);        // its motors
        void (*touch)(int hand, float amplitude, float seconds);    // a Touch controller's impulse
        void (*stop_touch)();                                   // both Touch controllers quiet now
    };

    Rumble();
    void set_out(const Out &o) { out_ = o; }

    // the spring: on (the game's menu choice); its strength at full lock and full speed (27 of 63, say),
    // and the share of that left at a standstill
    bool spring = false;
    float SpringMax = 27.f / 63.f, SpringFloor = 0.25f;

    // the spring's strength now, 0..1 (0 when off); speed negative: full
    float spring_strength(float steer, float speed) const;

    // A pulse of `strength` 0..1 for `seconds` from `now`, on `side` (-1 left hand .. 1 right, 0 both; a pad has
    // one strength). A new one replaces the last.
    void pulse(float strength, float seconds, double now, float side = 0.f);

    // Once a frame, after the game read its controls. `used`: the pad in use (an id), 0 for the Touch controllers.
    void update(const Inputs &in, double now, uint32_t &used);

    void stop();

    // the strength last sent, 0..1 (for the tests and the stats)
    float level() const { return level_; }

private:
    float pulse_ = 0.f, pulse_side_ = 0.f;
    double pulse_until_ = 0.0;          // THE PULSE: its strength, side and end (the caller's clock)
    uint32_t pad_ = 0;                  // the pad whose motors were last set
    float level_ = 0.f;                 // the strength last sent
    bool touch_ = false;                // the Touch controllers have an impulse running
    Out out_;
};

}
}

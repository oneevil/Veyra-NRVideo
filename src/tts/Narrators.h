// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// The narrators the voice offers, each with a person's name, and how a speaker is given one.
//
// TeraTTSv2 has eight ready voices. Each man's and woman's voice also gives two
// more narrators: its speech made 8 % slower or faster and then played as much faster or slower,
// like a tape, so the pace stays and the voice sounds like a larger or a smaller person
// (TeraVoice::synthesize). That makes 21 narrators: 9 men, 12 women (and one man's voice pitched
// between the sexes, chosen by hand only).
//
// A speaker is given a narrator of the right sex that nobody else has, the closest in pitch to
// the voice heard in the game when that is known, and preferably not a variant of a narrator
// already speaking (variants of one voice still sound related).
// Portable C++, no Qt.
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace veyra::tts {

struct Narrator {
    std::string id;       // "eng_m4", "ru_f1_low"
    std::string style;    // the TeraTTSv2 style it is made from: "eng_m4", "ru_f1"
    float ratio = 1.0f;   // < 1 lower and slower-played, > 1 higher
    bool female = false;
    bool byHandOnly = false; // never handed out on its own
    float pitch = 0.0f;   // typical pitch, Hz
    std::string name;     // as the window calls it: "Виктор"
};

// Every narrator, in the order they are handed out: the voices as recorded first.
const std::vector<Narrator>& narrators();
const Narrator* findNarrator(std::string_view id);
// The narrators that speak when nobody chose one: a man's and a woman's.
const Narrator& defaultNarrator(bool female);

// A narrator for a new speaker. `taken`: the narrators other speakers already have.
std::string pickNarrator(bool female, const std::vector<std::string>& taken, std::optional<float> pitch = {});

// The typical pitch of a voice in Hz (autocorrelation over its louder frames), or nothing when
// too little of it is voiced. Mono samples at `rate`.
std::optional<float> medianPitch(const std::vector<float>& wave, int rate);

// `wave` played `ratio` times faster (pitch and pace together, like a tape).
std::vector<float> playFaster(const std::vector<float>& wave, float ratio);

} // namespace veyra::tts

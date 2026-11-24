// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Who speaks, and which narrator reads for each (the list of voices).
//
// A member is one of: "" every line (the voice is not listened to), "@male" / "@female" the two
// narrators of lines nobody named, "name:<name>" a character named in the subtitles, "#<voice>"
// a voice recognised in the game's sound ("Голос 2"). Each has a choice - "auto", "mute" or a
// narrator id - and, for "auto", the narrator it was given: the defaults for the first three,
// for the others a narrator of their sex that nobody else reads with, the closest in pitch
// (Narrators.h).
//
// The sex of a character is settled so: guessed (from the name) when it
// is first met, then decided once by the opening of its first line that could be heard, and after
// that changed only when its whole lines, counted one by one, say the other sex. Each line's
// opening on its own is too short and too noisy to go by: deciding by it again and again gave
// one character different narrators line after line.
// Portable C++, no Qt.
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace veyra::tts {

struct CastMember {
    std::string key, title;
    std::optional<bool> female;
    std::optional<float> pitch;
    std::string choice = "auto";   // "auto", "mute" or a narrator id
    std::string fallback;          // the narrator "auto" means for this member
    uint64_t lastSpoke = 0;        // order of the latest line (0: never)
    bool heard = false;            // the narrator was chosen from how the speaker really sounds
    int misses = 0;                // lines of theirs in which no voice could be made out
    int votes = 0;                 // whole lines so far: above zero a woman, below a man
};

class Cast {
public:
    // The member for `key`, created when new (`created` tells); the reference holds until the
    // next call that adds or removes a member. `female` and `pitch` choose the narrator of a new
    // member (a guess, until `hear`); an existing one keeps its narrator.
    CastMember& member(const std::string& key, const std::string& title, std::optional<bool> female,
                       std::optional<float> pitch, bool* created = nullptr);
    // Not heard yet: its narrator is still a guess, so its line is worth holding back a moment
    // to hear who is talking.
    bool needsHearing(const std::string& key) const;
    // The opening of a line of it, as heard (`female` empty: no voice in it). Decides the sex
    // once; twice nothing audible and the guess stays.
    void hear(const std::string& key, std::optional<bool> female, std::optional<float> pitch);
    // A whole line of it was heard: a vote for its sex. When the votes come to say the other sex
    // than its narrator's, an automatic narrator is chosen anew.
    void confirm(const std::string& key, bool female, std::optional<float> pitch);
    const CastMember* find(const std::string& key) const;
    // A member kept from an earlier session, as it was (nothing is picked anew); an existing
    // member of that key is left alone.
    void restore(CastMember member);
    // The narrator that reads for it now; empty when muted.
    std::string narrator(const CastMember& member) const;
    // Marks a line of it as just spoken (the list shows the latest speakers first).
    void spoke(CastMember& member) { member.lastSpoke = ++lines_; }
    void setChoice(const std::string& key, const std::string& choice);
    // A recognised voice merged into another, or the session over.
    void remove(const std::string& key);
    void removeIf(bool (*test)(const CastMember&));
    const std::vector<CastMember>& members() const { return members_; }

private:
    void pick(CastMember& member);
    CastMember* get(const std::string& key);
    static bool named(const std::string& key) { return !key.empty() && key.front() != '@'; }

    std::vector<CastMember> members_;
    uint64_t lines_ = 0;
};

} // namespace veyra::tts

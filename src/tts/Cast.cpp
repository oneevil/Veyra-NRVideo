// SPDX-License-Identifier: GPL-3.0-only
#include "Cast.h"
#include "Narrators.h"

#include <algorithm>

namespace veyra::tts {

CastMember& Cast::member(const std::string& key, const std::string& title, std::optional<bool> female,
                         std::optional<float> pitch, bool* created) {
    auto found = std::find_if(members_.begin(), members_.end(), [&](const CastMember& m) { return m.key == key; });
    const bool isNew = found == members_.end();
    if (created) *created = isNew;
    if (isNew) {
        CastMember fresh;
        fresh.key = key;
        fresh.title = title;
        fresh.female = female;
        fresh.pitch = pitch;
        members_.push_back(std::move(fresh));
        pick(members_.back());
        return members_.back();
    }
    auto& m = *found;
    if (!title.empty()) m.title = title;
    if (pitch && !m.pitch) m.pitch = pitch;
    if (!m.female && female) m.female = female;
    return m;
}

void Cast::restore(CastMember member) {
    if (find(member.key)) return;
    member.lastSpoke = 0;
    if (!findNarrator(member.fallback)) pick(members_.emplace_back(std::move(member)));
    else members_.push_back(std::move(member));
}

CastMember* Cast::get(const std::string& key) {
    for (auto& m : members_) if (m.key == key) return &m;
    return nullptr;
}

bool Cast::needsHearing(const std::string& key) const {
    const auto* m = find(key);
    return !m || (named(key) && m->choice == "auto" && !m->heard);
}

void Cast::hear(const std::string& key, std::optional<bool> female, std::optional<float> pitch) {
    auto* m = get(key);
    if (!m || !named(key) || m->heard) return;
    if (!female) {
        m->heard = ++m->misses >= 2;
        return;
    }
    if (pitch) m->pitch = pitch;
    const auto* current = findNarrator(m->fallback);
    const bool guessedRight = current && current->female == *female;
    m->female = female;
    // (A line already spoken in a guessed narrator of the right sex keeps it: a character should
    // not change voice without a reason.)
    if (!(m->misses && guessedRight)) pick(*m);
    m->heard = true;
    m->votes = *female ? 1 : -1;
}

void Cast::confirm(const std::string& key, bool female, std::optional<float> pitch) {
    auto* m = get(key);
    if (!m || !named(key)) return;
    if (pitch && !m->pitch) m->pitch = pitch;
    const auto* current = findNarrator(m->fallback);
    if (m->votes == 0 && current) m->votes = current->female ? 1 : -1;
    m->votes = std::clamp(m->votes + (female ? 2 : -2), -6, 6);
    const bool wanted = m->votes > 0;
    m->female = wanted;
    m->heard = true;
    if (m->choice == "auto" && (!current || current->female != wanted)) pick(*m);
}

const CastMember* Cast::find(const std::string& key) const {
    for (const auto& m : members_) if (m.key == key) return &m;
    return nullptr;
}

std::string Cast::narrator(const CastMember& member) const {
    if (member.choice == "mute") return {};
    if (member.choice != "auto" && findNarrator(member.choice)) return member.choice;
    return member.fallback;
}

void Cast::setChoice(const std::string& key, const std::string& choice) {
    for (auto& m : members_)
        if (m.key == key) m.choice = choice == "mute" || findNarrator(choice) ? choice : "auto";
}

void Cast::remove(const std::string& key) {
    std::erase_if(members_, [&](const CastMember& m) { return m.key == key; });
}

void Cast::removeIf(bool (*test)(const CastMember&)) {
    std::erase_if(members_, [&](const CastMember& m) { return test(m); });
}

void Cast::pick(CastMember& member) {
    if (member.key.empty() || member.key == "@male") { member.fallback = defaultNarrator(false).id; return; }
    if (member.key == "@female") { member.fallback = defaultNarrator(true).id; return; }
    std::vector<std::string> taken;
    for (const auto& other : members_) {
        if (&other == &member) continue;
        const auto voice = narrator(other);
        if (!voice.empty()) taken.push_back(voice);
    }
    member.fallback = pickNarrator(member.female.value_or(false), taken, member.pitch);
}

} // namespace veyra::tts

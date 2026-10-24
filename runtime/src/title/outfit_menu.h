#pragma once

#include <cstddef>
#include <cstdint>
#include <array>
#include <map>
#include <set>
#include <string>

namespace cafe::title::outfit_menu {

// Entry storage starts at +92, with the pending flag at +324. The native
// builder appends customization rows only while count < 14 (033E379C),
// before our hook hides them. Ten presets therefore leave four such rows.
constexpr size_t kMaxPresets = 14;
static_assert(92 + 16 * kMaxPresets <= 324);

// Preflight the whole namespace before registering anything, including packs
// appearing after an outfit owner. Reject both owners of a duplicate code.
class CodeOwners {
public:
    void add(size_t owner, const std::string& code) {
        const auto [it, inserted] = owners_.emplace(code, owner);
        if (!inserted) {
            conflicts_.insert(owner);
            conflicts_.insert(it->second);
        }
    }
    bool conflicts(size_t owner) const { return conflicts_.contains(owner); }

private:
    std::map<std::string, size_t> owners_;
    std::set<size_t> conflicts_;
};

// State shared by loader and recipe hooks. A menu can reuse a slot without
// another loader call; discard that slot's appearance when its owner changes.
class BodySelections {
public:
    void remember(uint32_t slot, uint32_t fighter, uint32_t body) {
        if (slot < slots_.size()) slots_[slot] = {fighter, body};
    }
    uint32_t resolve(uint32_t slot, uint32_t fighter) {
        if (slot >= slots_.size()) return 0;
        auto& selection = slots_[slot];
        if (selection.fighter != fighter) selection = {};
        return selection.body;
    }

private:
    struct Selection { uint32_t fighter = 0, body = 0; };
    std::array<Selection, 5> slots_{};
};

// Native chooser confirmations in sub_033E39B0. Other calls to the pick
// function include opening CHANGE COSTUME: their button value must survive.
constexpr bool confirming(uint32_t return_address) {
    return return_address == 0x033E3B30 || return_address == 0x033E3BDC;
}

constexpr uint32_t native_style(size_t outfit_count, uint32_t return_address, uint32_t input) {
    return outfit_count && confirming(return_address) ? 1 : input;
}

// Regression: forcing 1 for an opening request silently commits the fighter.
static_assert(native_style(7, 0x033E2000, 0) == 0);
static_assert(native_style(7, 0x033E2000, 2) == 2);
static_assert(native_style(7, 0x033E3B30, 7) == 1);
static_assert(native_style(7, 0x033E3BDC, 4) == 1);
static_assert(native_style(0, 0x033E3BDC, 4) == 4);
static_assert(native_style(9, 0x033E3BDC, 9) == 1);

// A loader slot can be reused by the menu without another loadPlayer call.
// A remembered appearance must never replace that new recipe's fighter.
constexpr bool matches_fighter(uint32_t incoming, uint32_t owner) {
    return incoming == owner;
}

static_assert(matches_fighter(74, 74));
static_assert(!matches_fighter(60, 74));
static_assert(!matches_fighter(75, 74));

constexpr bool needs_reload(uint32_t previous_code, uint32_t next_code,
                            bool pending, uint32_t current_record, uint32_t requested_record) {
    return previous_code != 0 && previous_code != next_code && !pending && current_record == requested_record;
}

static_assert(needs_reload(1, 2, false, 160, 160));
static_assert(!needs_reload(1, 1, false, 160, 160));
static_assert(!needs_reload(1, 2, true, 160, 160));
static_assert(!needs_reload(1, 2, false, 0, 160));
static_assert(!needs_reload(0, 2, false, 160, 160));

} // namespace cafe::title::outfit_menu

#include "dusklight_online/game/save_recovery.hpp"

#include <cassert>

using dusklight_online::game::RecoveryFact;
using dusklight_online::game::SaveRecoveryDelta;
using nlohmann::json;

int main() {
    const json saved = {
        {"event_flags", {12}},
        {"chests", {{{"stage", 3}, {"flags", {1}}}}},
        {"key_counts", {{{"stage", 3}, {"count", 2}}}},
        {"rupees", 100},
    };
    SaveRecoveryDelta journal;
    journal.begin(saved);

    json before = saved;
    before["event_flags"].push_back(55); // A local change before the remote sync.
    json after = before;
    after["event_flags"].push_back(77);
    after["chests"][0]["flags"].push_back(4);
    after["key_counts"][0]["count"] = 1;
    after["rupees"] = 80;
    journal.observe(before, after);

    auto restore = journal.applicable(saved);
    assert(restore.size() == 4);
    assert(restore.at(RecoveryFact{"event_flags", 77}) == true);
    assert(!restore.contains(RecoveryFact{"event_flags", 55}));
    assert(restore.at(RecoveryFact{"chests", 3, 4}) == true);
    assert(restore.at(RecoveryFact{"key_counts", 3})["count"] == 1);
    assert(restore.at(RecoveryFact{"rupees"}) == 80);

    // A newer live wallet update must win over the journal.
    json loadedWithNewerSync = saved;
    loadedWithNewerSync["rupees"] = 90;
    assert(!journal.applicable(loadedWithNewerSync).contains(RecoveryFact{"rupees"}));

    // Returning a fact to the saved value removes it from the journal.
    json reversed = after;
    reversed["rupees"] = 100;
    journal.observe(after, reversed);
    assert(!journal.applicable(saved).contains(RecoveryFact{"rupees"}));

    // A received clear is represented as an absent fact.
    json cleared = reversed;
    cleared["event_flags"] = json::array({55, 77});
    journal.observe(reversed, cleared);
    assert(journal.applicable(saved).at(RecoveryFact{"event_flags", 12}).is_null());
}

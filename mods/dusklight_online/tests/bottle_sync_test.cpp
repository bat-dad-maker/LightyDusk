#include "dusklight_online/game/bottle_sync.hpp"
#include <array>
#include <set>
#include <stdexcept>

using namespace dusklight_online::game;
void require(bool value) {
    if (!value) throw std::runtime_error("Bottle source merge regression");
}
struct Player {
    int bottles = 0;
    std::set<int> sources;
    void receive(int source, int count) {
        sources.insert(source);
        bottles = merged_bottle_count(bottles, count, sources.size());
    }
    void catch_up(const Player& peer) {
        sources.insert(peer.sources.begin(), peer.sources.end());
        bottles = merged_bottle_count(bottles, peer.bottles, sources.size());
    }
};
int main() {
    // Recorded failure: one slot, no recorded source, stale complete=true.
    // The arriving oil reward used to turn that existing slot into two.
    require(!bottle_sources_exact(true, 0, 1));
    require(merged_bottle_count(1, 1, 1) == 1);
    std::array<Player, 5> players;
    for (auto& p : players) p.bottles = 1;
    for (int reload = 0; reload < 20; ++reload) {
        // A sender reloading a pre-reward save can issue a new sequence for
        // the same reward. Source identity, rather than sequence, dedupes it.
        for (auto& p : players) p.receive(102, 1);
        for (auto& p : players)
            for (const auto& peer : players) p.catch_up(peer);
        for (const auto& p : players) require(p.bottles == 1);
    }
    // Two independent known rewards still merge even if each sender had one.
    players[0].receive(96, 1);
    players[1].receive(101, 1);
    for (auto& p : players)
        for (const auto& peer : players) p.catch_up(peer);
    for (auto& p : players) p.catch_up(players[0]);
    for (const auto& p : players) require(p.bottles == 3 && p.sources.size() == 3);
    // A reloaded client catches up from the source union without multiplying.
    Player reloaded;
    reloaded.catch_up(players[0]);
    reloaded.receive(102, 1);
    require(reloaded.bottles == 3);
    require(merged_bottle_count(3, 1, 1) == 3); // Older save cannot remove slots.
    require(merged_bottle_count(1, 2, 0) == 2); // Legacy absolute catch-up.
    require(merged_bottle_count(4, 1, 4) == 4);
    require(bottle_sources_exact(true, 3, 3));
    require(!bottle_sources_exact(false, 3, 3));
}

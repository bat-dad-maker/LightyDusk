#include "dusklight_online/game/audio_lifetime.hpp"
#include <cassert>
#include <vector>

using dusk::multiplayer::RemoteAudioEvent;
using dusklight_online::game::AudioLifetimeTracker;

int main() {
    AudioLifetimeTracker tracker;
    RemoteAudioEvent scream;
    scream.sequence = 1;
    scream.soundId = 0x10000;
    scream.tracked = true;
    std::vector<RemoteAudioEvent> snapshot{scream};
    auto changes = tracker.update(snapshot);
    assert(changes.start[0] == 0);
    // Repeated packets, including after the receiver's sample naturally
    // finishes, must never generate the short second playback.
    for (int frame = 0; frame < 120; ++frame) {
        changes = tracker.update(snapshot);
        for (int start : changes.start) assert(start == -1);
        for (bool stop : changes.stop) assert(!stop);
    }
    // Water/landing/any native interruption removes the old instance.
    changes = tracker.update({});
    assert(changes.stop[0]);
    changes = tracker.update(snapshot);
    for (int start : changes.start) assert(start == -1);
    // A genuinely new occurrence of the same sample is still audible.
    snapshot[0].sequence = 2;
    assert(tracker.update(snapshot).start[0] == 0);
    // Replacement voice stops the old handle before starting its replacement.
    snapshot[0].sequence = 3;
    snapshot[0].soundId++;
    changes = tracker.update(snapshot);
    assert(changes.stop[0] && changes.start[0] == 0);
    // Charge and voice are independent and snapshot ordering is irrelevant.
    RemoteAudioEvent charge = scream;
    charge.sequence = 5;
    charge.soundId = 0x20025;
    scream.sequence = 4;
    snapshot = {charge, scream};
    changes = tracker.update(snapshot);
    assert(changes.start[0] == 0 && changes.start[1] == 1);
    snapshot.erase(snapshot.begin());
    changes = tracker.update(snapshot);
    assert(changes.stop[0] && !changes.stop[1]);
    // Existing clawshot-style level sounds never enter the one-shot tracker.
    RemoteAudioEvent level;
    level.soundId = 123;
    level.level = true;
    snapshot.push_back(level);
    changes = tracker.update(snapshot);
    for (int start : changes.start) assert(start == -1);
    tracker.clear();
    changes = tracker.update(snapshot);
    for (int start : changes.start) assert(start == -1);
}

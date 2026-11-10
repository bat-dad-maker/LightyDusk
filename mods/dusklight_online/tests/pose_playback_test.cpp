#include "dusklight_online/game/pose_playback.hpp"
#include "dusklight_online/game/pose_prediction.hpp"
#include <random>
#include <cassert>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <map>
#include <vector>
using dusklight_online::game::PosePlayback;

struct Movement {
    double position = 0, start = 0, target = 0;
    int age = 0, duration = 1;
    void sample(double value) {
        start = position; target = value;
        duration = std::clamp(age + 1, 1, 4); age = 0;
    }
    double update() {
        const double previous = position;
        if (age < duration) ++age;
        position = start + (target - start) * double(age) / duration;
        return position - previous;
    }
};

int main() {
    for (int latency : {0, 5, 9, 18}) {
        // 0/167/300/600ms one-way latency; batches of three followed by
        // three individual arrivals. All samples retain their sender sequence.
        std::map<int, std::vector<int>> arrivals;
        for (int seq = 1; seq < 900; ++seq) {
            const int phase = seq % 6;
            const int jitter = phase >= 3 ? 6 - phase : 0;
            arrivals[seq + latency + jitter].push_back(seq);
        }
        PosePlayback<int> playback(3, false);
        Movement buffered, latest;
        double bufferedError = 0, latestError = 0;
        for (int tick = 1; tick < 880; ++tick) {
            for (int seq : arrivals[tick]) playback.push(seq, seq);
            if (!arrivals[tick].empty()) latest.sample(arrivals[tick].back());
            if (auto sample = playback.update()) buffered.sample(*sample);
            const double b = buffered.update(), l = latest.update();
            if (tick > 100) { bufferedError += std::abs(b - 1); latestError += std::abs(l - 1); }
        }
        assert(bufferedError < 0.001);
        assert(latestError > 100);
        std::cout << "one-way ticks=" << latency << " movement error: latest="
                  << latestError << " buffered=" << bufferedError << '\n';
    }
    // Exercise the production predictor and playback clock with stops/reversals,
    // loss and burst delivery at several fixed one-way delays.
    using Pose = dusk::multiplayer::PeerPoseSnapshot;
    const auto makePose = [](uint32_t seq, float x) {
        Pose p; p.valid = true; p.sequence = seq; p.x = x; p.procId = 4;
        p.visualMode = Pose::VisualMode::SemanticGameplay;
        p.bodyRootValid = true; p.bodyRootX = x + 2;
        p.underBck0 = 1; p.underFrame0 = float(seq); p.underRate0 = 1;
        return p;
    };
    const auto predict = [](const Pose& a, const Pose& b, int64_t ahead, Pose& out) {
        return dusklight_online::game::predict_pose(a, b, ahead, out);
    };
    double oldError = 0, newError = 0;
    for (int latency : {0, 5, 9, 18}) for (int lossPercent : {0, 2, 5, 10})
    for (int motion : {0, 1, 2}) for (int phase : {0, 1, 2, 3, 4, 5}) {
        const auto position = [motion, phase](int seq) {
            const int tick = std::max(0, seq + phase);
            if (motion == 0) return float(tick);
            const int cycle = tick / 180, part = tick % 180;
            return motion == 1 ? float(cycle * 90 + std::min(part, 90)) :
                                  float(part <= 90 ? part : 180 - part);
        };
        std::mt19937 random(721);
        std::map<int, std::vector<uint32_t>> arrivals;
        for (uint32_t seq = 1; seq < 900; ++seq) {
            if (int(random() % 100) < lossPercent) continue;
            const int jitter = seq % 6 >= 3 ? 6 - seq % 6 : 0;
            arrivals[seq + latency + jitter].push_back(seq);
        }
        PosePlayback<Pose> old(3, false), current;
        Movement baseline, candidate;
        uint32_t accepted = 0;
        double oldCase = 0, newCase = 0;
        for (int tick = 1; tick < 880; ++tick) {
            for (auto seq : arrivals[tick]) {
                if (seq <= accepted) continue;
                accepted = seq;
                auto p = makePose(seq, position(seq));
                old.push(seq, p); current.push(seq, p);
            }
            if (auto p = old.update()) baseline.sample(p->x);
            if (auto p = current.update(predict)) candidate.sample(p->x);
            baseline.update(); candidate.update();
            assert(current.size() <= current.capacity);
            assert(current.delay_ticks() >= 1 && current.delay_ticks() <= 3);
            if (tick > 120 + latency) {
                oldCase += std::abs(baseline.position - position(tick - latency));
                newCase += std::abs(candidate.position - position(tick - latency));
            }
        }
        assert(newCase < oldCase);
        oldError += oldCase; newError += newCase;
    }
    std::cout << "Production prediction matrix (288 scenarios): tracking error baseline="
              << oldError << " candidate=" << newError << '\n';
    Pose a = makePose(1, 0), b = makePose(2, 1), projected;
    b.lanternVisualValid = b.lanternLinkAnchored = true; b.lanternX = 7;
    b.boomerangVisualValid = true; b.boomerangX = 99; // detached projectile stays authoritative
    b.audioEvents.push_back({}); b.activeAudioEvents.push_back({});
    assert(predict(a, b, 2, projected));
    assert(projected.x == 3 && projected.bodyRootX == 5 && projected.lanternX == 9);
    assert(projected.boomerangX == 99 && projected.sequence == b.sequence);
    assert(projected.audioEvents.empty() && projected.activeAudioEvents.empty());
    assert(projected.underFrame0 == 4 && b.underFrame0 == 2);
    // Pause and other stops may leave a movement proc and animation rate set.
    // Repeated held samples must neither move nor advance that old clip.
    Pose stopped = b;
    stopped.sequence = 3;
    assert(!predict(b, stopped, 1, projected));
    stopped.x = b.x + 1;
    stopped.bodyRootX = stopped.x + 2;
    assert(predict(b, stopped, 1, projected));
    assert(projected.underFrame0 == stopped.underFrame0);
    stopped.x = b.x;
    stopped.underFrame0 += 1;
    assert(predict(b, stopped, 1, projected));
    assert(projected.x == stopped.x && projected.underFrame0 == stopped.underFrame0 + 1);
    Pose upperOnly = b;
    upperOnly.underRatio0 = 0;
    upperOnly.upperBck0 = 7;
    upperOnly.upperFrame0 = 2;
    Pose upperNext = upperOnly;
    upperNext.sequence = upperOnly.sequence + 1;
    upperNext.upperFrame0 = 3;
    assert(predict(upperOnly, upperNext, 1, projected));
    assert(projected.upperFrame0 == 4);
    upperNext.upperBck0 = 8;
    assert(!predict(upperOnly, upperNext, 1, projected));
    assert(!predict(a, b, 3, projected)); // hard horizon
    a.procId = 5; assert(!predict(a, b, 1, projected)); a.procId = b.procId;
    a.stage = "different"; assert(!predict(a, b, 1, projected)); a.stage = b.stage;
    b.isTransforming = true; assert(!predict(a, b, 1, projected)); b.isTransforming = false;
    b.x = 1000; assert(!predict(a, b, 1, projected)); b.x = 1;
    b.linkMatrices.valid = true; assert(!predict(a, b, 1, projected));
    // Sustained starvation grows headroom; stable delivery returns it to one.
    PosePlayback<int> adaptive;
    adaptive.push(1, 1);
    for (int tick = 0; tick < 12; ++tick) adaptive.update();
    assert(adaptive.delay_ticks() == 3);
    for (int seq = 2; seq < 1000; ++seq) {
        adaptive.push(seq, seq); adaptive.update();
    }
    assert(adaptive.delay_ticks() == 1);

    // A shorter delivery path must not leave a permanent pose backlog.
    for (int advance : {1, 6, 12}) for (int delay : {1, 3}) {
        PosePlayback<int> route(delay, false);
        int received = 100, displayed = 0;
        for (int tick = 0; tick < 30; ++tick) {
            route.push(received, received); ++received;
            if (auto sample = route.update()) displayed = *sample;
        }
        assert(received - 1 - displayed == delay);
        for (int n = 0; n < advance; ++n) { route.push(received, received); ++received; }
        for (int tick = 0; tick < 120; ++tick) {
            route.push(received, received); ++received;
            if (auto sample = route.update()) {
                assert(*sample > displayed && *sample - displayed <= 2);
                displayed = *sample;
            }
        }
        assert(received - 1 - displayed == delay);
        assert(route.delay_ticks() == delay);
    }
    // A transient early burst, followed by the corresponding arrival gap,
    // must retain every sample rather than trigger catch-up.
    PosePlayback<int> burst(1, false);
    int lastBurst = 0;
    for (int tick = 1; tick <= 60; ++tick) {
        if (tick == 20) for (int seq = 20; seq <= 23; ++seq) burst.push(seq, seq);
        else if (tick < 20 || tick > 23) burst.push(tick, tick);
        if (auto sample = burst.update()) { assert(*sample == lastBurst + 1); lastBurst = *sample; }
    }

    // Missing packets do not block playback or repeat events.
    PosePlayback<int> loss(3, false);
    int previous = 0, received = 0;
    for (int tick = 1; tick < 100; ++tick) {
        if (tick < 90 && tick % 10 != 0) loss.push(tick, tick);
        if (auto sample = loss.update()) { assert(*sample > previous); previous = *sample; ++received; }
    }
    assert(received == 81);
    // A stopped sender cannot create an unbounded queue; a restarted sender
    // is rebuffered instead of replaying an old backlog.
    PosePlayback<int> recovery(3, false);
    recovery.push(1, 1);
    for (int i = 0; i < 100; ++i) recovery.update();
    recovery.push(2, 2);
    for (int i = 0; i < 3; ++i) assert(!recovery.update());
    assert(recovery.update() == 2);
    for (int i = 3; i < 10000; ++i) recovery.push(i, i);
    assert(recovery.size() <= PosePlayback<int>::capacity);
    recovery = PosePlayback<int>(3, false);
    recovery.push(1, 42);
    for (int i = 0; i < 3; ++i) assert(!recovery.update());
    assert(recovery.update() == 42);
}

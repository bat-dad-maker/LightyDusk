#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <utility>

namespace dusklight_online::game {
// The sequence clock is independent of round-trip latency. Retain receive
// bursts; adapt playback headroom only when samples repeatedly arrive late.
template<class Pose>
class PosePlayback {
public:
    static constexpr int64_t maxPredictionTicks = 2;
    static constexpr size_t capacity = 16;
    explicit PosePlayback(int64_t delay = 1, bool adaptive = true)
        : delay_(std::clamp(delay, int64_t{1}, int64_t{3})), adaptive_(adaptive) {}

    void push(uint32_t sequence, Pose pose) {
        const int64_t tick = sequence;
        if (!started_ || tick > cursor_ + static_cast<int64_t>(capacity) ||
            tick < cursor_ - (maxPredictionTicks + 1)) {
            pending_.clear(); previous_.reset(); latest_.reset();
            cursor_ = tick - delay_;
            started_ = true;
            lateTicks_ = stableTicks_ = excessTicks_ = 0;
        }
        pending_.emplace_back(tick, std::move(pose));
        while (pending_.size() > capacity) pending_.pop_front();
    }

    std::optional<Pose> update() {
        return update([](const Pose&, const Pose&, int64_t, Pose&) { return false; });
    }

    template<class Predictor>
    std::optional<Pose> update(Predictor predict) {
        predicted_ = false;
        if (!started_) return std::nullopt;
        // A shorter path can move the arrival clock ahead without starving
        // playback. Recover that excess separately from adaptive jitter delay.
        // Six consecutive updates distinguish sustained backlog from brief
        // bursts; advance at most one extra tick per six updates.
        if (!pending_.empty() && pending_.back().first - cursor_ > delay_) {
            if (++excessTicks_ >= 6) { ++cursor_; excessTicks_ = 0; }
        } else {
            excessTicks_ = 0;
        }
        std::optional<Pose> result;
        while (!pending_.empty() && pending_.front().first <= cursor_) {
            previous_ = std::move(latest_);
            latest_ = std::move(pending_.front());
            pending_.pop_front();
            result = latest_->second;
        }
        const int64_t ahead = latest_ ? cursor_ - latest_->first : 0;
        if (latest_ && previous_ && ahead > 0 && ahead <= maxPredictionTicks) {
            Pose projected = latest_->second;
            if (predict(previous_->second, latest_->second, ahead, projected)) {
                result = std::move(projected);
                predicted_ = true;
            }
        }
        // Sustained lateness increases headroom one tick at a time. A lone
        // delayed packet must not force every subsequent pose to wait longer.
        lateTicks_ = ahead > 1 ? lateTicks_ + 1 : 0;
        stableTicks_ = ahead == 0 ? stableTicks_ + 1 : 0;
        if (adaptive_ && lateTicks_ >= 3 && delay_ < 3) {
            ++delay_; lateTicks_ = stableTicks_ = 0;
        } else {
            ++cursor_;
            // Remove only one tick after ten seconds of uninterrupted samples.
            if (adaptive_ && stableTicks_ >= 300 && delay_ > 1) {
                --delay_; ++cursor_; stableTicks_ = 0;
            }
        }
        return result;
    }
    size_t size() const { return pending_.size(); }
    int64_t delay_ticks() const { return delay_; }
    bool predicted() const { return predicted_; }
private:
    bool started_ = false, predicted_ = false;
    int64_t cursor_ = 0, delay_ = 1;
    bool adaptive_ = true;
    uint32_t lateTicks_ = 0, stableTicks_ = 0, excessTicks_ = 0;
    std::deque<std::pair<int64_t, Pose>> pending_;
    std::optional<std::pair<int64_t, Pose>> previous_, latest_;
};
} // namespace dusklight_online::game

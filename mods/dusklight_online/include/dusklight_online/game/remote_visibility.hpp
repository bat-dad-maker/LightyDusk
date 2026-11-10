#pragma once

namespace dusklight_online::game {

inline constexpr unsigned kUnsupportedEventPresentation = 1u << 3;

// A requested stage is still live during the native exit walk/fade. Peek is
// the overlap manager's scene-swap barrier, after the outgoing fade finishes.
constexpr bool remote_link_transition_hidden(bool nextStageRequested,
                                             bool overlapPeek, bool loadExit) {
    return overlapPeek || (nextStageRequested && !loadExit);
}

// Apply independently to sender and receiver. Cleanup is allowed only for a
// previously observed permitted event, never just because isOrderOK is true.
constexpr bool remote_link_scene_hidden(bool sceneChanging, bool eventRunning,
                                       bool talking, bool loadEntrance,
                                       bool eventPresentation, bool allowedEventCleanup = false) {
    return sceneChanging ||
           (eventRunning && !allowedEventCleanup && !talking && !loadEntrance && !eventPresentation);
}

struct RemoteLinkEventVisibility {
    bool allowedEvent = false;
    int allowedEventId = -1;
    bool presentationEventLatched = false;

    constexpr void reset() {
        allowedEvent = false;
        allowedEventId = -1;
        presentationEventLatched = false;
    }

    constexpr bool hidden(bool sceneChanging, bool eventRunning, bool eventOrderOK,
                          bool talking, bool loadEntrance, bool eventPresentation,
                          int eventId = -1) {
        if (sceneChanging || !eventRunning) {
            reset();
            return sceneChanging;
        }

        // Door/item-get procedure markers can disappear during the same event's
        // handoff. Keep the exception tied to that event identity rather than
        // to the transient command or procedure marker. A different event
        // takes ownership immediately and cannot inherit the exception.
        if (presentationEventLatched && allowedEventId >= 0 && eventId >= 0 &&
            allowedEventId != eventId) {
            reset();
        }
        if (eventPresentation) {
            allowedEvent = true;
            if (eventId >= 0) {
                presentationEventLatched = true;
                allowedEventId = eventId;
            }
        } else if (presentationEventLatched) {
            // Keep a recognised presentation event visible while its markers
            // hand off, including the order-OK cleanup window.
            allowedEvent = true;
        } else if (!eventOrderOK) {
            // Preserve the existing one-sample TALK/START exceptions. Unlike a
            // door/item-get event, they have no local ownership latch here.
            allowedEvent = !remote_link_scene_hidden(false, true, talking,
                                                      loadEntrance, false);
        }
        return !allowedEvent;
    }
};

} // namespace dusklight_online::game

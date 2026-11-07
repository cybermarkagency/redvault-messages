#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>

namespace redvault::lk {

/**
 * Mixes the audio of everyone else in a group call into the one stream the
 * speaker plays. Each participant has a lane the media library fills (48 kHz
 * mono, in small frames as they arrive); the speaker pulls a block at a time.
 *
 * A lane waits until it holds a little audio before it starts playing, so
 * ordinary jitter doesn't chop the voice, and starts over after an underrun.
 * One that falls too far behind drops its oldest audio rather than grow a
 * delay. Thread-safe: the library's thread pushes, the audio thread mixes.
 */
class AudioMixer {
public:
    /// Samples a lane holds before it starts playing (30 ms).
    static constexpr std::size_t kPrebufferSamples = 48000 * 30 / 1000;
    /// The most a lane keeps (240 ms); older audio is dropped.
    static constexpr std::size_t kMaxBufferedSamples = 48000 * 240 / 1000;

    /// Adds audio from `participant` (mono, 48 kHz).
    void push(const std::string& participant, const std::int16_t* samples, std::size_t count);
    /// Forgets a participant who left (or whose track ended).
    void remove(const std::string& participant);
    void clear();
    /// Fills `out` with the sum of every lane (silence when there's nothing), clipping instead of wrapping.
    void mix(std::int16_t* out, std::size_t count);
    /// Participants with audio waiting right now (for tests and diagnostics).
    std::size_t activeLanes() const;

private:
    struct Lane {
        std::deque<std::int16_t> samples;
        bool playing = false;
    };
    mutable std::mutex mutex_;
    std::map<std::string, Lane> lanes_;
};

}  // namespace redvault::lk

#include "redvault/groupcall/audio_mixer.hpp"

#include <algorithm>
#include <vector>

namespace redvault::lk {

void AudioMixer::push(const std::string& participant, const std::int16_t* samples, std::size_t count) {
    std::lock_guard lock(mutex_);
    Lane& lane = lanes_[participant];
    lane.samples.insert(lane.samples.end(), samples, samples + count);
    if (lane.samples.size() > kMaxBufferedSamples) {
        lane.samples.erase(lane.samples.begin(), lane.samples.begin() + static_cast<std::ptrdiff_t>(lane.samples.size() - kMaxBufferedSamples));
    }
}

void AudioMixer::remove(const std::string& participant) {
    std::lock_guard lock(mutex_);
    lanes_.erase(participant);
}

void AudioMixer::clear() {
    std::lock_guard lock(mutex_);
    lanes_.clear();
}

void AudioMixer::mix(std::int16_t* out, std::size_t count) {
    std::vector<std::int32_t> sum(count, 0);
    {
        std::lock_guard lock(mutex_);
        for (auto& [name, lane] : lanes_) {
            if (!lane.playing && lane.samples.size() >= kPrebufferSamples) lane.playing = true;
            if (!lane.playing) continue;
            const std::size_t take = std::min(count, lane.samples.size());
            for (std::size_t i = 0; i < take; ++i) sum[i] += lane.samples[i];
            lane.samples.erase(lane.samples.begin(), lane.samples.begin() + static_cast<std::ptrdiff_t>(take));
            if (take < count) lane.playing = false;  // ran dry: wait for a little audio again
        }
    }
    for (std::size_t i = 0; i < count; ++i) out[i] = static_cast<std::int16_t>(std::clamp<std::int32_t>(sum[i], -32768, 32767));
}

std::size_t AudioMixer::activeLanes() const {
    std::lock_guard lock(mutex_);
    std::size_t n = 0;
    for (const auto& [name, lane] : lanes_) n += lane.playing || !lane.samples.empty() ? 1 : 0;
    return n;
}

}  // namespace redvault::lk

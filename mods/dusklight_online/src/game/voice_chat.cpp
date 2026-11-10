#include "dusklight_online/game/voice_chat.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <utility>

#if defined(DUSKLIGHT_ONLINE_VOICE)
#include <SDL3/SDL_audio.h>
#include <opus.h>
#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#endif

namespace dusklight_online::game {

struct VoiceChat::Impl {
    std::string lastError;
#if defined(DUSKLIGHT_ONLINE_VOICE)
    static constexpr int sampleRate = 48000;
    static constexpr int frameSamples = 960;
    static constexpr int playbackChannels = 2;
    static constexpr int prebufferFrames = 4;
    static constexpr int maxQueuedFrames = 15;
    static constexpr size_t maxPacketBytes = 400;
    static constexpr float captureBoost = 2.5f;
    static constexpr float playbackBoost = 6.0f;
    using DeviceList = decltype(&SDL_GetAudioRecordingDevices);
    using DeviceName = decltype(&SDL_GetAudioDeviceName);
    using DeviceFormat = decltype(&SDL_GetAudioDeviceFormat);
    using OpenDevice = decltype(&SDL_OpenAudioDevice);
    using CloseDevice = decltype(&SDL_CloseAudioDevice);
    using CreateStream = decltype(&SDL_CreateAudioStream);
    using BindStream = decltype(&SDL_BindAudioStream);
    using OpenStream = decltype(&SDL_OpenAudioDeviceStream);
    using DestroyStream = decltype(&SDL_DestroyAudioStream);
    using ResumeStream = decltype(&SDL_ResumeAudioStreamDevice);
    using PauseStream = decltype(&SDL_PauseAudioStreamDevice);
    using Available = decltype(&SDL_GetAudioStreamAvailable);
    using Queued = decltype(&SDL_GetAudioStreamQueued);
    using ClearStream = decltype(&SDL_ClearAudioStream);
    using ReadStream = decltype(&SDL_GetAudioStreamData);
    using WriteStream = decltype(&SDL_PutAudioStreamData);
    using StreamGain = decltype(&SDL_SetAudioStreamGain);
    using Free = decltype(&SDL_free);
    DeviceList listDevices = nullptr;
    DeviceName deviceName = nullptr;
    DeviceFormat deviceFormat = nullptr;
    OpenDevice openDevice = nullptr;
    CloseDevice closeDevice = nullptr;
    CreateStream createStream = nullptr;
    BindStream bindStream = nullptr;
    OpenStream openStream = nullptr;
    DestroyStream destroyStream = nullptr;
    ResumeStream resumeStream = nullptr;
    PauseStream pauseStream = nullptr;
    Available available = nullptr;
    Queued queued = nullptr;
    ClearStream clearStream = nullptr;
    ReadStream readStream = nullptr;
    WriteStream writeStream = nullptr;
    StreamGain streamGain = nullptr;
    Free freeMemory = nullptr;
    bool sdlLoaded = false;
    SDL_AudioStream* captureStream = nullptr;
    SDL_AudioDeviceID captureDevice = 0;
    OpusEncoder* encoder = nullptr;
    std::string inputName;
    bool enabled = false;
    bool openAttempted = false;
    int micVolume = 100;
    int playerVolume = 100;
    float masterGain = 1.0f;
    int targetPrebufferFrames = prebufferFrames;
    uint32_t nextSequence = 0;
    struct Peer {
        OpusDecoder* decoder = nullptr;
        SDL_AudioStream* stream = nullptr;
        uint32_t sequence = 0;
        bool playing = false;
        std::chrono::steady_clock::time_point lastArrival{};
        int prebufferFrames = Impl::prebufferFrames;
    };
    std::map<std::string, Peer> peers;

    bool load_sdl() {
        if (sdlLoaded) return true;
#if defined(_WIN32)
        HMODULE module = GetModuleHandleA("SDL3.dll");
        if (module == nullptr) { lastError = "SDL3 is unavailable"; return false; }
        const auto resolve = [module](const char* name) {
            return reinterpret_cast<void*>(GetProcAddress(module, name));
        };
#else
        void* module = dlopen("libSDL3.so.0", RTLD_NOW | RTLD_NOLOAD);
        if (module == nullptr) module = dlopen("libSDL3.so.0", RTLD_NOW);
        if (module == nullptr) { lastError = "SDL3 is unavailable"; return false; }
        const auto resolve = [module](const char* name) { return dlsym(module, name); };
#endif
        listDevices = reinterpret_cast<DeviceList>(resolve("SDL_GetAudioRecordingDevices"));
        deviceName = reinterpret_cast<DeviceName>(resolve("SDL_GetAudioDeviceName"));
        deviceFormat = reinterpret_cast<DeviceFormat>(resolve("SDL_GetAudioDeviceFormat"));
        openDevice = reinterpret_cast<OpenDevice>(resolve("SDL_OpenAudioDevice"));
        closeDevice = reinterpret_cast<CloseDevice>(resolve("SDL_CloseAudioDevice"));
        createStream = reinterpret_cast<CreateStream>(resolve("SDL_CreateAudioStream"));
        bindStream = reinterpret_cast<BindStream>(resolve("SDL_BindAudioStream"));
        openStream = reinterpret_cast<OpenStream>(resolve("SDL_OpenAudioDeviceStream"));
        destroyStream = reinterpret_cast<DestroyStream>(resolve("SDL_DestroyAudioStream"));
        resumeStream = reinterpret_cast<ResumeStream>(resolve("SDL_ResumeAudioStreamDevice"));
        pauseStream = reinterpret_cast<PauseStream>(resolve("SDL_PauseAudioStreamDevice"));
        available = reinterpret_cast<Available>(resolve("SDL_GetAudioStreamAvailable"));
        queued = reinterpret_cast<Queued>(resolve("SDL_GetAudioStreamQueued"));
        clearStream = reinterpret_cast<ClearStream>(resolve("SDL_ClearAudioStream"));
        readStream = reinterpret_cast<ReadStream>(resolve("SDL_GetAudioStreamData"));
        writeStream = reinterpret_cast<WriteStream>(resolve("SDL_PutAudioStreamData"));
        streamGain = reinterpret_cast<StreamGain>(resolve("SDL_SetAudioStreamGain"));
        freeMemory = reinterpret_cast<Free>(resolve("SDL_free"));
        sdlLoaded = listDevices && deviceName && openStream && destroyStream &&
            resumeStream && pauseStream && available && queued && clearStream &&
            readStream && writeStream && streamGain && freeMemory;
        if (!sdlLoaded) lastError = "SDL3 audio functions are unavailable";
        return sdlLoaded;
    }

    void close_capture() {
        if (captureStream != nullptr) { destroyStream(captureStream); captureStream = nullptr; }
        if (captureDevice != 0) { closeDevice(captureDevice); captureDevice = 0; }
        if (encoder != nullptr) { opus_encoder_destroy(encoder); encoder = nullptr; }
    }
    void close_peer(Peer& peer) {
        if (peer.stream != nullptr) { destroyStream(peer.stream); peer.stream = nullptr; }
        if (peer.decoder != nullptr) { opus_decoder_destroy(peer.decoder); peer.decoder = nullptr; }
    }
    bool open_capture() {
        if (!load_sdl()) return false;
        SDL_AudioDeviceID device = SDL_AUDIO_DEVICE_DEFAULT_RECORDING;
        if (!inputName.empty()) {
            int count = 0;
            SDL_AudioDeviceID* devices = listDevices(&count);
            for (int i = 0; devices != nullptr && i < count; ++i) {
                const char* name = deviceName(devices[i]);
                if (name != nullptr && inputName == name) { device = devices[i]; break; }
            }
            if (devices != nullptr) freeMemory(devices);
        }
        const SDL_AudioSpec spec{SDL_AUDIO_S16, 1, sampleRate};
#if defined(_WIN32)
        // Open selected non-48 kHz devices at their own clock, then let SDL
        // convert capture to the 48 kHz format required by Opus.
        SDL_AudioSpec preferred{};
        if (device != SDL_AUDIO_DEVICE_DEFAULT_RECORDING && deviceFormat &&
            openDevice && closeDevice && createStream && bindStream &&
            deviceFormat(device, &preferred, nullptr) && preferred.freq != sampleRate) {
            captureDevice = openDevice(device, &preferred);
            if (captureDevice != 0) {
                captureStream = createStream(nullptr, &spec);
                if (captureStream == nullptr || !bindStream(captureDevice, captureStream))
                    close_capture();
            }
        }
#endif
        if (captureStream == nullptr)
            captureStream = openStream(device, &spec, nullptr, nullptr);
        if (captureStream == nullptr) { lastError = "Could not open microphone"; return false; }
        int error = OPUS_OK;
        encoder = opus_encoder_create(sampleRate, 1, OPUS_APPLICATION_VOIP, &error);
        if (encoder == nullptr || error != OPUS_OK) {
            lastError = "Could not start voice codec"; close_capture(); return false;
        }
        opus_encoder_ctl(encoder, OPUS_SET_BITRATE(20000));
        opus_encoder_ctl(encoder, OPUS_SET_DTX(1));
        if (!resumeStream(captureStream)) {
            lastError = "Could not start microphone"; close_capture(); return false;
        }
        lastError.clear();
        return true;
    }
#endif
};

VoiceChat::VoiceChat() : impl_(std::make_unique<Impl>()) {}
VoiceChat::~VoiceChat() { stop(); }

std::vector<std::string> VoiceChat::input_devices() {
    std::vector<std::string> result;
#if defined(DUSKLIGHT_ONLINE_VOICE)
    if (!impl_->load_sdl()) return result;
    int count = 0;
    SDL_AudioDeviceID* devices = impl_->listDevices(&count);
    for (int i = 0; devices != nullptr && i < count; ++i) {
        const char* name = impl_->deviceName(devices[i]);
        if (name != nullptr && *name != '\0') result.emplace_back(name);
    }
    if (devices != nullptr) impl_->freeMemory(devices);
#endif
    return result;
}

void VoiceChat::configure(bool enabled, bool micMuted, std::string_view input, int micPercent,
                          int playerPercent, bool directMode, float masterGain) {
#if defined(DUSKLIGHT_ONLINE_VOICE)
    impl_->micVolume = std::clamp(micPercent, 0, 200);
    impl_->playerVolume = std::clamp(playerPercent, 0, 200);
    impl_->masterGain = std::isfinite(masterGain) ?
        std::clamp(masterGain, 0.0f, 1.0f) : 1.0f;
    impl_->targetPrebufferFrames = directMode ? 6 : Impl::prebufferFrames;
    if (!enabled) { stop(); return; }
    if (!impl_->enabled || impl_->inputName != input) {
        impl_->close_capture();
        impl_->inputName = input;
        impl_->enabled = true;
        impl_->openAttempted = false;
    }
    if (micMuted) {
        // Closing capture drops buffered speech without stopping peer playback.
        impl_->close_capture();
        impl_->openAttempted = false;
        return;
    }
    if (!impl_->openAttempted) {
        impl_->openAttempted = true;
        (void)impl_->open_capture();
    }
#else
    (void)enabled; (void)micMuted; (void)input; (void)micPercent; (void)playerPercent;
    (void)directMode; (void)masterGain;
#endif
}

std::vector<VoiceFrame> VoiceChat::capture() {
    std::vector<VoiceFrame> frames;
#if defined(DUSKLIGHT_ONLINE_VOICE)
    if (impl_->captureStream == nullptr || impl_->encoder == nullptr) return frames;
    // A game stall should not make newly spoken audio play seconds late.
    std::array<int16_t, Impl::frameSamples> discarded{};
    while (impl_->available(impl_->captureStream) >
           10 * int(sizeof(discarded))) {
        if (impl_->readStream(impl_->captureStream, discarded.data(),
                              sizeof(discarded)) != sizeof(discarded)) break;
    }
    for (int i = 0; i < 8 &&
         impl_->available(impl_->captureStream) >= Impl::frameSamples * int(sizeof(int16_t)); ++i) {
        std::array<int16_t, Impl::frameSamples> pcm{};
        if (impl_->readStream(impl_->captureStream, pcm.data(), sizeof(pcm)) != sizeof(pcm)) break;
        const float micGain = (impl_->micVolume / 100.0f) * Impl::captureBoost;
        for (int16_t& sample : pcm) {
            const int scaled = int(std::lround(sample * micGain));
            sample = static_cast<int16_t>(std::clamp(scaled, -32768, 32767));
        }
        std::array<uint8_t, Impl::maxPacketBytes> packet{};
        const int count = opus_encode(impl_->encoder, pcm.data(), Impl::frameSamples,
                                      packet.data(), packet.size());
        // Let Opus DTX carry quiet frames. Dropping individual 20 ms frames
        // at an RMS threshold chopped off consonants and emptied playback.
        if (count <= 0 || count > int(packet.size())) continue;
        frames.push_back({++impl_->nextSequence,
            std::vector<uint8_t>(packet.begin(), packet.begin() + count)});
    }
#endif
    return frames;
}

void VoiceChat::receive(std::string_view peerId, uint32_t sequence,
                        std::span<const uint8_t> encoded, float proximityGain, float pan) {
#if defined(DUSKLIGHT_ONLINE_VOICE)
    if (!impl_->enabled || !impl_->load_sdl() || peerId.empty() || sequence == 0 ||
        encoded.empty() || encoded.size() > Impl::maxPacketBytes) return;
    auto& peer = impl_->peers[std::string(peerId)];
    const auto now = std::chrono::steady_clock::now();
    if (peer.lastArrival == std::chrono::steady_clock::time_point{}) {
        peer.prebufferFrames = impl_->targetPrebufferFrames;
    }
    if (sequence <= peer.sequence) return;
    if (peer.decoder == nullptr) {
        int error = OPUS_OK;
        peer.decoder = opus_decoder_create(Impl::sampleRate, 1, &error);
        if (error != OPUS_OK || peer.decoder == nullptr) return;
    }
    if (peer.stream == nullptr) {
        const SDL_AudioSpec spec{SDL_AUDIO_S16, Impl::playbackChannels, Impl::sampleRate};
        peer.stream = impl_->openStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                         &spec, nullptr, nullptr);
        if (peer.stream == nullptr || !impl_->pauseStream(peer.stream)) {
            impl_->close_peer(peer); return;
        }
    }
    const auto gapMs = peer.lastArrival == std::chrono::steady_clock::time_point{} ? 0 :
        std::chrono::duration_cast<std::chrono::milliseconds>(now - peer.lastArrival).count();
    if (peer.lastArrival != std::chrono::steady_clock::time_point{} &&
        now - peer.lastArrival > std::chrono::milliseconds(150)) {
        impl_->clearStream(peer.stream);
        impl_->pauseStream(peer.stream);
        peer.playing = false;
        peer.prebufferFrames = std::min(peer.prebufferFrames + 1, 10);
    }
    peer.lastArrival = now;
    constexpr int frameBytes = Impl::frameSamples * Impl::playbackChannels * sizeof(int16_t);
    const int queuedBefore = impl_->queued(peer.stream);
    if (peer.playing && queuedBefore == 0 && gapMs >= 60) {
        impl_->clearStream(peer.stream);
        impl_->pauseStream(peer.stream);
        peer.playing = false;
        peer.prebufferFrames = std::min(peer.prebufferFrames + 1, 10);
    }
    if (queuedBefore > Impl::maxQueuedFrames * frameBytes) {
        impl_->clearStream(peer.stream);
        impl_->pauseStream(peer.stream);
        peer.playing = false;
    }
    const float gain = std::clamp(proximityGain, 0.0f, 1.0f) *
                       (impl_->playerVolume / 100.0f) * impl_->masterGain;
    impl_->streamGain(peer.stream, gain);
    const float clampedPan = std::isfinite(pan) ? std::clamp(pan, 0.0f, 1.0f) : 0.5f;
    const float angle = clampedPan * 1.57079632679f;
    const float leftGain = std::cos(angle);
    const float rightGain = std::sin(angle);
    const auto queue_audio = [&](const int16_t* mono, int samples) {
        std::array<int16_t, Impl::frameSamples * 2 * Impl::playbackChannels> stereo{};
        for (int i = 0; i < samples; ++i) {
            // Raise quiet microphones before Master Volume while smoothly
            // limiting peaks so the fixed boost does not hard-clip speech.
            const float boosted = std::tanh(
                (mono[i] / 32768.0f) * Impl::playbackBoost) * 32767.0f;
            stereo[i * 2] = static_cast<int16_t>(std::clamp(
                int(std::lround(boosted * leftGain)), -32768, 32767));
            stereo[i * 2 + 1] = static_cast<int16_t>(std::clamp(
                int(std::lround(boosted * rightGain)), -32768, 32767));
        }
        impl_->writeStream(peer.stream, stereo.data(),
                           samples * Impl::playbackChannels * int(sizeof(int16_t)));
    };
    if (peer.sequence != 0 && sequence > peer.sequence + 1) {
        const uint32_t missing = sequence - peer.sequence - 1;
        for (uint32_t i = 0; i < std::min(missing, 3U); ++i) {
            std::array<int16_t, Impl::frameSamples> concealed{};
            const int samples = opus_decode(peer.decoder, nullptr, 0,
                                            concealed.data(), concealed.size(), 0);
            if (samples > 0) queue_audio(concealed.data(), samples);
        }
    }
    std::array<int16_t, Impl::frameSamples * 2> pcm{};
    const int samples = opus_decode(peer.decoder, encoded.data(), encoded.size(),
                                    pcm.data(), pcm.size(), 0);
    if (samples <= 0) return;
    peer.sequence = sequence;
    queue_audio(pcm.data(), samples);
    if (!peer.playing && impl_->queued(peer.stream) >= peer.prebufferFrames * frameBytes) {
        peer.playing = impl_->resumeStream(peer.stream);
    }
#else
    (void)peerId; (void)sequence; (void)encoded; (void)proximityGain; (void)pan;
#endif
}

void VoiceChat::peer_left(std::string_view peerId) {
#if defined(DUSKLIGHT_ONLINE_VOICE)
    auto it = impl_->peers.find(std::string(peerId));
    if (it != impl_->peers.end()) { impl_->close_peer(it->second); impl_->peers.erase(it); }
#else
    (void)peerId;
#endif
}

void VoiceChat::stop() {
#if defined(DUSKLIGHT_ONLINE_VOICE)
    if (!impl_->enabled && impl_->captureStream == nullptr && impl_->peers.empty()) return;
    impl_->close_capture();
    for (auto& [id, peer] : impl_->peers) { (void)id; impl_->close_peer(peer); }
    impl_->peers.clear();
    impl_->enabled = false;
    impl_->openAttempted = false;
#endif
}

const std::string& VoiceChat::error() const { return impl_->lastError; }

}  // namespace dusklight_online::game

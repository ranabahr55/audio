// recorder.cpp — see recorder.hpp for the design rationale.

#include "recorder.hpp"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>

Recorder::Recorder(int sample_rate, int channels, int frame_samples)
    : sample_rate_(sample_rate),
      channels_(channels),
      frame_floats_(static_cast<std::size_t>(frame_samples) * channels) {
    // Hold ~0.5 s of audio so brief consumer hiccups don't drop samples,
    // while still bounding worst-case latency if the consumer stalls.
    std::size_t cap = frame_floats_ *
                      static_cast<std::size_t>((sample_rate / frame_samples) / 2 + 4);
    ring_.assign(cap, 0.0f);
}

Recorder::~Recorder() { stop(); }

bool Recorder::start() {
    PaError err = Pa_Initialize();
    if (err != paNoError) {
        std::fprintf(stderr, "PortAudio init failed: %s\n", Pa_GetErrorText(err));
        return false;
    }

    // List every input-capable device so the user can identify the mic. On a
    // Jetson the I2S/ADMAIF capture is never the default device, so this makes
    // it easy to pick the right `device` value for config.cfg.
    const int n_devices = Pa_GetDeviceCount();
    std::fprintf(stderr, "Input devices:\n");
    for (int i = 0; i < n_devices; ++i) {
        const PaDeviceInfo* di = Pa_GetDeviceInfo(i);
        if (di && di->maxInputChannels > 0) {
            std::fprintf(stderr, "  [%d] %s  (max in %d ch, default %.0f Hz)\n",
                         i, di->name, di->maxInputChannels, di->defaultSampleRate);
        }
    }

    // Resolve device_spec_: empty => default; all-digits => index; otherwise a
    // case-insensitive substring match on the device name (e.g. "APE", "hw:1,2").
    PaDeviceIndex dev = paNoDevice;
    if (device_spec_.empty()) {
        dev = Pa_GetDefaultInputDevice();
    } else {
        bool numeric = true;
        for (unsigned char c : device_spec_)
            if (!std::isdigit(c)) { numeric = false; break; }
        if (numeric) {
            dev = std::stoi(device_spec_);
        } else {
            std::string needle = device_spec_;
            for (char& c : needle) c = static_cast<char>(std::tolower((unsigned char)c));
            for (int i = 0; i < n_devices; ++i) {
                const PaDeviceInfo* di = Pa_GetDeviceInfo(i);
                if (!di || di->maxInputChannels <= 0) continue;
                std::string name = di->name;
                for (char& c : name) c = static_cast<char>(std::tolower((unsigned char)c));
                if (name.find(needle) != std::string::npos) { dev = i; break; }
            }
        }
    }

    if (dev == paNoDevice || dev < 0 || dev >= n_devices ||
        !Pa_GetDeviceInfo(dev) || Pa_GetDeviceInfo(dev)->maxInputChannels <= 0) {
        std::fprintf(stderr,
                     "Could not resolve input device '%s' (empty = default). "
                     "Pick an [index] or name from the list above and set "
                     "`device` in config.cfg.\n",
                     device_spec_.c_str());
        Pa_Terminate();
        return false;
    }
    std::fprintf(stderr, "Using input device [%d] %s\n",
                 dev, Pa_GetDeviceInfo(dev)->name);

    PaStreamParameters in{};
    in.device = dev;
    in.channelCount = channels_;
    in.sampleFormat = paFloat32;
    in.suggestedLatency = Pa_GetDeviceInfo(in.device)->defaultLowInputLatency;
    in.hostApiSpecificStreamInfo = nullptr;

    const unsigned long frames_per_buffer = frame_floats_ / channels_;

    err = Pa_OpenStream(&stream_, &in, /*output=*/nullptr, sample_rate_,
                        frames_per_buffer, paClipOff, &Recorder::PaCallback, this);
    if (err != paNoError) {
        std::fprintf(stderr, "Pa_OpenStream failed: %s\n", Pa_GetErrorText(err));
        Pa_Terminate();
        return false;
    }

    running_.store(true);
    err = Pa_StartStream(stream_);
    if (err != paNoError) {
        std::fprintf(stderr, "Pa_StartStream failed: %s\n", Pa_GetErrorText(err));
        running_.store(false);
        Pa_CloseStream(stream_);
        stream_ = nullptr;
        Pa_Terminate();
        return false;
    }

    const PaStreamInfo* info = Pa_GetStreamInfo(stream_);
    std::fprintf(stderr, "Capture started: %d Hz, %d ch, input latency %.1f ms\n",
                 sample_rate_, channels_,
                 info ? info->inputLatency * 1000.0 : -1.0);
    return true;
}

void Recorder::stop() {
    if (!running_.exchange(false) && !stream_) return;

    if (stream_) {
        Pa_StopStream(stream_);
        Pa_CloseStream(stream_);
        stream_ = nullptr;
        Pa_Terminate();
    }
    // Wake any consumer blocked in read_frame() so it can return false.
    cv_.notify_all();
}

int Recorder::PaCallback(const void* input, void* /*output*/,
                         unsigned long frame_count,
                         const PaStreamCallbackTimeInfo* /*time_info*/,
                         PaStreamCallbackFlags /*flags*/, void* user_data) {
    auto* self = static_cast<Recorder*>(user_data);
    if (input) {
        self->push(static_cast<const float*>(input),
                   static_cast<std::size_t>(frame_count) * self->channels_);
    }
    return self->running_.load() ? paContinue : paComplete;
}

void Recorder::push(const float* in, std::size_t n_floats) {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        const std::size_t cap = ring_.size();

        // If the producer is about to overflow the buffer, drop the oldest
        // whole frames to make room. Real-time audio favors the freshest
        // samples over a backlog.
        if (count_ + n_floats > cap) {
            std::size_t need = count_ + n_floats - cap;
            // Round up to whole frames so the ring stays frame-aligned.
            std::size_t drop = ((need + frame_floats_ - 1) / frame_floats_) * frame_floats_;
            if (drop > count_) drop = count_;
            head_ = (head_ + drop) % cap;
            count_ -= drop;
            overruns_.fetch_add(drop / frame_floats_);
        }

        // Boost the I2S MEMS mic (ICS-43434) as samples land in the ring. The
        // multiply is cheap and real-time-safe; load the gain once so it stays
        // constant across this frame.
        const float g = push_gain_.load();
        for (std::size_t i = 0; i < n_floats; ++i) {
            ring_[tail_] = in[i] * g;
            tail_ = (tail_ + 1) % cap;
        }
        count_ += n_floats;
    }
    cv_.notify_one();
}

bool Recorder::read_frame(std::vector<float>& out) {
    std::unique_lock<std::mutex> lock(mtx_);
    cv_.wait(lock, [this] { return count_ >= frame_floats_ || !running_.load(); });

    if (count_ < frame_floats_) return false;  // stopped and drained

    out.resize(frame_floats_);
    const std::size_t cap = ring_.size();
    for (std::size_t i = 0; i < frame_floats_; ++i) {
        out[i] = ring_[head_];
        head_ = (head_ + 1) % cap;
    }
    count_ -= frame_floats_;
    return true;
}

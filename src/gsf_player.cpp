#include "dragonbytez/gsf_player.hpp"

#include "dragonbytez/rom.hpp"

#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include "vbam/gba/GBA.h"
#include "vbam/gba/Sound.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace dragonbytez {

namespace {

class CaptureOutput final : public GBASoundOut {
public:
    explicit CaptureOutput(std::size_t maximum_samples)
        : maximum_samples_(maximum_samples) {
        samples_.reserve(maximum_samples);
    }

    void write(const void* samples, unsigned long bytes) override {
        const auto* source =
            static_cast<const std::int16_t*>(samples);
        const std::size_t available =
            bytes / sizeof(std::int16_t);
        const std::size_t remaining =
            maximum_samples_ > samples_.size()
                ? maximum_samples_ - samples_.size()
                : 0;
        const std::size_t accepted =
            std::min(available, remaining);
        samples_.insert(
            samples_.end(), source, source + accepted);
    }

    bool full() const noexcept {
        return samples_.size() >= maximum_samples_;
    }

    std::size_t frames() const noexcept {
        return samples_.size() / 2U;
    }

    const std::vector<std::int16_t>& samples() const noexcept {
        return samples_;
    }

private:
    std::size_t maximum_samples_;
    std::vector<std::int16_t> samples_;
};

void write_u16(std::ofstream& output, std::uint16_t value) {
    const char bytes[] = {
        static_cast<char>(value),
        static_cast<char>(value >> 8U)};
    output.write(bytes, 2);
}

void write_u32(std::ofstream& output, std::uint32_t value) {
    const char bytes[] = {
        static_cast<char>(value),
        static_cast<char>(value >> 8U),
        static_cast<char>(value >> 16U),
        static_cast<char>(value >> 24U)};
    output.write(bytes, 4);
}

void write_stereo_wav(
    const std::filesystem::path& path,
    const std::vector<std::int16_t>& samples,
    std::uint32_t sample_rate) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    if (!output) {
        throw std::runtime_error(
            "cannot create rendered music preview");
    }

    const std::uint32_t data_size =
        static_cast<std::uint32_t>(
            samples.size() * sizeof(std::int16_t));
    output.write("RIFF", 4);
    write_u32(output, 36U + data_size);
    output.write("WAVEfmt ", 8);
    write_u32(output, 16);
    write_u16(output, 1);
    write_u16(output, 2);
    write_u32(output, sample_rate);
    write_u32(output, sample_rate * 4U);
    write_u16(output, 4);
    write_u16(output, 16);
    output.write("data", 4);
    write_u32(output, data_size);
    output.write(
        reinterpret_cast<const char*>(samples.data()),
        static_cast<std::streamsize>(data_size));
    if (!output) {
        throw std::runtime_error(
            "cannot finish rendered music preview");
    }
}

double window_rms(
    const std::vector<std::int16_t>& samples,
    std::size_t first_frame,
    std::size_t frame_count) {
    constexpr std::size_t channel_count = 2;
    const std::size_t first_sample = first_frame * channel_count;
    const std::size_t available_frames =
        samples.size() / channel_count - first_frame;
    const std::size_t accepted_frames =
        std::min(frame_count, available_frames);
    if (accepted_frames == 0) return 0.0;

    long double square_sum = 0.0;
    const std::size_t sample_count =
        accepted_frames * channel_count;
    for (std::size_t index = 0; index < sample_count; ++index) {
        const long double value =
            samples[first_sample + index];
        square_sum += value * value;
    }
    return std::sqrt(
        static_cast<double>(
            square_sum / static_cast<long double>(sample_count)));
}

bool has_recent_sustained_music(
    const std::vector<std::int16_t>& samples,
    std::uint32_t sample_rate) {
    constexpr std::size_t channel_count = 2;
    constexpr double music_rms_threshold = 180.0;
    constexpr std::size_t required_active_windows = 4;
    const std::size_t total_frames =
        samples.size() / channel_count;
    const std::size_t window_frames =
        std::max<std::size_t>(1, sample_rate / 20U);
    const std::size_t required_frames =
        required_active_windows * window_frames;
    if (total_frames < sample_rate + required_frames) return false;

    const std::size_t first_frame =
        total_frames - required_frames;
    for (std::size_t window = 0;
         window < required_active_windows;
         ++window) {
        if (window_rms(
                samples,
                first_frame + window * window_frames,
                window_frames) < music_rms_threshold) {
            return false;
        }
    }
    return true;
}

std::size_t find_music_start_frame(
    const std::vector<std::int16_t>& samples,
    std::uint32_t sample_rate) {
    constexpr std::size_t channel_count = 2;
    constexpr double music_rms_threshold = 180.0;
    constexpr std::size_t required_active_windows = 4;
    const std::size_t total_frames =
        samples.size() / channel_count;
    const std::size_t guard_frames =
        static_cast<std::size_t>(sample_rate);
    const std::size_t window_frames =
        std::max<std::size_t>(1, sample_rate / 20U);

    std::size_t active_windows = 0;
    for (std::size_t frame = guard_frames;
         frame + window_frames <= total_frames;
         frame += window_frames) {
        if (window_rms(samples, frame, window_frames) >=
            music_rms_threshold) {
            ++active_windows;
            if (active_windows >= required_active_windows) {
                const std::size_t active_start =
                    frame -
                    (required_active_windows - 1U) * window_frames;
                const std::size_t lead_frames =
                    sample_rate / 100U;
                return active_start > lead_frames
                    ? active_start - lead_frames
                    : 0;
            }
        } else {
            active_windows = 0;
        }
    }
    throw std::runtime_error(
        "the GBA engine did not produce a sustained music signal");
}

std::vector<std::int16_t> trim_and_fade_music(
    const std::vector<std::int16_t>& captured_samples,
    std::uint32_t sample_rate,
    unsigned seconds) {
    constexpr std::size_t channel_count = 2;
    const std::size_t start_frame =
        find_music_start_frame(captured_samples, sample_rate);
    const std::size_t requested_frames =
        static_cast<std::size_t>(sample_rate) * seconds;
    const std::size_t available_frames =
        captured_samples.size() / channel_count - start_frame;
    if (available_frames < requested_frames) {
        throw std::runtime_error(
            "the GBA engine did not capture enough music after startup");
    }

    const std::size_t first_sample =
        start_frame * channel_count;
    const std::size_t requested_samples =
        requested_frames * channel_count;
    std::vector<std::int16_t> output(
        captured_samples.begin() +
            static_cast<std::ptrdiff_t>(first_sample),
        captured_samples.begin() +
            static_cast<std::ptrdiff_t>(
                first_sample + requested_samples));

    const std::size_t fade_in_frames =
        std::min<std::size_t>(sample_rate / 50U, requested_frames);
    const std::size_t fade_out_frames =
        std::min<std::size_t>(sample_rate / 10U, requested_frames);
    for (std::size_t frame = 0; frame < fade_in_frames; ++frame) {
        const double gain =
            static_cast<double>(frame) /
            static_cast<double>(fade_in_frames);
        for (std::size_t channel = 0;
             channel < channel_count;
             ++channel) {
            const std::size_t sample =
                frame * channel_count + channel;
            output[sample] = static_cast<std::int16_t>(
                std::lround(output[sample] * gain));
        }
    }
    for (std::size_t frame = 0; frame < fade_out_frames; ++frame) {
        const double gain =
            static_cast<double>(fade_out_frames - frame - 1U) /
            static_cast<double>(fade_out_frames);
        const std::size_t output_frame =
            requested_frames - fade_out_frames + frame;
        for (std::size_t channel = 0;
             channel < channel_count;
             ++channel) {
            const std::size_t sample =
                output_frame * channel_count + channel;
            output[sample] = static_cast<std::int16_t>(
                std::lround(output[sample] * gain));
        }
    }

    const std::size_t validation_frames =
        std::min<std::size_t>(
            requested_frames,
            static_cast<std::size_t>(sample_rate) * 3U);
    if (window_rms(output, 0, validation_frames) < 180.0) {
        throw std::runtime_error(
            "the rendered WAV did not contain an audible music signal");
    }
    return output;
}

void set_a_button(GBASystem& system, bool pressed) {
    constexpr std::uint16_t a_button = 0x0001;
    if (pressed) {
        system.P1 =
            static_cast<std::uint16_t>(system.P1 & ~a_button);
    } else {
        system.P1 =
            static_cast<std::uint16_t>(system.P1 | a_button);
    }
    system.ioMem[0x130] =
        static_cast<std::uint8_t>(system.P1);
    system.ioMem[0x131] =
        static_cast<std::uint8_t>(system.P1 >> 8U);
}

}

void render_bgm_preview_wav(
    const Rom& rom,
    std::size_t track,
    const std::filesystem::path& output_path,
    unsigned seconds) {
    const auto& profile = profile_for(rom);
    if (track >= profile.bgm_count) {
        throw std::out_of_range("BGM track index is out of range");
    }
    if (seconds == 0 || seconds > 300) {
        throw std::out_of_range(
            "BGM preview duration must be between 1 and 300 seconds");
    }

    std::vector<std::uint8_t> patched_rom = rom.bytes();
    const std::size_t source =
        profile.bgm_table + track * 20;
    std::copy_n(
        patched_rom.begin() + static_cast<std::ptrdiff_t>(source),
        20,
        patched_rom.begin() +
            static_cast<std::ptrdiff_t>(profile.title_bgm_record));

    constexpr std::uint32_t sample_rate = 44100;
    constexpr unsigned startup_allowance_seconds = 12;
    const std::size_t maximum_samples =
        static_cast<std::size_t>(sample_rate) * 2U *
        (seconds + startup_allowance_seconds);
    CaptureOutput capture(maximum_samples);
    GBASystem system;
    system.cpuIsMultiBoot = false;
    system.soundSampleRate = sample_rate;
    system.soundDeclicking = false;
    system.soundInterpolation = false;

    if (CPULoadRom(
            &system, patched_rom.data(),
            static_cast<std::uint32_t>(patched_rom.size())) == 0) {
        throw std::runtime_error(
            "the embedded GBA audio engine could not load the ROM");
    }

    try {
        if (!soundInit(&system, &capture)) {
            throw std::runtime_error(
                "the embedded GBA audio engine could not initialize");
        }
        soundReset(&system);
        CPUInit(&system);
        CPUReset(&system);
        soundResume(&system);
        bool music_started = false;
        std::size_t next_signal_check = sample_rate;
        while (!capture.full()) {
            if (std::string_view(profile.game_code) == "ALFP") {
                const std::size_t elapsed_frames = capture.frames();
                const std::size_t one_second = sample_rate;
                const bool select_default_language =
                    !music_started &&
                    elapsed_frames >= one_second &&
                    elapsed_frames < one_second * 10U &&
                    elapsed_frames % one_second < one_second / 10U;
                set_a_button(system, select_default_language);
            }
            CPULoop(&system, 250000);
            if (!music_started &&
                capture.frames() >= next_signal_check) {
                music_started = has_recent_sustained_music(
                    capture.samples(), sample_rate);
                next_signal_check =
                    capture.frames() + sample_rate / 20U;
            }
        }
        const auto music = trim_and_fade_music(
            capture.samples(), sample_rate, seconds);
        write_stereo_wav(output_path, music, sample_rate);
    } catch (...) {
        CPUCleanUp(&system);
        throw;
    }
    CPUCleanUp(&system);
}

} // namespace dragonbytez

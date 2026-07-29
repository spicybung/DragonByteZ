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
#include <limits>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
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


struct DetectedLoop {
    std::size_t start_frame = 0;
    std::size_t length_frames = 0;
};

std::uint64_t audio_block_fingerprint(
    const std::vector<std::int16_t>& samples,
    std::size_t first_frame,
    std::size_t frame_count) {
    constexpr std::size_t channel_count = 2;
    constexpr std::uint64_t fnv_offset = 1469598103934665603ULL;
    constexpr std::uint64_t fnv_prime = 1099511628211ULL;
    std::uint64_t hash = fnv_offset;
    const std::size_t total_frames = samples.size() / channel_count;
    const std::size_t end_frame = std::min(total_frames, first_frame + frame_count);
    for (std::size_t frame = first_frame; frame < end_frame; frame += 3U) {
        const std::int32_t left = samples[frame * channel_count];
        const std::int32_t right = samples[frame * channel_count + 1U];
        const std::int16_t mono = static_cast<std::int16_t>((left + right) / 2);
        const std::uint16_t quantized = static_cast<std::uint16_t>(
            static_cast<std::int32_t>(mono) + 32768) >> 5U;
        hash ^= static_cast<std::uint8_t>(quantized);
        hash *= fnv_prime;
        hash ^= static_cast<std::uint8_t>(quantized >> 8U);
        hash *= fnv_prime;
    }
    return hash;
}

std::vector<std::uint64_t> build_audio_block_fingerprints(
    const std::vector<std::int16_t>& samples,
    std::size_t music_start_frame,
    std::uint32_t sample_rate,
    std::size_t& block_frames) {
    block_frames = std::max<std::size_t>(1U, sample_rate / 60U);
    const std::size_t total_frames = samples.size() / 2U;
    if (total_frames <= music_start_frame + block_frames) return {};
    const std::size_t block_count =
        (total_frames - music_start_frame) / block_frames;
    std::vector<std::uint64_t> fingerprints;
    fingerprints.reserve(block_count);
    for (std::size_t block = 0; block < block_count; ++block) {
        fingerprints.push_back(audio_block_fingerprint(
            samples,
            music_start_frame + block * block_frames,
            block_frames));
    }
    return fingerprints;
}

std::uint64_t sequence_hash(
    const std::vector<std::uint64_t>& prefix,
    const std::vector<std::uint64_t>& powers,
    std::size_t start,
    std::size_t count) {
    return prefix[start + count] - prefix[start] * powers[count];
}

bool fingerprint_sequences_equal(
    const std::vector<std::uint64_t>& fingerprints,
    std::size_t first,
    std::size_t second,
    std::size_t count) {
    if (first + count > fingerprints.size() ||
        second + count > fingerprints.size()) {
        return false;
    }
    return std::equal(
        fingerprints.begin() + static_cast<std::ptrdiff_t>(first),
        fingerprints.begin() + static_cast<std::ptrdiff_t>(first + count),
        fingerprints.begin() + static_cast<std::ptrdiff_t>(second));
}

DetectedLoop detect_audio_loop(
    const std::vector<std::int16_t>& samples,
    std::size_t music_start_frame,
    std::uint32_t sample_rate) {
    std::size_t block_frames = 0;
    const auto fingerprints = build_audio_block_fingerprints(
        samples, music_start_frame, sample_rate, block_frames);
    if (fingerprints.empty()) return {};

    constexpr std::uint64_t base = 11400714819323198485ULL;
    const std::size_t verification_blocks = 180U;
    const std::size_t minimum_loop_blocks = 8U * 60U;
    const std::size_t maximum_loop_blocks = 300U * 60U;
    const std::size_t intro_guard_blocks = 3U * 60U;
    if (fingerprints.size() <
        intro_guard_blocks + minimum_loop_blocks + verification_blocks) {
        return {};
    }

    std::vector<std::uint64_t> prefix(fingerprints.size() + 1U, 0U);
    std::vector<std::uint64_t> powers(fingerprints.size() + 1U, 1U);
    for (std::size_t index = 0; index < fingerprints.size(); ++index) {
        prefix[index + 1U] = prefix[index] * base + fingerprints[index];
        powers[index + 1U] = powers[index] * base;
    }

    std::unordered_map<std::uint64_t, std::vector<std::size_t>> earlier;
    earlier.reserve(fingerprints.size());
    const std::size_t last_start = fingerprints.size() - verification_blocks;
    for (std::size_t start = intro_guard_blocks; start <= last_start; ++start) {
        const std::uint64_t hash = sequence_hash(
            prefix, powers, start, verification_blocks);
        auto& candidates = earlier[hash];
        for (const std::size_t previous : candidates) {
            const std::size_t distance = start - previous;
            if (distance < minimum_loop_blocks ||
                distance > maximum_loop_blocks) {
                continue;
            }
            if (!fingerprint_sequences_equal(
                    fingerprints,
                    previous,
                    start,
                    verification_blocks)) {
                continue;
            }
            const std::size_t validation_frames =
                std::min<std::size_t>(sample_rate * 2U, distance * block_frames);
            if (window_rms(
                    samples,
                    music_start_frame + previous * block_frames,
                    validation_frames) < 180.0) {
                continue;
            }
            return {
                music_start_frame + previous * block_frames,
                distance * block_frames};
        }
        if (candidates.size() < 8U) candidates.push_back(start);
    }

    struct Feature {
        double rms = 0.0;
        double mean = 0.0;
        double difference = 0.0;
    };

    const std::size_t total_frames = samples.size() / 2U;
    const std::size_t feature_count =
        total_frames > music_start_frame
            ? (total_frames - music_start_frame) / block_frames
            : 0U;
    constexpr std::size_t tolerant_verification_blocks = 30U * 60U;
    if (feature_count <
        intro_guard_blocks + minimum_loop_blocks +
            tolerant_verification_blocks) {
        return {};
    }

    std::vector<Feature> features;
    features.reserve(feature_count);
    for (std::size_t block = 0; block < feature_count; ++block) {
        const std::size_t first_frame =
            music_start_frame + block * block_frames;
        long double square_sum = 0.0;
        long double mean_sum = 0.0;
        long double difference_sum = 0.0;
        std::int32_t previous_mono = 0;
        for (std::size_t frame = 0; frame < block_frames; ++frame) {
            const std::size_t sample = (first_frame + frame) * 2U;
            const std::int32_t mono =
                (static_cast<std::int32_t>(samples[sample]) +
                 static_cast<std::int32_t>(samples[sample + 1U])) / 2;
            square_sum += static_cast<long double>(mono) * mono;
            mean_sum += mono;
            if (frame != 0U) {
                difference_sum += std::abs(mono - previous_mono);
            }
            previous_mono = mono;
        }
        Feature feature;
        feature.rms = std::sqrt(static_cast<double>(
            square_sum / static_cast<long double>(block_frames)));
        feature.mean = static_cast<double>(
            mean_sum / static_cast<long double>(block_frames));
        feature.difference = block_frames > 1U
            ? static_cast<double>(
                difference_sum /
                static_cast<long double>(block_frames - 1U))
            : 0.0;
        features.push_back(feature);
    }

    Feature average;
    for (const Feature& feature : features) {
        average.rms += feature.rms;
        average.mean += feature.mean;
        average.difference += feature.difference;
    }
    const double feature_divisor = static_cast<double>(features.size());
    average.rms /= feature_divisor;
    average.mean /= feature_divisor;
    average.difference /= feature_divisor;

    Feature deviation;
    for (const Feature& feature : features) {
        deviation.rms +=
            (feature.rms - average.rms) * (feature.rms - average.rms);
        deviation.mean +=
            (feature.mean - average.mean) * (feature.mean - average.mean);
        deviation.difference +=
            (feature.difference - average.difference) *
            (feature.difference - average.difference);
    }
    deviation.rms = std::sqrt(deviation.rms / feature_divisor);
    deviation.mean = std::sqrt(deviation.mean / feature_divisor);
    deviation.difference = std::sqrt(
        deviation.difference / feature_divisor);
    deviation.rms = std::max(1.0, deviation.rms);
    deviation.mean = std::max(1.0, deviation.mean);
    deviation.difference = std::max(1.0, deviation.difference);

    std::vector<Feature> normalized(features.size());
    for (std::size_t index = 0; index < features.size(); ++index) {
        normalized[index].rms =
            (features[index].rms - average.rms) / deviation.rms;
        normalized[index].mean =
            (features[index].mean - average.mean) / deviation.mean;
        normalized[index].difference =
            (features[index].difference - average.difference) /
            deviation.difference;
    }

    const std::size_t second_start =
        normalized.size() - tolerant_verification_blocks;
    const std::size_t maximum_lag = std::min<std::size_t>(
        maximum_loop_blocks,
        second_start > intro_guard_blocks
            ? second_start - intro_guard_blocks
            : 0U);
    double best_score = std::numeric_limits<double>::infinity();
    std::size_t best_lag = 0U;
    for (std::size_t lag = minimum_loop_blocks;
         lag <= maximum_lag;
         ++lag) {
        const std::size_t first_start = second_start - lag;
        long double total_difference = 0.0;
        for (std::size_t offset = 0;
             offset < tolerant_verification_blocks;
             ++offset) {
            const Feature& first = normalized[first_start + offset];
            const Feature& second = normalized[second_start + offset];
            total_difference += std::abs(first.rms - second.rms);
            total_difference += std::abs(first.mean - second.mean);
            total_difference +=
                std::abs(first.difference - second.difference);
        }
        const double score = static_cast<double>(
            total_difference /
            static_cast<long double>(
                tolerant_verification_blocks * 3U));
        if (score < best_score) {
            best_score = score;
            best_lag = lag;
        }
    }

    constexpr double tolerant_score_limit = 0.18;
    if (best_lag == 0U || best_score > tolerant_score_limit) {
        return {};
    }

    const std::size_t start_search_end =
        second_start >= best_lag ? second_start - best_lag : 0U;
    const std::size_t start_window = 8U * 60U;
    std::size_t loop_start_block = intro_guard_blocks;
    double earliest_score = std::numeric_limits<double>::infinity();
    for (std::size_t start = intro_guard_blocks;
         start + best_lag + start_window <= normalized.size() &&
         start <= start_search_end;
         ++start) {
        long double total_difference = 0.0;
        for (std::size_t offset = 0; offset < start_window; ++offset) {
            const Feature& first = normalized[start + offset];
            const Feature& second = normalized[start + best_lag + offset];
            total_difference += std::abs(first.rms - second.rms);
            total_difference += std::abs(first.mean - second.mean);
            total_difference +=
                std::abs(first.difference - second.difference);
        }
        const double score = static_cast<double>(
            total_difference /
            static_cast<long double>(start_window * 3U));
        if (score < earliest_score) {
            earliest_score = score;
            loop_start_block = start;
        }
        if (score <= std::max(0.20, best_score * 1.8)) {
            loop_start_block = start;
            break;
        }
    }

    return {
        music_start_frame + loop_start_block * block_frames,
        best_lag * block_frames};
}

bool recent_silence(
    const std::vector<std::int16_t>& samples,
    std::uint32_t sample_rate,
    unsigned seconds) {
    const std::size_t total_frames = samples.size() / 2U;
    const std::size_t requested_frames =
        static_cast<std::size_t>(sample_rate) * seconds;
    if (total_frames < requested_frames) return false;
    return window_rms(
        samples,
        total_frames - requested_frames,
        requested_frames) < 45.0;
}

void apply_fades(
    std::vector<std::int16_t>& samples,
    std::uint32_t sample_rate,
    unsigned fade_out_seconds) {
    constexpr std::size_t channel_count = 2;
    const std::size_t total_frames = samples.size() / channel_count;
    if (total_frames == 0) return;

    const std::size_t fade_in_frames =
        std::min<std::size_t>(sample_rate / 50U, total_frames);
    for (std::size_t frame = 0; frame < fade_in_frames; ++frame) {
        const double gain = fade_in_frames > 1U
            ? static_cast<double>(frame) /
                static_cast<double>(fade_in_frames - 1U)
            : 1.0;
        for (std::size_t channel = 0; channel < channel_count; ++channel) {
            const std::size_t sample = frame * channel_count + channel;
            samples[sample] = static_cast<std::int16_t>(
                std::lround(static_cast<double>(samples[sample]) * gain));
        }
    }

    const std::size_t fade_out_frames = std::min<std::size_t>(
        static_cast<std::size_t>(sample_rate) * fade_out_seconds,
        total_frames);
    if (fade_out_frames == 0) return;
    const std::size_t fade_start = total_frames - fade_out_frames;
    for (std::size_t frame = 0; frame < fade_out_frames; ++frame) {
        const double gain = fade_out_frames > 1U
            ? static_cast<double>(fade_out_frames - frame - 1U) /
                static_cast<double>(fade_out_frames - 1U)
            : 0.0;
        const std::size_t output_frame = fade_start + frame;
        for (std::size_t channel = 0; channel < channel_count; ++channel) {
            const std::size_t sample = output_frame * channel_count + channel;
            samples[sample] = static_cast<std::int16_t>(
                std::lround(static_cast<double>(samples[sample]) * gain));
        }
    }
}

std::vector<std::int16_t> select_full_music(
    const std::vector<std::int16_t>& captured_samples,
    std::uint32_t sample_rate,
    std::size_t music_start_frame,
    const DetectedLoop& loop,
    unsigned loop_count,
    bool natural_end,
    unsigned fade_seconds,
    unsigned maximum_seconds,
    BgmRenderResult& result) {
    constexpr std::size_t channel_count = 2;
    const std::size_t total_frames = captured_samples.size() / channel_count;
    std::size_t end_frame = std::min<std::size_t>(
        total_frames,
        music_start_frame +
            static_cast<std::size_t>(sample_rate) * maximum_seconds);

    if (loop.length_frames != 0U) {
        const std::size_t loop_end = loop.start_frame +
            loop.length_frames * std::max<unsigned>(1U, loop_count);
        end_frame = std::min(total_frames, loop_end);
        result.loop_detected = true;
        result.loop_start_seconds =
            static_cast<double>(loop.start_frame - music_start_frame) /
            static_cast<double>(sample_rate);
        result.loop_length_seconds =
            static_cast<double>(loop.length_frames) /
            static_cast<double>(sample_rate);
    } else if (natural_end) {
        const std::size_t trailing_silence =
            static_cast<std::size_t>(sample_rate) * 4U;
        if (end_frame > music_start_frame + trailing_silence) {
            end_frame -= trailing_silence;
        }
        result.natural_end_detected = true;
    }

    if (end_frame <= music_start_frame) {
        throw std::runtime_error("the GBA engine produced no complete track audio");
    }
    const std::size_t first_sample = music_start_frame * channel_count;
    const std::size_t end_sample = end_frame * channel_count;
    std::vector<std::int16_t> output(
        captured_samples.begin() + static_cast<std::ptrdiff_t>(first_sample),
        captured_samples.begin() + static_cast<std::ptrdiff_t>(end_sample));
    apply_fades(output, sample_rate, fade_seconds);
    result.duration_seconds = static_cast<unsigned>(
        output.size() / channel_count / sample_rate);
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
    const GameProfile profile = music_profile_for(rom);
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
    system.soundDeclicking = true;
    system.soundInterpolation = true;

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
        std::size_t previous_captured_frames = capture.frames();
        unsigned stalled_iterations = 0;
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
            const std::size_t captured_frames = capture.frames();
            if (captured_frames == previous_captured_frames) {
                ++stalled_iterations;
                if (stalled_iterations >= 600U) {
                    throw std::runtime_error(
                        "the GBA audio engine stopped producing samples");
                }
            } else {
                previous_captured_frames = captured_frames;
                stalled_iterations = 0;
            }
            if (!music_started &&
                captured_frames >= next_signal_check) {
                music_started = has_recent_sustained_music(
                    capture.samples(), sample_rate);
                next_signal_check =
                    captured_frames + sample_rate / 20U;
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


BgmRenderResult render_bgm_full_wav(
    const Rom& rom,
    std::size_t track,
    const std::filesystem::path& output_path,
    unsigned maximum_seconds,
    unsigned loop_count,
    unsigned fade_seconds) {
    const GameProfile profile = music_profile_for(rom);
    if (track >= profile.bgm_count) {
        throw std::out_of_range("BGM track index is out of range");
    }
    if (maximum_seconds < 30U || maximum_seconds > 900U) {
        throw std::out_of_range(
            "full BGM maximum duration must be between 30 and 900 seconds");
    }
    if (loop_count == 0U || loop_count > 4U) {
        throw std::out_of_range("BGM loop count must be between 1 and 4");
    }
    if (fade_seconds > 30U) {
        throw std::out_of_range("BGM fade duration must not exceed 30 seconds");
    }

    std::vector<std::uint8_t> patched_rom = rom.bytes();
    const std::size_t source = profile.bgm_table + track * 20U;
    std::copy_n(
        patched_rom.begin() + static_cast<std::ptrdiff_t>(source),
        20,
        patched_rom.begin() +
            static_cast<std::ptrdiff_t>(profile.title_bgm_record));

    constexpr std::uint32_t sample_rate = 44100;
    constexpr unsigned startup_allowance_seconds = 15;
    const std::size_t maximum_samples =
        static_cast<std::size_t>(sample_rate) * 2U *
        (maximum_seconds + startup_allowance_seconds);
    CaptureOutput capture(maximum_samples);
    GBASystem system;
    system.cpuIsMultiBoot = false;
    system.soundSampleRate = sample_rate;
    system.soundDeclicking = true;
    system.soundInterpolation = true;

    if (CPULoadRom(
            &system,
            patched_rom.data(),
            static_cast<std::uint32_t>(patched_rom.size())) == 0) {
        throw std::runtime_error(
            "the embedded GBA audio engine could not load the ROM");
    }

    BgmRenderResult result;
    DetectedLoop detected_loop;
    bool natural_end = false;
    std::size_t music_start_frame = 0;
    bool have_music_start = false;
    std::size_t target_end_frame = 0;
    std::size_t next_signal_check = sample_rate;
    std::size_t next_loop_check = sample_rate * 45U;

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
        bool finished = false;
        std::size_t previous_captured_frames = capture.frames();
        unsigned stalled_iterations = 0;
        while (!capture.full() && !finished) {
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
            const std::size_t captured_frames = capture.frames();
            if (captured_frames == previous_captured_frames) {
                ++stalled_iterations;
                if (stalled_iterations >= 600U) {
                    throw std::runtime_error(
                        "the GBA audio engine stopped producing samples");
                }
            } else {
                previous_captured_frames = captured_frames;
                stalled_iterations = 0;
            }
            if (!music_started && captured_frames >= next_signal_check) {
                music_started = has_recent_sustained_music(
                    capture.samples(), sample_rate);
                next_signal_check = captured_frames + sample_rate / 20U;
                if (music_started) {
                    next_signal_check = captured_frames + sample_rate;
                }
            }
            if (music_started && !have_music_start &&
                captured_frames >= next_signal_check) {
                music_start_frame = find_music_start_frame(
                    capture.samples(), sample_rate);
                have_music_start = true;
                next_loop_check = std::max(
                    captured_frames + sample_rate * 30U,
                    music_start_frame + sample_rate * 45U);
            }

            if (!have_music_start) continue;

            if (target_end_frame != 0U && captured_frames >= target_end_frame) {
                finished = true;
                continue;
            }

            const std::size_t music_frames = captured_frames - music_start_frame;
            if (music_frames >= sample_rate * 15U &&
                recent_silence(capture.samples(), sample_rate, 4U)) {
                natural_end = true;
                finished = true;
                continue;
            }

            if (captured_frames >= next_loop_check) {
                detected_loop = detect_audio_loop(
                    capture.samples(), music_start_frame, sample_rate);
                next_loop_check = captured_frames + sample_rate * 45U;
                if (detected_loop.length_frames != 0U) {
                    target_end_frame = detected_loop.start_frame +
                        detected_loop.length_frames * loop_count;
                    if (captured_frames >= target_end_frame) {
                        finished = true;
                    }
                }
            }
        }

        if (!have_music_start) {
            music_start_frame = find_music_start_frame(
                capture.samples(), sample_rate);
            have_music_start = true;
        }
        if (detected_loop.length_frames == 0U && !natural_end) {
            detected_loop = detect_audio_loop(
                capture.samples(), music_start_frame, sample_rate);
        }

        auto music = select_full_music(
            capture.samples(),
            sample_rate,
            music_start_frame,
            detected_loop,
            loop_count,
            natural_end,
            fade_seconds,
            maximum_seconds,
            result);
        if (window_rms(
                music,
                0,
                std::min<std::size_t>(music.size() / 2U, sample_rate * 3U)) <
            180.0) {
            throw std::runtime_error(
                "the rendered full WAV did not contain an audible music signal");
        }
        write_stereo_wav(output_path, music, sample_rate);
    } catch (...) {
        CPUCleanUp(&system);
        throw;
    }
    CPUCleanUp(&system);
    return result;
}

BgmRenderResult write_full_stereo_wav_from_capture(
    const std::vector<std::int16_t>& captured_samples,
    std::uint32_t sample_rate,
    const std::filesystem::path& output_path,
    unsigned maximum_seconds,
    unsigned loop_count,
    unsigned fade_seconds) {
    if (sample_rate < 8000U || sample_rate > 192000U) {
        throw std::out_of_range("captured audio sample rate is unsupported");
    }
    if (maximum_seconds < 30U || maximum_seconds > 900U) {
        throw std::out_of_range(
            "full track maximum duration must be between 30 and 900 seconds");
    }
    if (loop_count == 0U || loop_count > 4U) {
        throw std::out_of_range("track loop count must be between 1 and 4");
    }
    if (fade_seconds > 30U) {
        throw std::out_of_range("track fade duration must not exceed 30 seconds");
    }
    if (captured_samples.size() < static_cast<std::size_t>(sample_rate) * 2U) {
        throw std::runtime_error("captured audio is too short");
    }

    const std::size_t music_start_frame = find_music_start_frame(
        captured_samples, sample_rate);
    const DetectedLoop loop = detect_audio_loop(
        captured_samples, music_start_frame, sample_rate);
    const bool natural_end = loop.length_frames == 0U &&
        recent_silence(captured_samples, sample_rate, 4U);
    BgmRenderResult result;
    auto music = select_full_music(
        captured_samples,
        sample_rate,
        music_start_frame,
        loop,
        loop_count,
        natural_end,
        fade_seconds,
        maximum_seconds,
        result);
    write_stereo_wav(output_path, music, sample_rate);
    return result;
}

} // namespace dragonbytez

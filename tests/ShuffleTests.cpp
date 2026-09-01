#include "nnaga/native_plugin.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {
constexpr uint32_t kRate = 48000;
constexpr uint32_t kBarFrames = 96000;
constexpr uint32_t kBlockFrames = 512;
constexpr uint32_t kEnabledPort = 4;
constexpr uint32_t kAmountPort = 5;
constexpr uint32_t kSeedPort = 6;
constexpr uint32_t kMixPort = 7;
constexpr uint32_t kBarsPort = 8;
constexpr uint32_t kStartPort = 9;
constexpr uint32_t kEndPort = 10;
constexpr uint32_t kGridFromPort = 11;
constexpr uint32_t kGridToPort = 12;
constexpr uint32_t kGridModePort = 13;
constexpr uint32_t kGridSeedPort = 14;

void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", message);
        std::exit(1);
    }
}

struct RunResult {
    std::vector<float> in_l;
    std::vector<float> in_r;
    std::vector<float> out_l;
    std::vector<float> out_r;
};

struct Settings {
    uint32_t bars = 1;
    float enabled = 1.0f;
    float amount = 1.0f;
    float seed = 0.25f;
    float mix = 1.0f;
    float start = 0.0f;
    float end = 1.0f;
    float grid_from = 0.2f;
    float grid_to = 0.6f;
    float grid_mode = 0.0f;
    float grid_seed = 0.0f;
    bool playing = true;
};

const NnagaPluginDescriptorV1* shuffle_descriptor() {
    const auto* library = nnaga_plugin_entry(NNAGA_NATIVE_ABI_VERSION);
    require(library && library->plugin_count == 1 && library->get_plugin,
            "shuffle library descriptor");
    const auto* descriptor = library->get_plugin(0);
    require(descriptor && descriptor->create && descriptor->destroy && descriptor->activate &&
                descriptor->set_parameter && descriptor->format_parameter && descriptor->process,
            "shuffle plugin callbacks");
    return descriptor;
}

RunResult run_shuffle(const Settings& settings, uint32_t cycles = 2) {
    const auto* descriptor = shuffle_descriptor();
    const uint32_t cycle_frames = settings.bars * kBarFrames;
    const uint32_t total_frames = cycle_frames * cycles;
    require(cycle_frames != 0 && total_frames % kBlockFrames == 0,
            "host run consists of contiguous 512-frame blocks");

    NnagaPluginHandle handle = descriptor->create();
    require(handle && descriptor->activate(handle, kRate, kBlockFrames), "shuffle activate");
    descriptor->set_parameter(handle, kEnabledPort, settings.enabled);
    descriptor->set_parameter(handle, kAmountPort, settings.amount);
    descriptor->set_parameter(handle, kSeedPort, settings.seed);
    descriptor->set_parameter(handle, kMixPort, settings.mix);
    descriptor->set_parameter(handle, kBarsPort, static_cast<float>(settings.bars - 1) / 7.0f);
    descriptor->set_parameter(handle, kStartPort, settings.start);
    descriptor->set_parameter(handle, kEndPort, settings.end);
    descriptor->set_parameter(handle, kGridFromPort, settings.grid_from);
    descriptor->set_parameter(handle, kGridToPort, settings.grid_to);
    descriptor->set_parameter(handle, kGridModePort, settings.grid_mode);
    descriptor->set_parameter(handle, kGridSeedPort, settings.grid_seed);

    RunResult result;
    result.in_l.resize(total_frames);
    result.in_r.resize(total_frames);
    result.out_l.resize(total_frames);
    result.out_r.resize(total_frames);
    for (uint32_t frame = 0; frame < total_frames; ++frame) {
        const uint32_t cycle = frame / cycle_frames;
        const uint32_t phase = frame % cycle_frames;
        const float value = (cycle == 0 ? 1000.0f : 2000000.0f + 100000.0f * cycle) + phase;
        result.in_l[frame] = value;
        result.in_r[frame] = -value;
    }

    for (uint32_t offset = 0; offset < total_frames; offset += kBlockFrames) {
        NnagaProcessContextV1 context{};
        context.struct_size = sizeof(context);
        context.sample_position = offset;
        context.transport_frame = offset;
        context.loop_end_frame = 0;
        context.sample_rate = static_cast<double>(kRate);
        context.beats_per_minute = 120.0;
        context.playing = settings.playing ? 1 : 0;
        context.looping = 0;
        context.beat_position = 0.0;
        context.bar = 0;
        context.bar_beat = 0.0;
        context.musical_quarter_notes = 0.0;
        context.beats_per_bar = 4.0f;
        context.beat_unit = 4;
        descriptor->process(handle, result.in_l.data() + offset, result.in_r.data() + offset,
                            result.out_l.data() + offset, result.out_r.data() + offset,
                            kBlockFrames, &context);
    }
    descriptor->destroy(handle);
    return result;
}

void assert_dry(const RunResult& result, const char* message) {
    require(result.out_l == result.in_l && result.out_r == result.in_r, message);
}

void assert_stereo_aligned(const RunResult& result, const char* message) {
    for (size_t i = 0; i < result.out_l.size(); ++i)
        require(std::isfinite(result.out_l[i]) && std::isfinite(result.out_r[i]) &&
                    std::fabs(result.out_l[i] + result.out_r[i]) < 1.0e-3f,
                message);
}

void assert_format(const NnagaPluginDescriptorV1* descriptor, NnagaPluginHandle handle,
                   uint32_t port, float value, const char* expected, const char* message) {
    char output[64] = {};
    const uint32_t length = descriptor->format_parameter(handle, port, value, output, sizeof(output));
    require(length == std::strlen(expected) && std::strcmp(output, expected) == 0, message);
}
}

int main() {
    require(nnaga_plugin_entry(0) == nullptr, "unsupported ABI is rejected");
    const auto* descriptor = shuffle_descriptor();
    require(std::strcmp(descriptor->version, "1.1.1") == 0, "current shuffle version");
    require(descriptor->parameter_count == 11, "current shuffle parameter count");

    const char* expected_names[] = {"Enabled", "Shuffle", "Seed", "Mix", "Bars",
                                    "Shuffle start position", "Shuffle end position", "Grid from",
                                    "Grid to", "Grid mode", "Grid seed"};
    for (uint32_t i = 0; i < descriptor->parameter_count; ++i) {
        require(descriptor->parameters[i].port_index == 4 + i &&
                    std::strcmp(descriptor->parameters[i].name, expected_names[i]) == 0,
                "current shuffle port layout");
    }

    NnagaPluginHandle format_handle = descriptor->create();
    require(format_handle && descriptor->activate(format_handle, kRate, kBlockFrames),
            "formatter activation");
    descriptor->set_parameter(format_handle, kBarsPort, 0.0f);
    assert_format(descriptor, format_handle, kStartPort, 0.0f, "1:1:1", "start position format");
    assert_format(descriptor, format_handle, kEndPort, 1.0f, "2:1:1", "end position format");
    const char* grid_labels[] = {"1/32", "1/16", "1/8", "1/4", "1/2", "1 bar"};
    for (uint32_t i = 0; i < 6; ++i)
        assert_format(descriptor, format_handle, kGridFromPort, i / 5.0f, grid_labels[i],
                      "grid length format");
    const char* mode_labels[] = {"Allow dotted", "Allow triplets", "Use dotted", "Use triplets",
                                 "Allow dotted and triplets"};
    for (uint32_t i = 0; i < 5; ++i)
        assert_format(descriptor, format_handle, kGridModePort, i / 4.0f, mode_labels[i],
                      "grid mode format");
    descriptor->destroy(format_handle);

    Settings ranged;
    ranged.start = 0.25f;
    ranged.end = 0.75f;
    ranged.grid_from = 0.4f;
    ranged.grid_to = 0.4f;
    const RunResult ranged_result = run_shuffle(ranged);
    const uint32_t cycle = kBarFrames;
    for (uint32_t phase = 0; phase < cycle; ++phase)
        require(ranged_result.out_l[phase] == ranged_result.in_l[phase] &&
                    ranged_result.out_r[phase] == ranged_result.in_r[phase],
                "first complete cycle is captured dry");
    for (uint32_t phase = 0; phase < cycle; ++phase) {
        const uint32_t frame = cycle + phase;
        if (phase < cycle / 4 || phase >= 3 * cycle / 4) {
            require(ranged_result.out_l[frame] == ranged_result.in_l[frame] &&
                        ranged_result.out_r[frame] == ranged_result.in_r[frame],
                    "range is dry outside the half-open interval");
        } else {
            require(std::isfinite(ranged_result.out_l[frame]) &&
                        std::isfinite(ranged_result.out_r[frame]) &&
                        ranged_result.out_l[frame] > 0.0f && ranged_result.out_l[frame] < 1000000.0f &&
                        ranged_result.out_l[frame] != ranged_result.in_l[frame],
                    "active range is wet and never silent or unwritten");
        }
    }
    assert_stereo_aligned(ranged_result, "ranged shuffle remains stereo aligned");

    Settings identity;
    identity.amount = 0.0f;
    identity.grid_from = 1.0f;
    identity.grid_to = 1.0f;
    const RunResult identity_result = run_shuffle(identity);
    for (uint32_t phase = 0; phase < cycle; ++phase)
        require(identity_result.out_l[cycle + phase] == identity_result.in_l[phase] &&
                    identity_result.out_r[cycle + phase] == identity_result.in_r[phase],
                "zero amount replays at the original temporal rate");

    Settings random_grid;
    random_grid.grid_from = 0.0f;
    random_grid.grid_to = 1.0f;
    random_grid.grid_mode = 1.0f;
    random_grid.seed = 0.37f;
    random_grid.grid_seed = 0.19f;
    const RunResult same_grid_a = run_shuffle(random_grid);
    const RunResult same_grid_b = run_shuffle(random_grid);
    require(same_grid_a.out_l == same_grid_b.out_l && same_grid_a.out_r == same_grid_b.out_r,
            "same grid seed is deterministic");
    random_grid.grid_seed = 0.83f;
    const RunResult changed_grid = run_shuffle(random_grid);
    bool grid_changed = false;
    for (uint32_t i = cycle; i < 2 * cycle; ++i)
        if (same_grid_a.out_l[i] != changed_grid.out_l[i] || same_grid_a.out_r[i] != changed_grid.out_r[i]) {
            grid_changed = true;
            break;
        }
    require(grid_changed, "different grid seed changes variable grid pattern");

    random_grid.grid_seed = 0.19f;
    random_grid.seed = 0.91f;
    const RunResult changed_source = run_shuffle(random_grid);
    bool source_changed = false;
    for (uint32_t i = cycle; i < 2 * cycle; ++i)
        if (same_grid_a.out_l[i] != changed_source.out_l[i] || same_grid_a.out_r[i] != changed_source.out_r[i]) {
            source_changed = true;
            break;
        }
    require(source_changed, "source seed changes source selection");

    Settings broad_grid;
    broad_grid.bars = 8;
    broad_grid.grid_from = 0.0f;
    broad_grid.grid_to = 1.0f;
    broad_grid.grid_mode = 0.75f;
    broad_grid.grid_seed = 0.41f;
    const RunResult broad_result = run_shuffle(broad_grid);
    const uint32_t broad_cycle = 8 * kBarFrames;
    for (uint32_t phase = 0; phase < broad_cycle; ++phase) {
        const uint32_t frame = broad_cycle + phase;
        require(std::isfinite(broad_result.out_l[frame]) && std::isfinite(broad_result.out_r[frame]) &&
                    broad_result.out_l[frame] > 0.0f && broad_result.out_l[frame] < 1000000.0f,
                "triplet variable grid stays finite and covers every slice");
    }
    assert_stereo_aligned(broad_result, "triplet variable grid remains stereo aligned");

    Settings disabled;
    disabled.enabled = 0.0f;
    assert_dry(run_shuffle(disabled), "disabled shuffle is exactly dry");
    Settings stopped;
    stopped.playing = false;
    assert_dry(run_shuffle(stopped), "stopped shuffle is exactly dry");
    return 0;
}

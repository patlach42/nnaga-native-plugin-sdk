#include "nnaga/native_plugin.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

namespace {
constexpr uint32_t kRate = 48000;
constexpr uint32_t kBarFrames = 96000;
constexpr uint32_t kBlockFrames = 512;
constexpr uint32_t kEnabledPort = 4;
constexpr uint32_t kShufflePort = 5;
constexpr uint32_t kSeedPort = 6;
constexpr uint32_t kMixPort = 7;
constexpr uint32_t kBarsPort = 8;
constexpr uint32_t kStartPort = 9;
constexpr uint32_t kEndPort = 10;
constexpr uint32_t kGridPort = 11;
constexpr uint32_t kFillFromPort = 12;
constexpr uint32_t kFillToPort = 13;
constexpr uint32_t kGridModePort = 14;
constexpr uint32_t kGridSeedPort = 15;
constexpr uint32_t kTripletModePort = 16;

void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", message);
        std::exit(1);
    }
}

struct Settings {
    uint32_t bars = 1;
    float enabled = 1.0f;
    float shuffle = 1.0f;
    float seed = 0.25f;
    float mix = 1.0f;
    float start = 0.0f;
    float end = 1.0f;
    float grid = 0.2f;
    float fill_from = 0.2f;
    float fill_to = 0.6f;
    float grid_mode = 0.0f;
    float triplet_mode = 0.0f;
    float grid_seed = 0.0f;
    bool playing = true;
};

struct RunResult {
    std::vector<float> in_l;
    std::vector<float> in_r;
    std::vector<float> out_l;
    std::vector<float> out_r;
};

NnagaProcessContextV2 make_context(uint64_t transport_frame = 0) {
    NnagaProcessContextV2 context{};
    context.struct_size = sizeof(context);
    context.sample_position = transport_frame;
    context.transport_frame = transport_frame;
    context.sample_rate = static_cast<double>(kRate);
    context.beats_per_minute = 120.0;
    context.playing = 1;
    context.beats_per_bar = 4.0f;
    context.beat_unit = 4;
    return context;
}

const NnagaPluginDescriptorV2* shuffle_descriptor() {
    const auto* library = nnaga_plugin_entry(NNAGA_NATIVE_ABI_VERSION);
    require(library && library->struct_size >= sizeof(NnagaPluginLibraryV2) &&
                library->abi_version == NNAGA_NATIVE_ABI_VERSION && library->plugin_count == 1 && library->get_plugin,
            "shuffle library ABI v2 descriptor");
    const auto* descriptor = library->get_plugin(0);
    require(descriptor && descriptor->struct_size >= sizeof(NnagaPluginDescriptorV2) && descriptor->create &&
                descriptor->destroy && descriptor->activate && descriptor->deactivate && descriptor->reset &&
                descriptor->set_parameter && descriptor->format_parameter && descriptor->process,
            "shuffle plugin ABI v2 callbacks");
    return descriptor;
}

void set_settings(const NnagaPluginDescriptorV2* descriptor, NnagaPluginHandle handle, const Settings& settings) {
    descriptor->set_parameter(handle, kEnabledPort, settings.enabled);
    descriptor->set_parameter(handle, kShufflePort, settings.shuffle);
    descriptor->set_parameter(handle, kSeedPort, settings.seed);
    descriptor->set_parameter(handle, kMixPort, settings.mix);
    descriptor->set_parameter(handle, kBarsPort, static_cast<float>(settings.bars - 1) / 7.0f);
    descriptor->set_parameter(handle, kStartPort, settings.start);
    descriptor->set_parameter(handle, kEndPort, settings.end);
    descriptor->set_parameter(handle, kGridPort, settings.grid);
    descriptor->set_parameter(handle, kFillFromPort, settings.fill_from);
    descriptor->set_parameter(handle, kFillToPort, settings.fill_to);
    descriptor->set_parameter(handle, kGridModePort, settings.grid_mode);
    descriptor->set_parameter(handle, kGridSeedPort, settings.grid_seed);
    descriptor->set_parameter(handle, kTripletModePort, settings.triplet_mode);
}

RunResult run_shuffle(const Settings& settings, uint32_t cycles = 2) {
    const auto* descriptor = shuffle_descriptor();
    const uint32_t cycle_frames = settings.bars * kBarFrames;
    const uint32_t total_frames = cycle_frames * cycles;
    require(cycle_frames != 0 && total_frames % kBlockFrames == 0,
            "host run consists of contiguous 512-frame blocks");

    NnagaPluginHandle handle = descriptor->create();
    require(handle && descriptor->activate(handle, kRate, kBlockFrames), "shuffle activate");
    set_settings(descriptor, handle, settings);

    RunResult result;
    result.in_l.resize(total_frames);
    result.in_r.resize(total_frames);
    result.out_l.resize(total_frames, -99.0f);
    result.out_r.resize(total_frames, -99.0f);
    for (uint32_t frame = 0; frame < total_frames; ++frame) {
        const uint32_t cycle = frame / cycle_frames;
        const uint32_t phase = frame % cycle_frames;
        const float value = (cycle == 0 ? 1000.0f : 2000000.0f + 100000.0f * cycle) + phase;
        result.in_l[frame] = value;
        result.in_r[frame] = -value;
    }
    for (uint32_t offset = 0; offset < total_frames; offset += kBlockFrames) {
        NnagaProcessContextV2 context = make_context(offset);
        context.playing = settings.playing ? 1 : 0;
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

void assert_format(const NnagaPluginDescriptorV2* descriptor, NnagaPluginHandle handle,
                   uint32_t port, float value, const char* expected, const char* message) {
    char output[64] = {};
    const uint32_t length = descriptor->format_parameter(handle, port, value, output, sizeof(output));
    require(length == std::strlen(expected) && std::strcmp(output, expected) == 0, message);
}
}

int main() {
    require(nnaga_plugin_entry(0) == nullptr, "unsupported ABI is rejected");
    require(nnaga_plugin_entry(NNAGA_NATIVE_ABI_VERSION + 1) == nullptr, "future ABI is rejected");
    const auto* descriptor = shuffle_descriptor();
    require(std::strcmp(descriptor->id, "com.vibes.dsp.shuffle") == 0 &&
                std::strcmp(descriptor->alias, "shuffle") == 0 && descriptor->max_frames == NNAGA_NATIVE_MAX_FRAMES &&
                descriptor->audio_inputs == 2 && descriptor->audio_outputs == 2 && descriptor->parameter_count == 13 &&
                descriptor->realtime_class == NNAGA_REALTIME_CERTIFIED_IN_PROCESS,
            "shuffle descriptor identity and bounds");

    const char* expected_names[] = {"Enabled", "Shuffle", "Seed", "Mix", "Bars",
                                    "Shuffle start position", "Shuffle end position", "Grid",
                                    "Fill length from", "Fill length to", "Grid mode", "Grid seed",
                                    "Triplet mode"};
    for (uint32_t i = 0; i < descriptor->parameter_count; ++i) {
        require(descriptor->parameters[i].struct_size >= sizeof(NnagaParameterV2) &&
                    descriptor->parameters[i].port_index == 4 + i &&
                    std::strcmp(descriptor->parameters[i].name, expected_names[i]) == 0,
                "shuffle parameter ABI v2 layout");
    }
    const NnagaParameterV2& grid_parameter = descriptor->parameters[7];
    require((grid_parameter.flags & NNAGA_PARAMETER_ENUM) != 0 && grid_parameter.scale_point_count == 6 &&
                grid_parameter.scale_points && grid_parameter.step_count == 0, "grid enum metadata");
    const NnagaParameterV2& triplet_parameter = descriptor->parameters[12];
    require((triplet_parameter.flags & NNAGA_PARAMETER_ENUM) != 0 && triplet_parameter.scale_point_count == 2 &&
                triplet_parameter.scale_points && triplet_parameter.step_count == 0 &&
                std::strcmp(triplet_parameter.scale_points[0].label, "Retrigger same segment") == 0 &&
                std::strcmp(triplet_parameter.scale_points[1].label, "Use separate segments") == 0,
            "triplet mode enum metadata");

    NnagaPluginHandle handle = descriptor->create();
    require(handle != nullptr, "shuffle create");
    require(descriptor->activate(handle, 48000.0, 0) == 0, "zero max frame activation rejected");
    require(descriptor->activate(handle, 48000.0, NNAGA_NATIVE_MAX_FRAMES + 1u) == 0,
            "oversize max frame activation rejected");
    require(descriptor->activate(handle, -1.0, 512) == 0, "invalid sample rate activation rejected");
    require(descriptor->activate(handle, 48000.0, kBlockFrames) != 0, "shuffle activation");

    // Oversize and null process calls are all-or-nothing: no partial output is observable.
    std::vector<float> input(513, 0.25f), output_left(513, -7.0f), output_right(513, -7.0f);
    NnagaProcessContextV2 timing = make_context();
    descriptor->process(handle, input.data(), input.data(), output_left.data(), output_right.data(), 513, &timing);
    for (float value : output_left) require(value == -7.0f, "oversize shuffle process leaves left output untouched");
    for (float value : output_right) require(value == -7.0f, "oversize shuffle process leaves right output untouched");
    descriptor->process(handle, nullptr, input.data(), output_left.data(), output_right.data(), 1, &timing);
    require(output_left[0] == -7.0f && output_right[0] == -7.0f, "null shuffle input leaves output untouched");
    descriptor->process(handle, input.data(), input.data(), output_left.data(), output_right.data(), 0, &timing);
    require(output_left[0] == -7.0f && output_right[0] == -7.0f, "zero-frame process leaves output untouched");
    descriptor->destroy(handle);

    NnagaPluginHandle format_handle = descriptor->create();
    require(format_handle && descriptor->activate(format_handle, kRate, kBlockFrames), "formatter activation");
    assert_format(descriptor, format_handle, kStartPort, 0.0f, "1:1:1", "start position format");
    assert_format(descriptor, format_handle, kEndPort, 1.0f, "2:1:1", "end position format");
    assert_format(descriptor, format_handle, kGridPort, 0.2f, "1/16", "grid length format");
    assert_format(descriptor, format_handle, kBarsPort, 0.0f, "1 bar", "bar format");
    char tiny[1] = {'x'};
    require(descriptor->format_parameter(format_handle, kGridPort, 0.2f, tiny, sizeof(tiny)) == 0 && tiny[0] == '\0',
            "bounded shuffle parameter format");
    descriptor->destroy(format_handle);

    Settings ranged;
    ranged.start = 0.25f;
    ranged.end = 0.75f;
    ranged.fill_from = 0.4f;
    ranged.fill_to = 0.4f;
    const RunResult ranged_result = run_shuffle(ranged);
    const uint32_t cycle = kBarFrames;
    for (uint32_t phase = 0; phase < cycle; ++phase)
        require(ranged_result.out_l[phase] == ranged_result.in_l[phase] &&
                    ranged_result.out_r[phase] == ranged_result.in_r[phase],
                "first complete cycle is captured dry");
    for (uint32_t phase = 0; phase < cycle; ++phase) {
        const uint32_t frame = cycle + phase;
        if (phase < cycle / 4 || phase >= 3 * cycle / 4)
            require(ranged_result.out_l[frame] == ranged_result.in_l[frame] &&
                        ranged_result.out_r[frame] == ranged_result.in_r[frame],
                    "range is dry outside the half-open interval");
        else
            require(std::isfinite(ranged_result.out_l[frame]) && std::isfinite(ranged_result.out_r[frame]) &&
                        ranged_result.out_l[frame] > 0.0f && ranged_result.out_l[frame] < 1000000.0f &&
                        ranged_result.out_l[frame] != ranged_result.in_l[frame],
                    "active range is wet and fully written");
    }
    assert_stereo_aligned(ranged_result, "ranged shuffle remains stereo aligned");

    Settings identity;
    identity.shuffle = 0.0f;
    identity.grid = 1.0f;
    identity.fill_from = 1.0f;
    identity.fill_to = 1.0f;
    const RunResult identity_result = run_shuffle(identity);
    for (uint32_t phase = 0; phase < cycle; ++phase)
        require(identity_result.out_l[cycle + phase] == identity_result.in_l[phase] &&
                    identity_result.out_r[cycle + phase] == identity_result.in_r[phase],
                "zero shuffle replays original temporal rate");

    Settings random_grid;
    random_grid.fill_from = 0.0f;
    random_grid.fill_to = 1.0f;
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

    Settings triplet;
    triplet.grid = 0.0f;
    triplet.fill_from = 0.0f;
    triplet.fill_to = 0.0f;
    triplet.grid_mode = 0.8f;
    triplet.seed = 0.37f;
    triplet.start = 0.0f;
    triplet.end = 0.2f;
    const RunResult same_start = run_shuffle(triplet);
    triplet.triplet_mode = 1.0f;
    const RunResult separate = run_shuffle(triplet);
    const uint32_t segment_frames = kBarFrames / 96;
    const uint32_t probe = segment_frames / 2;
    require(std::fabs(same_start.out_l[cycle + probe] - same_start.out_l[cycle + probe + segment_frames]) < 1.0e-3f,
            "same-start triplets reuse source segment");
    require(std::fabs(separate.out_l[cycle + probe] - separate.out_l[cycle + probe + segment_frames]) > 1.0e-3f,
            "separate triplets use distinct source segments");

    Settings disabled;
    disabled.enabled = 0.0f;
    assert_dry(run_shuffle(disabled), "disabled shuffle is exactly dry");
    Settings stopped;
    stopped.playing = false;
    assert_dry(run_shuffle(stopped), "stopped shuffle is exactly dry");

    // Invalid timing, giant frame arithmetic, and a wrapping transport position must remain bounded and finite.
    NnagaPluginHandle edge_handle = descriptor->create();
    require(edge_handle && descriptor->activate(edge_handle, 1000.0, kBlockFrames), "edge-rate activation");
    std::vector<float> edge_in(32), edge_left(32, -3.0f), edge_right(32, -3.0f);
    for (uint32_t i = 0; i < edge_in.size(); ++i) edge_in[i] = static_cast<float>(i + 1);
    const double invalid_rates[] = {0.0, -1.0, std::numeric_limits<double>::infinity(),
                                    std::numeric_limits<double>::quiet_NaN()};
    for (double rate : invalid_rates) {
        NnagaProcessContextV2 edge = make_context();
        edge.sample_rate = rate;
        descriptor->process(edge_handle, edge_in.data(), edge_in.data(), edge_left.data(), edge_right.data(),
                            static_cast<uint32_t>(edge_in.size()), &edge);
        require(edge_left == edge_in && edge_right == edge_in, "invalid timing is safely dry");
    }
    NnagaProcessContextV2 giant = make_context(UINT64_MAX);
    giant.sample_rate = std::numeric_limits<double>::max();
    giant.beats_per_minute = std::numeric_limits<double>::min();
    descriptor->process(edge_handle, edge_in.data(), edge_in.data(), edge_left.data(), edge_right.data(),
                        static_cast<uint32_t>(edge_in.size()), &giant);
    for (uint32_t i = 0; i < edge_in.size(); ++i)
        require(std::isfinite(edge_left[i]) && std::isfinite(edge_right[i]), "extreme timing remains finite");
    descriptor->destroy(edge_handle);
    return 0;
}

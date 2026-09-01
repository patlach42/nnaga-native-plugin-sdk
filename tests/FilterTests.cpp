#include "nnaga/native_plugin.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", message);
        std::exit(1);
    }
}

float normalized_log(float value, float minimum, float maximum) {
    return std::log(value / minimum) / std::log(maximum / minimum);
}

float rms(const std::vector<float>& data, uint32_t offset) {
    double total = 0.0;
    for (uint32_t i = offset; i < data.size(); ++i) total += static_cast<double>(data[i]) * data[i];
    return static_cast<float>(std::sqrt(total / static_cast<double>(data.size() - offset)));
}

void fill_sine(std::vector<float>& out, float frequency) {
    for (uint32_t i = 0; i < out.size(); ++i)
        out[i] = std::sin(2.0f * 3.14159265358979323846f * frequency * static_cast<float>(i) / 48000.0f);
}

NnagaProcessContextV2 context(uint64_t transport_frame = 0) {
    NnagaProcessContextV2 value{};
    value.struct_size = sizeof(value);
    value.sample_position = transport_frame;
    value.transport_frame = transport_frame;
    value.sample_rate = 48000.0;
    value.beats_per_minute = 120.0;
    value.playing = 1;
    value.beats_per_bar = 4.0f;
    value.beat_unit = 4;
    return value;
}

float run_gain(const NnagaPluginDescriptorV2* descriptor, NnagaPluginHandle handle,
               float type, float cutoff, float signal_frequency) {
    descriptor->set_parameter(handle, 4, type);
    descriptor->set_parameter(handle, 5, normalized_log(cutoff, 20.0f, 20000.0f));
    descriptor->set_parameter(handle, 6, normalized_log(0.70710678f, 0.1f, 18.0f));
    std::vector<float> input(48000), output(48000);
    fill_sine(input, signal_frequency);
    for (uint32_t offset = 0; offset < input.size(); offset += 512) {
        NnagaProcessContextV2 timing = context(offset);
        descriptor->process(handle, input.data() + offset, input.data() + offset,
                            output.data() + offset, output.data() + offset,
                            std::min<uint32_t>(512, static_cast<uint32_t>(input.size() - offset)), &timing);
    }
    return rms(output, 24000) / rms(input, 24000);
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
    const NnagaPluginLibraryV2* library = nnaga_plugin_entry(NNAGA_NATIVE_ABI_VERSION);
    require(library && library->struct_size >= sizeof(NnagaPluginLibraryV2) &&
                library->abi_version == NNAGA_NATIVE_ABI_VERSION && library->plugin_count == 1 && library->get_plugin,
            "library ABI v2 descriptor");
    require(library->get_plugin(1) == nullptr, "out-of-range plugin index is rejected");
    const NnagaPluginDescriptorV2* descriptor = library->get_plugin(0);
    require(descriptor && descriptor->struct_size >= sizeof(NnagaPluginDescriptorV2) &&
                descriptor->parameter_count == 3 && descriptor->audio_inputs == 2 && descriptor->audio_outputs == 2 &&
                descriptor->max_frames == NNAGA_NATIVE_MAX_FRAMES &&
                descriptor->realtime_class == NNAGA_REALTIME_CERTIFIED_IN_PROCESS && descriptor->parameters &&
                descriptor->create && descriptor->destroy && descriptor->activate && descriptor->deactivate &&
                descriptor->reset && descriptor->set_parameter && descriptor->format_parameter && descriptor->process,
            "filter ABI v2 descriptor");
    require(std::strcmp(descriptor->id, "com.vibes.dsp.filter") == 0 && std::strcmp(descriptor->alias, "filter") == 0,
            "filter id and alias");
    for (uint32_t i = 0; i < descriptor->parameter_count; ++i) {
        require(descriptor->parameters[i].struct_size >= sizeof(NnagaParameterV2) &&
                    descriptor->parameters[i].port_index == 4 + i,
                "filter parameter ABI v2 layout");
    }

    NnagaPluginHandle handle = descriptor->create();
    require(handle != nullptr, "filter create");
    require(descriptor->activate(handle, 48000.0, 0) == 0, "zero max frame activation rejected");
    require(descriptor->activate(handle, 48000.0, NNAGA_NATIVE_MAX_FRAMES + 1u) == 0,
            "oversize max frame activation rejected");
    require(descriptor->activate(handle, std::numeric_limits<double>::quiet_NaN(), 512) == 0,
            "non-finite sample rate activation rejected");
    require(descriptor->activate(handle, 48000.0, 512) != 0, "filter activate");

    const float lp_low = run_gain(descriptor, handle, 0.0f, 1000.0f, 100.0f);
    descriptor->reset(handle);
    const float lp_high = run_gain(descriptor, handle, 0.0f, 1000.0f, 10000.0f);
    descriptor->reset(handle);
    const float hp_low = run_gain(descriptor, handle, 1.0f, 1000.0f, 100.0f);
    descriptor->reset(handle);
    const float hp_high = run_gain(descriptor, handle, 1.0f, 1000.0f, 10000.0f);
    require(lp_low >= 0.94f && lp_high <= 0.02f, "low-pass response");
    require(hp_low <= 0.02f && hp_high >= 0.94f, "high-pass response");

    // A callback quantum larger than the activated hard maximum must not partially overwrite output.
    std::vector<float> oversized_input(513, 0.25f), oversized_output(513, -7.0f);
    NnagaProcessContextV2 timing = context();
    descriptor->process(handle, oversized_input.data(), oversized_input.data(),
                        oversized_output.data(), oversized_output.data(), 513, &timing);
    for (float value : oversized_output) require(value == -7.0f, "oversize process leaves output untouched");
    descriptor->process(handle, nullptr, oversized_input.data(), oversized_output.data(), oversized_output.data(), 1, &timing);
    require(oversized_output[0] == -7.0f, "null input leaves output untouched");

    // Reset and zero input have a deterministic, denormal-free result and overwrite every output sample.
    std::vector<float> zeros(257, 0.0f), zero_output_left(257, 9.0f), zero_output_right(257, 9.0f);
    descriptor->reset(handle);
    descriptor->process(handle, zeros.data(), zeros.data(), zero_output_left.data(), zero_output_right.data(), 257, &timing);
    for (uint32_t i = 0; i < 257; ++i)
        require(zero_output_left[i] == 0.0f && zero_output_right[i] == 0.0f, "zero process overwrites without denormals");

    std::vector<float> left(257, 0.0f), right(257, 0.0f), in_place_left, in_place_right;
    left[0] = 1.0f;
    right[0] = -1.0f;
    in_place_left = left;
    in_place_right = right;
    std::vector<float> out_left(257), out_right(257);
    descriptor->set_parameter(handle, 5, 0.0f);
    descriptor->set_parameter(handle, 6, 1.0f);
    require(descriptor->activate(handle, 48000.0, 512), "re-activate extreme filter");
    descriptor->process(handle, left.data(), right.data(), out_left.data(), out_right.data(), 257, &timing);
    descriptor->reset(handle);
    descriptor->process(handle, in_place_left.data(), in_place_right.data(), in_place_left.data(), in_place_right.data(), 257, &timing);
    for (uint32_t i = 0; i < 257; ++i) {
        require(std::isfinite(out_left[i]) && std::isfinite(out_right[i]), "finite extremes");
        require(std::fabs(out_left[i] - in_place_left[i]) < 1.0e-6f &&
                    std::fabs(out_right[i] - in_place_right[i]) < 1.0e-6f,
                "in-place parity");
    }

    descriptor->set_parameter(handle, 4, 0.0f);
    assert_format(descriptor, handle, 4, 0.0f, "Low-pass", "low-pass format");
    assert_format(descriptor, handle, 4, 1.0f, "High-pass", "high-pass format");
    char tiny[1] = {'x'};
    require(descriptor->format_parameter(handle, 4, 0.0f, tiny, sizeof(tiny)) > 0 && tiny[0] == '\0',
            "bounded parameter format");
    descriptor->deactivate(handle);
    descriptor->destroy(handle);
    return 0;
}

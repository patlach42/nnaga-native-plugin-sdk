#include "nnaga/native_plugin.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
constexpr float kPi = 3.14159265358979323846f;
constexpr float kMinFrequency = 20.0f;
constexpr float kMaxFrequency = 20000.0f;
constexpr float kMinQ = 0.1f;
constexpr float kMaxQ = 18.0f;
constexpr uint32_t kTypePort = 4;
constexpr uint32_t kFrequencyPort = 5;
constexpr uint32_t kQPort = 6;
constexpr uint32_t kTypeFadeSamples = 64;

float clamp01(float value) noexcept {
    return std::isfinite(value) ? (value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value)) : 0.0f;
}

float log_to_normalized(float value, float minimum, float maximum) noexcept {
    return std::log(value / minimum) / std::log(maximum / minimum);
}

float normalized_to_log(float value, float minimum, float maximum) noexcept {
    return minimum * std::exp(std::log(maximum / minimum) * clamp01(value));
}

struct ChannelState {
    float ic1eq;
    float ic2eq;
};

struct Filter {
    ChannelState left;
    ChannelState right;
    float frequency_normalized;
    float q_normalized;
    float type_normalized;
    float g;
    float k;
    float target_g;
    float target_k;
    float g_increment;
    float k_increment;
    uint32_t coefficient_ramp_remaining;
    float mix;
    float target_mix;
    float mix_increment;
    uint32_t type_fade_remaining;
    double sample_rate;
    uint32_t max_frames;
};

void clear(Filter* filter) noexcept {
    filter->left = {};
    filter->right = {};
}

void update_targets(Filter* filter) noexcept {
    const float cutoff = normalized_to_log(filter->frequency_normalized, kMinFrequency, kMaxFrequency);
    const float safe_cutoff = cutoff < static_cast<float>(filter->sample_rate * 0.49)
        ? cutoff : static_cast<float>(filter->sample_rate * 0.49);
    const float q = normalized_to_log(filter->q_normalized, kMinQ, kMaxQ);
    filter->target_g = std::tan(kPi * safe_cutoff / static_cast<float>(filter->sample_rate));
    filter->target_k = 1.0f / q;
    const uint32_t ramp_samples = static_cast<uint32_t>(filter->sample_rate * 0.010);
    filter->coefficient_ramp_remaining = ramp_samples == 0 ? 1 : ramp_samples;
    filter->g_increment = (filter->target_g - filter->g) /
        static_cast<float>(filter->coefficient_ramp_remaining);
    filter->k_increment = (filter->target_k - filter->k) /
        static_cast<float>(filter->coefficient_ramp_remaining);
}

void update_type_target(Filter* filter) noexcept {
    filter->target_mix = filter->type_normalized >= 0.5f ? 1.0f : 0.0f;
    filter->type_fade_remaining = kTypeFadeSamples;
    filter->mix_increment = (filter->target_mix - filter->mix) / static_cast<float>(kTypeFadeSamples);
}

float process_one(ChannelState& state, float input, float g, float k, float mix) noexcept {
    const float a1 = 1.0f / (1.0f + g * (g + k));
    const float a2 = g * a1;
    const float a3 = g * a2;
    const float v3 = input - state.ic2eq;
    const float v1 = a1 * state.ic1eq + a2 * v3;
    const float v2 = state.ic2eq + a2 * state.ic1eq + a3 * v3;
    state.ic1eq = 2.0f * v1 - state.ic1eq;
    state.ic2eq = 2.0f * v2 - state.ic2eq;
    const float low_pass = v2;
    const float high_pass = input - k * v1 - v2;
    return low_pass + (high_pass - low_pass) * mix;
}

NnagaPluginHandle create() noexcept {
    Filter* filter = static_cast<Filter*>(std::malloc(sizeof(Filter)));
    if (!filter) return nullptr;
    std::memset(filter, 0, sizeof(Filter));
    filter->frequency_normalized = log_to_normalized(1000.0f, kMinFrequency, kMaxFrequency);
    filter->q_normalized = log_to_normalized(0.70710678f, kMinQ, kMaxQ);
    filter->sample_rate = 48000.0;
    filter->mix = 0.0f;
    filter->target_mix = 0.0f;
    return filter;
}

void destroy(NnagaPluginHandle handle) noexcept {
    std::free(handle);
}

int32_t activate(NnagaPluginHandle handle, double sample_rate, uint32_t max_frames) noexcept {
    Filter* filter = static_cast<Filter*>(handle);
    if (!filter || !std::isfinite(sample_rate) || sample_rate < 1000.0 ||
        max_frames == 0 || max_frames > NNAGA_NATIVE_MAX_FRAMES) return 0;
    filter->max_frames = max_frames;
    filter->sample_rate = sample_rate;
    clear(filter);
    filter->g = 0.0f;
    filter->k = 1.0f;
    update_targets(filter);
    filter->g = filter->target_g;
    filter->k = filter->target_k;
    filter->coefficient_ramp_remaining = 0;
    update_type_target(filter);
    filter->mix = filter->target_mix;
    filter->type_fade_remaining = 0;
    return 1;
}

void deactivate(NnagaPluginHandle) noexcept {}

void reset(NnagaPluginHandle handle) noexcept {
    if (Filter* filter = static_cast<Filter*>(handle)) clear(filter);
}

void set_parameter(NnagaPluginHandle handle, uint32_t port_index, float normalized) noexcept {
    Filter* filter = static_cast<Filter*>(handle);
    if (!filter) return;
    normalized = clamp01(normalized);
    switch (port_index) {
        case kTypePort:
            if (filter->type_normalized != normalized) {
                filter->type_normalized = normalized;
                update_type_target(filter);
            }
            break;
        case kFrequencyPort:
            if (filter->frequency_normalized != normalized) {
                filter->frequency_normalized = normalized;
                update_targets(filter);
            }
            break;
        case kQPort:
            if (filter->q_normalized != normalized) {
                filter->q_normalized = normalized;
                update_targets(filter);
            }
            break;
        default:
            break;
    }
}

uint32_t format_parameter(NnagaPluginHandle, uint32_t port_index, float normalized,
                          char* output, uint32_t capacity) noexcept {
    if (!output || capacity == 0) return 0;
    int written = 0;
    switch (port_index) {
        case kTypePort:
            written = std::snprintf(output, capacity, "%s", clamp01(normalized) >= 0.5f ? "High-pass" : "Low-pass");
            break;
        case kFrequencyPort: {
            const float frequency = normalized_to_log(normalized, kMinFrequency, kMaxFrequency);
            written = frequency >= 1000.0f
                ? std::snprintf(output, capacity, "%.2f kHz", frequency / 1000.0f)
                : std::snprintf(output, capacity, "%.0f Hz", frequency);
            break;
        }
        case kQPort:
            written = std::snprintf(output, capacity, "%.2f", normalized_to_log(normalized, kMinQ, kMaxQ));
            break;
        default:
            output[0] = '\0';
            return 0;
    }
    return written > 0 ? static_cast<uint32_t>(written) : 0;
}

uint32_t latency_frames(NnagaPluginHandle) noexcept { return 0; }

void process(NnagaPluginHandle handle, const float* input_left, const float* input_right,
             float* output_left, float* output_right, uint32_t frames,
             const NnagaProcessContextV2*) noexcept {
    Filter* filter = static_cast<Filter*>(handle);
    if (!filter || !input_left || !input_right || !output_left || !output_right ||
        frames == 0 || frames > filter->max_frames) return;
    for (uint32_t i = 0; i < frames; ++i) {
        if (filter->coefficient_ramp_remaining != 0) {
            filter->g += filter->g_increment;
            filter->k += filter->k_increment;
            if (--filter->coefficient_ramp_remaining == 0) {
                filter->g = filter->target_g;
                filter->k = filter->target_k;
            }
        }
        if (filter->type_fade_remaining != 0) {
            filter->mix += filter->mix_increment;
            if (--filter->type_fade_remaining == 0) filter->mix = filter->target_mix;
        }
        output_left[i] = process_one(filter->left, input_left[i], filter->g, filter->k, filter->mix);
        output_right[i] = process_one(filter->right, input_right[i], filter->g, filter->k, filter->mix);
    }
    if (std::fabs(filter->left.ic1eq) < 1.0e-20f) filter->left.ic1eq = 0.0f;
    if (std::fabs(filter->left.ic2eq) < 1.0e-20f) filter->left.ic2eq = 0.0f;
    if (std::fabs(filter->right.ic1eq) < 1.0e-20f) filter->right.ic1eq = 0.0f;
    if (std::fabs(filter->right.ic2eq) < 1.0e-20f) filter->right.ic2eq = 0.0f;
}

const NnagaScalePointV2 kTypePoints[] = {
    {sizeof(NnagaScalePointV2), 0.0f, "Low-pass"},
    {sizeof(NnagaScalePointV2), 1.0f, "High-pass"},
};
const NnagaParameterV2 kParameters[] = {
    {sizeof(NnagaParameterV2), kTypePort, "Type", "type", "", NNAGA_PARAMETER_ENUM,
     0.0f, 2, kTypePoints, 0},
    {sizeof(NnagaParameterV2), kFrequencyPort, "Frequency", "frequency", "Hz", 0,
     log_to_normalized(1000.0f, kMinFrequency, kMaxFrequency), 0, nullptr, 0},
    {sizeof(NnagaParameterV2), kQPort, "Q", "q", "", 0,
     log_to_normalized(0.70710678f, kMinQ, kMaxQ), 0, nullptr, 0},
};
const NnagaPluginDescriptorV2 kDescriptor = {
    sizeof(NnagaPluginDescriptorV2), "com.vibes.dsp.filter", "filter", "NNAGA Filter", "NNAGA", "1.0.0",
    2, 2, 3, NNAGA_NATIVE_MAX_FRAMES, NNAGA_REALTIME_CERTIFIED_IN_PROCESS, kParameters, create, destroy, activate,
    deactivate, reset, set_parameter, format_parameter, latency_frames, process,
};

const NnagaPluginDescriptorV2* get_plugin(uint32_t index) noexcept {
    return index == 0 ? &kDescriptor : nullptr;
}

const NnagaPluginLibraryV2 kLibrary = {
    sizeof(NnagaPluginLibraryV2), NNAGA_NATIVE_ABI_VERSION, 1, get_plugin,
};
} // namespace

extern "C" NNAGA_NATIVE_EXPORT const NnagaPluginLibraryV2*
nnaga_plugin_entry(uint32_t host_abi_version) noexcept {
    return host_abi_version == NNAGA_NATIVE_ABI_VERSION ? &kLibrary : nullptr;
}

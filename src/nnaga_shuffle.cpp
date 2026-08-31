#include "nnaga/native_plugin.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
constexpr uint32_t kEnabledPort = 4;
constexpr uint32_t kGridPort = 5;
constexpr uint32_t kAmountPort = 6;
constexpr uint32_t kSeedPort = 7;
constexpr uint32_t kMixPort = 8;
constexpr uint32_t kMaxSteps = 64;
constexpr uint32_t kCrossfadeFrames = 32;

struct Shuffle {
    float* ring_left = nullptr;
    float* ring_right = nullptr;
    uint32_t capacity = 0;
    uint32_t write = 0;
    uint64_t captured = 0;
    uint64_t previous_step = UINT64_MAX;
    uint64_t previous_loop_frames = 0;
    uint32_t steps = 0;
    uint32_t permutation[kMaxSteps]{};
    uint32_t rng = 1;
    float enabled = 1.0f;
    float grid = 0.5f;
    float amount = 1.0f;
    float seed = 0.0f;
    float mix = 1.0f;
    float previous_left = 0.0f;
    float previous_right = 0.0f;
    uint32_t fade_remaining = 0;
    double sample_rate = 48000.0;
};

float clamp01(float value) noexcept {
    return std::isfinite(value) ? (value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value)) : 0.0f;
}

uint32_t next_random(Shuffle* shuffle) noexcept {
    uint32_t x = shuffle->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    shuffle->rng = x ? x : 1;
    return shuffle->rng;
}

uint32_t grid_division(float grid) noexcept {
    return grid < 0.25f ? 4u : (grid < 0.75f ? 8u : 16u);
}

void make_pattern(Shuffle* shuffle, uint32_t steps, uint64_t loop_frames) noexcept {
    shuffle->steps = steps;
    shuffle->rng = static_cast<uint32_t>(shuffle->seed * 4294967294.0f) ^
        static_cast<uint32_t>(loop_frames) ^ static_cast<uint32_t>(loop_frames >> 32);
    if (!shuffle->rng) shuffle->rng = 1;
    for (uint32_t i = 0; i < steps; ++i) shuffle->permutation[i] = i;
    for (uint32_t i = steps; i > 1; --i) {
        if ((next_random(shuffle) & 0xffffu) > static_cast<uint32_t>(shuffle->amount * 65535.0f)) continue;
        const uint32_t a = i - 1;
        const uint32_t b = next_random(shuffle) % i;
        const uint32_t swap = shuffle->permutation[a];
        shuffle->permutation[a] = shuffle->permutation[b];
        shuffle->permutation[b] = swap;
    }
}

NnagaPluginHandle create() noexcept {
    Shuffle* shuffle = static_cast<Shuffle*>(std::calloc(1, sizeof(Shuffle)));
    if (!shuffle) return nullptr;
    shuffle->enabled = 1.0f;
    shuffle->grid = 0.5f;
    shuffle->amount = 1.0f;
    shuffle->mix = 1.0f;
    return shuffle;
}

void destroy(NnagaPluginHandle handle) noexcept {
    Shuffle* shuffle = static_cast<Shuffle*>(handle);
    if (!shuffle) return;
    std::free(shuffle->ring_left);
    std::free(shuffle->ring_right);
    std::free(shuffle);
}

int32_t activate(NnagaPluginHandle handle, double sample_rate, uint32_t) noexcept {
    Shuffle* shuffle = static_cast<Shuffle*>(handle);
    if (!shuffle || !std::isfinite(sample_rate) || sample_rate < 1000.0) return 0;
    const uint32_t capacity = static_cast<uint32_t>(sample_rate * 16.0);
    float* left = static_cast<float*>(std::calloc(capacity, sizeof(float)));
    float* right = static_cast<float*>(std::calloc(capacity, sizeof(float)));
    if (!left || !right) { std::free(left); std::free(right); return 0; }
    std::free(shuffle->ring_left);
    std::free(shuffle->ring_right);
    shuffle->ring_left = left;
    shuffle->ring_right = right;
    shuffle->capacity = capacity;
    shuffle->sample_rate = sample_rate;
    shuffle->write = 0;
    shuffle->captured = 0;
    shuffle->previous_step = UINT64_MAX;
    shuffle->previous_loop_frames = 0;
    shuffle->fade_remaining = 0;
    return 1;
}

void deactivate(NnagaPluginHandle) noexcept {}
void reset(NnagaPluginHandle handle) noexcept {
    Shuffle* shuffle = static_cast<Shuffle*>(handle);
    if (!shuffle || !shuffle->ring_left) return;
    std::memset(shuffle->ring_left, 0, shuffle->capacity * sizeof(float));
    std::memset(shuffle->ring_right, 0, shuffle->capacity * sizeof(float));
    shuffle->write = 0;
    shuffle->captured = 0;
    shuffle->previous_step = UINT64_MAX;
}

void set_parameter(NnagaPluginHandle handle, uint32_t port, float value) noexcept {
    Shuffle* shuffle = static_cast<Shuffle*>(handle);
    if (!shuffle) return;
    switch (port) {
        case kEnabledPort: shuffle->enabled = clamp01(value); break;
        case kGridPort: shuffle->grid = clamp01(value); break;
        case kAmountPort: shuffle->amount = clamp01(value); break;
        case kSeedPort: shuffle->seed = clamp01(value); break;
        case kMixPort: shuffle->mix = clamp01(value); break;
        default: break;
    }
}

uint32_t format_parameter(NnagaPluginHandle, uint32_t port, float value, char* output, uint32_t capacity) noexcept {
    if (!output || !capacity) return 0;
    const char* text = "";
    if (port == kEnabledPort) text = clamp01(value) >= 0.5f ? "On" : "Off";
    if (port == kGridPort) text = clamp01(value) < 0.25f ? "1/4" : (clamp01(value) < 0.75f ? "1/8" : "1/16");
    const int written = port == kAmountPort || port == kMixPort
        ? std::snprintf(output, capacity, "%.0f%%", clamp01(value) * 100.0f)
        : (port == kSeedPort ? std::snprintf(output, capacity, "%u", static_cast<uint32_t>(clamp01(value) * 65535.0f))
                             : std::snprintf(output, capacity, "%s", text));
    return written > 0 ? static_cast<uint32_t>(written) : 0;
}

void process(NnagaPluginHandle handle, const float* in_l, const float* in_r, float* out_l, float* out_r,
             uint32_t frames, const NnagaProcessContextV1* context) noexcept {
    Shuffle* shuffle = static_cast<Shuffle*>(handle);
    if (!shuffle || !shuffle->ring_left || !in_l || !in_r || !out_l || !out_r || !context) return;
    const uint64_t loop_frames = context->looping && context->loop_end_frame > 0
        ? context->loop_end_frame : 0;
    if (loop_frames == 0 || loop_frames > shuffle->capacity || shuffle->enabled < 0.5f) {
        for (uint32_t i = 0; i < frames; ++i) { out_l[i] = in_l[i]; out_r[i] = in_r[i]; }
        return;
    }
    const uint32_t division = grid_division(shuffle->grid);
    const uint64_t slice_frames = loop_frames / division;
    const uint32_t steps = static_cast<uint32_t>(loop_frames / slice_frames);
    for (uint32_t i = 0; i < frames; ++i) {
        const uint64_t frame = (context->transport_frame + i) % loop_frames;
        const uint64_t step = frame / slice_frames;
        if (loop_frames != shuffle->previous_loop_frames || step != shuffle->previous_step) {
            if (shuffle->previous_step != UINT64_MAX) shuffle->fade_remaining = kCrossfadeFrames;
            if (step == 0 || loop_frames != shuffle->previous_loop_frames) make_pattern(shuffle, steps, loop_frames);
            shuffle->previous_loop_frames = loop_frames;
            shuffle->previous_step = step;
        }
        shuffle->ring_left[shuffle->write] = in_l[i];
        shuffle->ring_right[shuffle->write] = in_r[i];
        float wet_l = in_l[i], wet_r = in_r[i];
        if (shuffle->captured >= loop_frames) {
            const uint64_t source_frame = static_cast<uint64_t>(shuffle->permutation[step]) * slice_frames + frame % slice_frames;
            const uint32_t read = static_cast<uint32_t>((shuffle->write + shuffle->capacity - loop_frames + source_frame) % shuffle->capacity);
            wet_l = shuffle->ring_left[read];
            wet_r = shuffle->ring_right[read];
        }
        if (shuffle->fade_remaining) {
            const float phase = 1.0f - static_cast<float>(shuffle->fade_remaining) / kCrossfadeFrames;
            wet_l = shuffle->previous_left + (wet_l - shuffle->previous_left) * phase;
            wet_r = shuffle->previous_right + (wet_r - shuffle->previous_right) * phase;
            --shuffle->fade_remaining;
        }
        out_l[i] = in_l[i] + (wet_l - in_l[i]) * shuffle->mix;
        out_r[i] = in_r[i] + (wet_r - in_r[i]) * shuffle->mix;
        shuffle->previous_left = out_l[i];
        shuffle->previous_right = out_r[i];
        shuffle->write = (shuffle->write + 1) % shuffle->capacity;
        ++shuffle->captured;
    }
}

constexpr NnagaScalePointV1 kOnOff[] = {{sizeof(NnagaScalePointV1), 0.0f, "Off"}, {sizeof(NnagaScalePointV1), 1.0f, "On"}};
constexpr NnagaScalePointV1 kGrid[] = {{sizeof(NnagaScalePointV1), 0.0f, "1/4"}, {sizeof(NnagaScalePointV1), 0.5f, "1/8"}, {sizeof(NnagaScalePointV1), 1.0f, "1/16"}};
constexpr NnagaParameterV1 kParameters[] = {
    {sizeof(NnagaParameterV1), kEnabledPort, "Enabled", "enabled", "", NNAGA_PARAMETER_TOGGLE, 1.0f, 2, kOnOff},
    {sizeof(NnagaParameterV1), kGridPort, "Grid", "grid", "", NNAGA_PARAMETER_ENUM, 0.5f, 3, kGrid},
    {sizeof(NnagaParameterV1), kAmountPort, "Shuffle", "shuffle", "%", 0, 1.0f, 0, nullptr},
    {sizeof(NnagaParameterV1), kSeedPort, "Seed", "seed", "", 0, 0.0f, 0, nullptr},
    {sizeof(NnagaParameterV1), kMixPort, "Mix", "mix", "%", 0, 1.0f, 0, nullptr},
};
constexpr NnagaPluginDescriptorV1 kDescriptor = {sizeof(NnagaPluginDescriptorV1), "com.vibes.dsp.shuffle", "NNAGA Shuffle", "NNAGA", "1.0.0", 2, 2, 5, kParameters, create, destroy, activate, deactivate, reset, set_parameter, format_parameter, nullptr, process};
const NnagaPluginDescriptorV1* get_plugin(uint32_t index) noexcept { return index == 0 ? &kDescriptor : nullptr; }
constexpr NnagaPluginLibraryV1 kLibrary = {sizeof(NnagaPluginLibraryV1), NNAGA_NATIVE_ABI_VERSION, 1, get_plugin};
} // namespace
extern "C" NNAGA_NATIVE_EXPORT const NnagaPluginLibraryV1* nnaga_plugin_entry(uint32_t version) noexcept { return version == NNAGA_NATIVE_ABI_VERSION ? &kLibrary : nullptr; }

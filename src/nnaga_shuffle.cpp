#include "nnaga/native_plugin.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
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
constexpr uint32_t kMaxSlices = 512;
constexpr uint32_t kRingSeconds = 64;
constexpr uint32_t kCrossfadeFrames = 32;
constexpr uint32_t kSixteenthsPerBar = 16;
constexpr uint32_t kGridChoices = 6;
constexpr uint32_t kGridSteps = 5;
constexpr uint32_t kGridUnitsPerQuarter = 192;

struct Slice {
    uint64_t destinationStart = 0;
    uint64_t destinationEnd = 0;
    uint64_t sourceStart = 0;
};

struct Shuffle {
    float* ringLeft = nullptr;
    float* ringRight = nullptr;
    uint32_t* ringGeneration = nullptr;
    uint32_t capacity = 0;
    uint32_t write = 0;
    uint32_t generation = 1;
    uint64_t cycleSamples = 0;
    uint64_t cycleStartWrite = 0;
    uint64_t expectedTransportFrame = 0;
    uint64_t previousPhase = 0;
    uint64_t activeLoopFrames = 0;
    uint32_t sourceRng = 1;
    uint32_t gridRng = 1;
    Slice slices[kMaxSlices]{};
    uint32_t sliceCount = 0;
    uint32_t sliceIndex = 0;
    bool phaseValid = false;
    bool previousCycleReady = false;
    float enabled = 1.0f;
    float amount = 1.0f;
    float seed = 0.0f;
    float mix = 1.0f;
    float bars = 0.0f;
    float startPosition = 0.0f;
    float endPosition = 1.0f;
    float gridFrom = 0.2f;
    float gridTo = 0.6f;
    float gridMode = 0.0f;
    float gridSeed = 0.0f;
    float previousLeft = 0.0f;
    float previousRight = 0.0f;
    uint32_t fadeRemaining = 0;
    double sampleRate = 48000.0;
};

float clamp01(float value) noexcept {
    return std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0.0f;
}

uint32_t nextRandom(uint32_t& state) noexcept {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    if (state == 0) state = 1;
    return state;
}

uint32_t barCount(float normalized) noexcept {
    return 1u + static_cast<uint32_t>(std::lround(clamp01(normalized) * 7.0f));
}

uint32_t positionStep(const Shuffle* shuffle, float normalized) noexcept {
    const uint32_t total = barCount(shuffle->bars) * kSixteenthsPerBar;
    return std::min(total, static_cast<uint32_t>(std::lround(clamp01(normalized) * total)));
}

bool framesPerBar(const NnagaProcessContextV1* context, uint64_t& frames) noexcept {
    if (!context || !std::isfinite(context->sample_rate) ||
        !std::isfinite(context->beats_per_minute) || !std::isfinite(context->beats_per_bar) ||
        context->sample_rate <= 0.0 || context->beats_per_minute <= 0.0 ||
        context->beats_per_bar <= 0.0f || context->beat_unit <= 0) return false;
    const double value = context->sample_rate * 60.0 / context->beats_per_minute *
        static_cast<double>(context->beats_per_bar) * 4.0 / static_cast<double>(context->beat_unit);
    if (!std::isfinite(value) || value < 1.0 || value > static_cast<double>(UINT64_MAX)) return false;
    frames = static_cast<uint64_t>(std::llround(value));
    return frames != 0;
}

uint32_t gridIndex(float normalized) noexcept {
    return std::clamp(static_cast<uint32_t>(std::lround(clamp01(normalized) * (kGridChoices - 1))), 0u, kGridChoices - 1);
}

uint32_t gridLengthUnits(uint32_t index) noexcept {
    constexpr uint32_t lengths[kGridChoices] = {6, 12, 24, 48, 96, 192};
    return lengths[index];
}

uint32_t chooseGridUnits(Shuffle* shuffle, uint32_t low, uint32_t high) noexcept {
    const uint32_t baseUnits = gridLengthUnits(low + nextRandom(shuffle->gridRng) % (high - low + 1));
    const uint32_t mode = std::clamp(static_cast<uint32_t>(std::lround(clamp01(shuffle->gridMode) * 5.0f)), 0u, 5u);
    if (mode == 0) return baseUnits;
    if (mode == 3) return baseUnits * 3 / 2;
    if (mode == 4) return std::max(1u, baseUnits * 2 / 3);
    const uint32_t variant = nextRandom(shuffle->gridRng) % (mode == 5 ? 3u : 2u);
    if (variant == 0) return baseUnits;
    if (mode == 1) return baseUnits * 3 / 2;
    if (mode == 2) return std::max(1u, baseUnits * 2 / 3);
    return variant == 1 ? baseUnits * 3 / 2 : std::max(1u, baseUnits * 2 / 3);
}

void resetCapture(Shuffle* shuffle) noexcept {
    shuffle->write = 0;
    shuffle->cycleSamples = 0;
    shuffle->cycleStartWrite = 0;
    shuffle->expectedTransportFrame = 0;
    shuffle->previousPhase = 0;
    shuffle->activeLoopFrames = 0;
    shuffle->sliceCount = 0;
    shuffle->sliceIndex = 0;
    shuffle->phaseValid = false;
    shuffle->previousCycleReady = false;
    shuffle->previousLeft = 0.0f;
    shuffle->previousRight = 0.0f;
    shuffle->fadeRemaining = 0;
    if (++shuffle->generation == 0) shuffle->generation = 1;
}

void makeGrid(Shuffle* shuffle, uint64_t loopFrames, uint64_t cycleNumber) noexcept {
    const uint32_t totalUnits = barCount(shuffle->bars) * kGridUnitsPerQuarter;
    const uint32_t startUnits = positionStep(shuffle, shuffle->startPosition) * (kGridUnitsPerQuarter / kSixteenthsPerBar);
    const uint32_t endUnits = positionStep(shuffle, shuffle->endPosition) * (kGridUnitsPerQuarter / kSixteenthsPerBar);
    shuffle->sliceCount = 0;
    shuffle->sliceIndex = 0;
    if (startUnits >= endUnits) return;

    shuffle->gridRng = static_cast<uint32_t>(shuffle->gridSeed * 4294967294.0f) ^
        static_cast<uint32_t>(cycleNumber) ^ static_cast<uint32_t>(cycleNumber >> 32);
    shuffle->sourceRng = static_cast<uint32_t>(shuffle->seed * 4294967294.0f) ^
        static_cast<uint32_t>(cycleNumber * 0x9e3779b9u);
    if (!shuffle->gridRng) shuffle->gridRng = 1;
    if (!shuffle->sourceRng) shuffle->sourceRng = 1;

    uint32_t low = gridIndex(shuffle->gridFrom);
    uint32_t high = gridIndex(shuffle->gridTo);
    if (low > high) std::swap(low, high);
    uint64_t cursor = startUnits;
    const uint64_t minimumUnits = gridLengthUnits(0);
    while (cursor < endUnits && shuffle->sliceCount < kMaxSlices) {
        const uint32_t selectedUnits = chooseGridUnits(shuffle, low, high);
        const uint64_t remaining = endUnits - cursor;
        const uint64_t destinationUnits = std::min<uint64_t>(remaining, std::max<uint64_t>(minimumUnits, selectedUnits));
        const uint64_t destinationStart = cursor * loopFrames / totalUnits;
        const uint64_t destinationEnd = std::min<uint64_t>(loopFrames, (cursor + destinationUnits) * loopFrames / totalUnits);
        if (destinationEnd <= destinationStart) break;

        Slice& slice = shuffle->slices[shuffle->sliceCount++];
        slice.destinationStart = destinationStart;
        slice.destinationEnd = destinationEnd;
        const uint64_t length = destinationEnd - destinationStart;
        const bool randomize = shuffle->amount > 0.0f &&
            (shuffle->amount >= 1.0f || (nextRandom(shuffle->sourceRng) / static_cast<float>(UINT32_MAX)) <= shuffle->amount);
        slice.sourceStart = randomize && length < loopFrames
            ? nextRandom(shuffle->sourceRng) % (loopFrames - length + 1)
            : destinationStart;
        cursor += destinationUnits;
    }
}

bool rangeActive(const Shuffle* shuffle, uint64_t phase, uint64_t loopFrames) noexcept {
    if (!loopFrames) return false;
    const uint32_t start = positionStep(shuffle, shuffle->startPosition);
    const uint32_t end = positionStep(shuffle, shuffle->endPosition);
    if (start >= end) return false;
    const uint32_t total = barCount(shuffle->bars) * kSixteenthsPerBar;
    const uint32_t position = static_cast<uint32_t>((phase % loopFrames) * total / loopFrames);
    return position >= start && position < end;
}

NnagaPluginHandle create() noexcept {
    auto* shuffle = static_cast<Shuffle*>(std::calloc(1, sizeof(Shuffle)));
    if (!shuffle) return nullptr;
    shuffle->enabled = 1.0f;
    shuffle->amount = 1.0f;
    shuffle->mix = 1.0f;
    shuffle->endPosition = 1.0f;
    shuffle->gridFrom = 0.2f;
    shuffle->gridTo = 0.6f;
    return shuffle;
}

void destroy(NnagaPluginHandle handle) noexcept {
    auto* shuffle = static_cast<Shuffle*>(handle);
    if (!shuffle) return;
    std::free(shuffle->ringLeft);
    std::free(shuffle->ringRight);
    std::free(shuffle->ringGeneration);
    std::free(shuffle);
}

int32_t activate(NnagaPluginHandle handle, double sampleRate, uint32_t) noexcept {
    auto* shuffle = static_cast<Shuffle*>(handle);
    if (!shuffle || !std::isfinite(sampleRate) || sampleRate < 1000.0) return 0;
    const uint32_t capacity = static_cast<uint32_t>(sampleRate * kRingSeconds);
    float* left = static_cast<float*>(std::calloc(capacity, sizeof(float)));
    float* right = static_cast<float*>(std::calloc(capacity, sizeof(float)));
    uint32_t* generation = static_cast<uint32_t*>(std::calloc(capacity, sizeof(uint32_t)));
    if (!left || !right || !generation) {
        std::free(left); std::free(right); std::free(generation);
        return 0;
    }
    std::free(shuffle->ringLeft);
    std::free(shuffle->ringRight);
    std::free(shuffle->ringGeneration);
    shuffle->ringLeft = left;
    shuffle->ringRight = right;
    shuffle->ringGeneration = generation;
    shuffle->capacity = capacity;
    shuffle->sampleRate = sampleRate;
    resetCapture(shuffle);
    return 1;
}

void deactivate(NnagaPluginHandle) noexcept {}

void reset(NnagaPluginHandle handle) noexcept {
    auto* shuffle = static_cast<Shuffle*>(handle);
    if (!shuffle || !shuffle->ringLeft) return;
    std::memset(shuffle->ringLeft, 0, shuffle->capacity * sizeof(float));
    std::memset(shuffle->ringRight, 0, shuffle->capacity * sizeof(float));
    std::memset(shuffle->ringGeneration, 0, shuffle->capacity * sizeof(uint32_t));
    resetCapture(shuffle);
}

void setParameter(NnagaPluginHandle handle, uint32_t port, float value) noexcept {
    auto* shuffle = static_cast<Shuffle*>(handle);
    if (!shuffle) return;
    value = clamp01(value);
    switch (port) {
        case kEnabledPort: shuffle->enabled = value; break;
        case kAmountPort: shuffle->amount = value; break;
        case kSeedPort: shuffle->seed = value; break;
        case kMixPort: shuffle->mix = value; break;
        case kBarsPort: shuffle->bars = value; break;
        case kStartPort: shuffle->startPosition = value; break;
        case kEndPort: shuffle->endPosition = value; break;
        case kGridFromPort: shuffle->gridFrom = value; break;
        case kGridToPort: shuffle->gridTo = value; break;
        case kGridModePort: shuffle->gridMode = value; break;
        case kGridSeedPort: shuffle->gridSeed = value; break;
        default: break;
    }
}

void formatPosition(const Shuffle* shuffle, float value, char* output, uint32_t capacity) noexcept {
    if (!shuffle || !output || !capacity) return;
    const uint32_t sixteenth = positionStep(shuffle, value);
    std::snprintf(output, capacity, "%u:%u:%u", sixteenth / 16 + 1, sixteenth % 16 / 4 + 1, sixteenth % 4 + 1);
}

uint32_t formatParameter(NnagaPluginHandle handle, uint32_t port, float value, char* output, uint32_t capacity) noexcept {
    if (!output || !capacity) return 0;
    const auto* shuffle = static_cast<const Shuffle*>(handle);
    if (port == kStartPort || port == kEndPort) formatPosition(shuffle, value, output, capacity);
    else if (port == kGridFromPort || port == kGridToPort) {
        constexpr const char* labels[kGridChoices] = {"1/32", "1/16", "1/8", "1/4", "1/2", "1 bar"};
        std::snprintf(output, capacity, "%s", labels[gridIndex(value)]);
    } else if (port == kGridModePort) {
        constexpr const char* labels[6] = {
            "No dotted or triplets", "Allow dotted", "Allow triplets",
            "Use dotted", "Use triplets", "Allow dotted and triplets",
        };
        std::snprintf(output, capacity, "%s",
                      labels[std::clamp(static_cast<uint32_t>(std::lround(clamp01(value) * 5.0f)), 0u, 5u)]);
    } else if (port == kBarsPort) {
        const uint32_t bars = barCount(value);
        std::snprintf(output, capacity, "%u %s", bars, bars == 1 ? "bar" : "bars");
    } else if (port == kAmountPort || port == kMixPort) {
        std::snprintf(output, capacity, "%.0f%%", clamp01(value) * 100.0f);
    } else {
        output[0] = '\0';
    }
    return static_cast<uint32_t>(std::strlen(output));
}

void process(NnagaPluginHandle handle, const float* inputLeft, const float* inputRight,
             float* outputLeft, float* outputRight, uint32_t frames,
             const NnagaProcessContextV1* context) noexcept {
    auto* shuffle = static_cast<Shuffle*>(handle);
    if (!shuffle || !shuffle->ringLeft || !shuffle->ringGeneration || !inputLeft || !inputRight ||
        !outputLeft || !outputRight || !context) return;
    uint64_t barFrames = 0;
    const bool validTiming = framesPerBar(context, barFrames);
    const uint32_t bars = barCount(shuffle->bars);
    const uint64_t loopFrames = validTiming && barFrames <= UINT64_MAX / bars ? barFrames * bars : 0;
    const uint32_t totalSteps = bars * 16;
    auto dry = [&] {
        for (uint32_t i = 0; i < frames; ++i) { outputLeft[i] = inputLeft[i]; outputRight[i] = inputRight[i]; }
    };
    if (!context->playing || !loopFrames || loopFrames > shuffle->capacity || totalSteps == 0 ||
        shuffle->enabled < 0.5f) {
        resetCapture(shuffle);
        dry();
        return;
    }
    if (!shuffle->phaseValid || shuffle->expectedTransportFrame != context->transport_frame ||
        shuffle->activeLoopFrames != loopFrames) {
        resetCapture(shuffle);
        shuffle->activeLoopFrames = loopFrames;
    }

    for (uint32_t i = 0; i < frames; ++i) {
        const uint64_t phase = (context->transport_frame + i) % loopFrames;
        if (!shuffle->phaseValid) {
            shuffle->phaseValid = true;
            shuffle->cycleStartWrite = shuffle->write;
            shuffle->cycleSamples = 0;
            makeGrid(shuffle, loopFrames, (context->transport_frame + i) / loopFrames);
        } else if (phase < shuffle->previousPhase) {
            shuffle->previousCycleReady = shuffle->cycleSamples >= loopFrames;
            shuffle->cycleStartWrite = shuffle->write;
            shuffle->cycleSamples = 0;
            shuffle->sliceIndex = 0;
            makeGrid(shuffle, loopFrames, (context->transport_frame + i) / loopFrames);
        }
        shuffle->previousPhase = phase;
        const bool inRange = rangeActive(shuffle, phase, loopFrames);
        while (shuffle->sliceIndex + 1 < shuffle->sliceCount &&
               phase >= shuffle->slices[shuffle->sliceIndex].destinationEnd)
            ++shuffle->sliceIndex;

        shuffle->ringLeft[shuffle->write] = inputLeft[i];
        shuffle->ringRight[shuffle->write] = inputRight[i];
        shuffle->ringGeneration[shuffle->write] = shuffle->generation;
        float wetLeft = inputLeft[i];
        float wetRight = inputRight[i];
        if (inRange && shuffle->previousCycleReady && shuffle->sliceIndex < shuffle->sliceCount) {
            const Slice& slice = shuffle->slices[shuffle->sliceIndex];
            if (phase >= slice.destinationStart && phase < slice.destinationEnd) {
                const uint64_t sourceOffset = phase - slice.destinationStart;
                const uint64_t sourceFrame = slice.sourceStart + sourceOffset;
                const uint32_t read = static_cast<uint32_t>(
                    (shuffle->cycleStartWrite + shuffle->capacity - loopFrames + sourceFrame) % shuffle->capacity);
                if (shuffle->ringGeneration[read] == shuffle->generation) {
                    wetLeft = shuffle->ringLeft[read];
                    wetRight = shuffle->ringRight[read];
                    if (shuffle->fadeRemaining) {
                        const float mix = 1.0f - static_cast<float>(shuffle->fadeRemaining) / kCrossfadeFrames;
                        wetLeft = shuffle->previousLeft + (wetLeft - shuffle->previousLeft) * mix;
                        wetRight = shuffle->previousRight + (wetRight - shuffle->previousRight) * mix;
                        --shuffle->fadeRemaining;
                    }
                }
            }
        } else {
            shuffle->fadeRemaining = 0;
        }
        outputLeft[i] = inRange ? inputLeft[i] + (wetLeft - inputLeft[i]) * shuffle->mix : inputLeft[i];
        outputRight[i] = inRange ? inputRight[i] + (wetRight - inputRight[i]) * shuffle->mix : inputRight[i];
        shuffle->previousLeft = outputLeft[i];
        shuffle->previousRight = outputRight[i];
        if (shuffle->sliceIndex + 1 < shuffle->sliceCount &&
            phase + 1 >= shuffle->slices[shuffle->sliceIndex].destinationEnd && inRange)
            shuffle->fadeRemaining = kCrossfadeFrames;
        shuffle->write = (shuffle->write + 1) % shuffle->capacity;
        ++shuffle->cycleSamples;
    }
    shuffle->expectedTransportFrame = context->transport_frame + frames;
}

constexpr NnagaScalePointV1 kEnabledPoints[] = {
    {sizeof(NnagaScalePointV1), 0.0f, "Off"}, {sizeof(NnagaScalePointV1), 1.0f, "On"},
};
constexpr NnagaScalePointV1 kGridLengths[] = {
    {sizeof(NnagaScalePointV1), 0.0f, "1/32"}, {sizeof(NnagaScalePointV1), 0.2f, "1/16"},
    {sizeof(NnagaScalePointV1), 0.4f, "1/8"}, {sizeof(NnagaScalePointV1), 0.6f, "1/4"},
    {sizeof(NnagaScalePointV1), 0.8f, "1/2"}, {sizeof(NnagaScalePointV1), 1.0f, "1 bar"},
};
constexpr NnagaScalePointV1 kGridModes[] = {
    {sizeof(NnagaScalePointV1), 0.0f, "No dotted or triplets"},
    {sizeof(NnagaScalePointV1), 0.2f, "Allow dotted"},
    {sizeof(NnagaScalePointV1), 0.4f, "Allow triplets"},
    {sizeof(NnagaScalePointV1), 0.6f, "Use dotted"},
    {sizeof(NnagaScalePointV1), 0.8f, "Use triplets"},
    {sizeof(NnagaScalePointV1), 1.0f, "Allow dotted and triplets"},
};
constexpr NnagaParameterV1 kParameters[] = {
    {sizeof(NnagaParameterV1), kEnabledPort, "Enabled", "enabled", "", NNAGA_PARAMETER_TOGGLE, 1.0f, 2, kEnabledPoints, 0},
    {sizeof(NnagaParameterV1), kAmountPort, "Shuffle", "shuffle", "%", 0, 1.0f, 0, nullptr, 0},
    {sizeof(NnagaParameterV1), kSeedPort, "Seed", "seed", "", 0, 0.0f, 0, nullptr, 0},
    {sizeof(NnagaParameterV1), kMixPort, "Mix", "mix", "%", 0, 1.0f, 0, nullptr, 0},
    {sizeof(NnagaParameterV1), kBarsPort, "Bars", "bars", "", 0, 0.0f, 0, nullptr, 7},
    {sizeof(NnagaParameterV1), kStartPort, "Shuffle start position", "shuffle_start_position", "", 0, 0.0f, 0, nullptr, 128},
    {sizeof(NnagaParameterV1), kEndPort, "Shuffle end position", "shuffle_end_position", "", 0, 1.0f, 0, nullptr, 128},
    {sizeof(NnagaParameterV1), kGridFromPort, "Grid from", "grid_from", "", 0, 0.2f, 0, nullptr, 5},
    {sizeof(NnagaParameterV1), kGridToPort, "Grid to", "grid_to", "", 0, 0.6f, 0, nullptr, 5},
    {sizeof(NnagaParameterV1), kGridModePort, "Grid mode", "grid_mode", "", NNAGA_PARAMETER_ENUM, 0.0f, 5, kGridModes, 0},
    {sizeof(NnagaParameterV1), kGridSeedPort, "Grid seed", "grid_seed", "", 0, 0.0f, 0, nullptr, 0},
};
constexpr NnagaPluginDescriptorV1 kDescriptor = {
    sizeof(NnagaPluginDescriptorV1), "com.vibes.dsp.shuffle", "NNAGA Shuffle", "NNAGA", "1.1.1",
    2, 2, 11, kParameters, create, destroy, activate, deactivate, reset, setParameter,
    formatParameter, nullptr, process,
};
const NnagaPluginDescriptorV1* getPlugin(uint32_t index) noexcept {
    return index == 0 ? &kDescriptor : nullptr;
}
constexpr NnagaPluginLibraryV1 kLibrary = {
    sizeof(NnagaPluginLibraryV1), NNAGA_NATIVE_ABI_VERSION, 1, getPlugin,
};
} // namespace

extern "C" NNAGA_NATIVE_EXPORT const NnagaPluginLibraryV1*
nnaga_plugin_entry(uint32_t hostAbiVersion) noexcept {
    return hostAbiVersion == NNAGA_NATIVE_ABI_VERSION ? &kLibrary : nullptr;
}

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
constexpr uint32_t kGridPort = 11;
constexpr uint32_t kFillFromPort = 12;
constexpr uint32_t kFillToPort = 13;
constexpr uint32_t kGridModePort = 14;
constexpr uint32_t kTripletModePort = 16;
constexpr uint32_t kGridSeedPort = 15;
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
    float grid = 0.2f;
    float fillFrom = 0.2f;
    float fillTo = 0.6f;
    float gridMode = 0.0f;
    float tripletMode = 0.0f;
    float gridSeed = 0.0f;
    uint32_t fadeRemaining = 0;
    double sampleRate = 48000.0;
    uint32_t maxFrames = 0;
    float previousLeft = 0.0f;
    float previousRight = 0.0f;
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

bool framesPerBar(const NnagaProcessContextV2* context, uint64_t& frames) noexcept {
    if (!context || !std::isfinite(context->sample_rate) ||
        !std::isfinite(context->beats_per_minute) || !std::isfinite(context->beats_per_bar) ||
        context->sample_rate <= 0.0 || context->beats_per_minute <= 0.0 ||
        context->beats_per_bar <= 0.0f || context->beat_unit <= 0) return false;
    const double value = context->sample_rate * 60.0 / context->beats_per_minute *
        static_cast<double>(context->beats_per_bar) * 4.0 / static_cast<double>(context->beat_unit);
    if (!std::isfinite(value) || value < 1.0 || value > static_cast<double>(INT64_MAX)) return false;
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

enum class GridVariant : uint32_t { Straight, Dotted, Triplet };

GridVariant chooseGridVariant(Shuffle* shuffle) noexcept {
    const uint32_t mode = std::clamp(
        static_cast<uint32_t>(std::lround(clamp01(shuffle->gridMode) * 5.0f)), 0u, 5u);
    switch (mode) {
        case 0: return GridVariant::Straight;
        case 3: return GridVariant::Dotted;
        case 4: return GridVariant::Triplet;
        case 1: return (nextRandom(shuffle->gridRng) & 1) ? GridVariant::Dotted : GridVariant::Straight;
        case 2: return (nextRandom(shuffle->gridRng) & 1) ? GridVariant::Triplet : GridVariant::Straight;
        default: {
            const uint32_t choice = nextRandom(shuffle->gridRng) % 3;
            return choice == 0 ? GridVariant::Straight
                               : (choice == 1 ? GridVariant::Dotted : GridVariant::Triplet);
        }
    }
}

uint32_t makeMotif(GridVariant variant, uint32_t baseUnits, uint32_t* lengths) noexcept {
    if (variant == GridVariant::Dotted) {
        lengths[0] = baseUnits * 3 / 2;
        lengths[1] = baseUnits / 2;
        return 2;
    }
    if (variant == GridVariant::Triplet) {
        lengths[0] = lengths[1] = lengths[2] = std::max(1u, baseUnits / 3);
        return 3;
    }
    lengths[0] = baseUnits;
    return 1;
}

uint64_t chooseSourceStart(Shuffle* shuffle, uint64_t loopFrames, uint64_t segmentLength,
                           uint64_t gridFrames, uint64_t destinationStart) noexcept {
    const bool randomize = shuffle->amount > 0.0f &&
        (shuffle->amount >= 1.0f ||
         nextRandom(shuffle->sourceRng) / static_cast<float>(UINT32_MAX) <= shuffle->amount);
    if (!randomize || segmentLength >= loopFrames) return destinationStart;
    const uint64_t maxStart = loopFrames - segmentLength;
    return gridFrames * (nextRandom(shuffle->sourceRng) % (maxStart / gridFrames + 1));
}

bool appendMotif(Shuffle* shuffle, uint64_t loopFrames, uint32_t totalUnits,
                 uint32_t gridUnits, uint64_t destinationUnits, const uint32_t* lengths,
                 uint32_t count, uint32_t groupUnits, GridVariant variant) noexcept {
    if (shuffle->sliceCount + count > kMaxSlices) return false;
    const uint64_t destinationStart = destinationUnits * loopFrames / totalUnits;
    const uint64_t destinationEnd = (destinationUnits + groupUnits) * loopFrames / totalUnits;
    if (destinationEnd <= destinationStart) return false;
    const uint64_t groupLength = destinationEnd - destinationStart;
    const uint64_t gridFrames = std::max<uint64_t>(1, gridUnits * loopFrames / totalUnits);
    const uint64_t groupSourceStart =
        chooseSourceStart(shuffle, loopFrames, groupLength, gridFrames, destinationStart);
    uint64_t offsetUnits = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint64_t segmentStart = (destinationUnits + offsetUnits) * loopFrames / totalUnits;
        const uint64_t segmentEnd =
            (destinationUnits + offsetUnits + lengths[i]) * loopFrames / totalUnits;
        if (segmentEnd <= segmentStart) return false;
        const uint64_t segmentLength = segmentEnd - segmentStart;
        const bool separateTripletSegments =
            variant == GridVariant::Triplet && shuffle->tripletMode >= 0.5f;
        const uint64_t sourceStart = separateTripletSegments
            ? chooseSourceStart(shuffle, loopFrames, segmentLength, gridFrames, segmentStart)
            : groupSourceStart;
        Slice& slice = shuffle->slices[shuffle->sliceCount++];
        slice.destinationStart = segmentStart;
        slice.destinationEnd = segmentEnd;
        const bool retriggerSameSegment =
            variant == GridVariant::Triplet && shuffle->tripletMode < 0.5f;
        slice.sourceStart = separateTripletSegments || retriggerSameSegment
            ? sourceStart
            : sourceStart + (segmentStart - destinationStart);
        offsetUnits += lengths[i];
    }
    return true;
}

void makeGrid(Shuffle* shuffle, uint64_t loopFrames, uint64_t cycleNumber) noexcept {
    const uint32_t totalUnits = barCount(shuffle->bars) * kGridUnitsPerQuarter;
    const uint32_t startUnits = positionStep(shuffle, shuffle->startPosition) *
        (kGridUnitsPerQuarter / kSixteenthsPerBar);
    const uint32_t endUnits = positionStep(shuffle, shuffle->endPosition) *
        (kGridUnitsPerQuarter / kSixteenthsPerBar);
    shuffle->sliceCount = 0;
    shuffle->sliceIndex = 0;
    if (startUnits >= endUnits) return;

    shuffle->gridRng = static_cast<uint32_t>(shuffle->gridSeed * 4294967294.0f) ^
        static_cast<uint32_t>(cycleNumber) ^ static_cast<uint32_t>(cycleNumber >> 32);
    shuffle->sourceRng = static_cast<uint32_t>(shuffle->seed * 4294967294.0f) ^
        static_cast<uint32_t>(cycleNumber * 0x9e3779b9u);
    if (!shuffle->gridRng) shuffle->gridRng = 1;
    if (!shuffle->sourceRng) shuffle->sourceRng = 1;

    uint32_t lowFill = gridIndex(shuffle->fillFrom);
    uint32_t highFill = gridIndex(shuffle->fillTo);
    if (lowFill > highFill) std::swap(lowFill, highFill);
    const uint32_t gridUnits = gridLengthUnits(gridIndex(shuffle->grid));
    uint64_t cursor = startUnits;
    while (cursor < endUnits) {
        const GridVariant variant = chooseGridVariant(shuffle);
        const uint32_t fillIndex = lowFill +
            nextRandom(shuffle->gridRng) % (highFill - lowFill + 1);
        const uint32_t baseUnits = gridLengthUnits(fillIndex);
        uint32_t lengths[3] = {};
        uint32_t count = makeMotif(variant, baseUnits, lengths);
        uint32_t groupUnits = 0;
        for (uint32_t i = 0; i < count; ++i) groupUnits += lengths[i];
        const uint32_t remaining = endUnits - static_cast<uint32_t>(cursor);
        if (groupUnits > remaining) {
            count = 1;
            lengths[0] = remaining;
            groupUnits = remaining;
        }
        if (!appendMotif(shuffle, loopFrames, totalUnits, gridUnits, cursor, lengths, count,
                         groupUnits, variant))
            break;
        cursor += groupUnits;
    }
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
    shuffle->fillFrom = 0.2f;
    shuffle->fillTo = 0.6f;
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

int32_t activate(NnagaPluginHandle handle, double sampleRate, uint32_t maxFrames) noexcept {
    auto* shuffle = static_cast<Shuffle*>(handle);
    if (!shuffle || !std::isfinite(sampleRate) || sampleRate < 1000.0 ||
        maxFrames == 0 || maxFrames > NNAGA_NATIVE_MAX_FRAMES) return 0;
    const double capacityValue = sampleRate * static_cast<double>(kRingSeconds);
    if (!std::isfinite(capacityValue) || capacityValue < 1.0 ||
        capacityValue > static_cast<double>(UINT32_MAX) ||
        capacityValue > static_cast<double>(SIZE_MAX / sizeof(float))) return 0;
    const uint32_t capacity = static_cast<uint32_t>(capacityValue);
    if (capacity == 0) return 0;
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
    shuffle->maxFrames = maxFrames;
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
        case kGridPort: shuffle->grid = value; break;
        case kFillFromPort: shuffle->fillFrom = value; break;
        case kFillToPort: shuffle->fillTo = value; break;
        case kGridModePort: shuffle->gridMode = value; break;
        case kGridSeedPort: shuffle->gridSeed = value; break;
        case kTripletModePort: shuffle->tripletMode = value; break;
        default: break;
    }
}

void formatPosition(float value, char* output, uint32_t capacity) noexcept {
    if (!output || !capacity) return;
    const uint32_t sixteenth = static_cast<uint32_t>(std::lround(clamp01(value) * kSixteenthsPerBar));
    std::snprintf(output, capacity, "%u:%u:%u", sixteenth / 16 + 1, sixteenth % 16 / 4 + 1, sixteenth % 4 + 1);
}
uint32_t formatParameter(NnagaPluginHandle, uint32_t port, float value, char* output, uint32_t capacity) noexcept {
    if (!output || !capacity) return 0;
    if (port == kStartPort || port == kEndPort) formatPosition(value, output, capacity);
    else if (port == kGridPort || port == kFillFromPort || port == kFillToPort) {
        constexpr const char* labels[kGridChoices] = {"1/32", "1/16", "1/8", "1/4", "1/2", "1 bar"};
        std::snprintf(output, capacity, "%s", labels[gridIndex(value)]);
    } else if (port == kGridModePort) {
        constexpr const char* labels[6] = {
            "No dotted or triplets", "Allow dotted", "Allow triplets",
            "Use dotted", "Use triplets", "Allow dotted and triplets",
        };
        std::snprintf(output, capacity, "%s",
                      labels[std::clamp(static_cast<uint32_t>(std::lround(clamp01(value) * 5.0f)), 0u, 5u)]);
    } else if (port == kTripletModePort) {
        constexpr const char* labels[2] = {"Retrigger same segment", "Use separate segments"};
        std::snprintf(output, capacity, "%s", labels[clamp01(value) >= 0.5f ? 1 : 0]);
    } else if (port == kGridSeedPort) {
        std::snprintf(output, capacity, "%u", static_cast<uint32_t>(clamp01(value) * 65535.0f));
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
             const NnagaProcessContextV2* context) noexcept {
    auto* shuffle = static_cast<Shuffle*>(handle);
    if (!shuffle || !shuffle->ringLeft || !shuffle->ringGeneration || !inputLeft || !inputRight ||
        !outputLeft || !outputRight || !context || frames == 0 || frames > shuffle->maxFrames) return;
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

constexpr NnagaScalePointV2 kEnabledPoints[] = {
    {sizeof(NnagaScalePointV2), 0.0f, "Off"}, {sizeof(NnagaScalePointV2), 1.0f, "On"},
};
constexpr NnagaScalePointV2 kGridLengths[] = {
    {sizeof(NnagaScalePointV2), 0.0f, "1/32"}, {sizeof(NnagaScalePointV2), 0.2f, "1/16"},
    {sizeof(NnagaScalePointV2), 0.4f, "1/8"}, {sizeof(NnagaScalePointV2), 0.6f, "1/4"},
    {sizeof(NnagaScalePointV2), 0.8f, "1/2"}, {sizeof(NnagaScalePointV2), 1.0f, "1 bar"},
};
constexpr NnagaScalePointV2 kGridModes[] = {
    {sizeof(NnagaScalePointV2), 0.0f, "No dotted or triplets"},
    {sizeof(NnagaScalePointV2), 0.2f, "Allow dotted"},
    {sizeof(NnagaScalePointV2), 0.4f, "Allow triplets"},
    {sizeof(NnagaScalePointV2), 0.6f, "Use dotted"},
    {sizeof(NnagaScalePointV2), 0.8f, "Use triplets"},
    {sizeof(NnagaScalePointV2), 1.0f, "Allow dotted and triplets"},
};
constexpr NnagaScalePointV2 kTripletModes[] = {
    {sizeof(NnagaScalePointV2), 0.0f, "Retrigger same segment"},
    {sizeof(NnagaScalePointV2), 1.0f, "Use separate segments"},
};
constexpr NnagaParameterV2 kParameters[] = {
    {sizeof(NnagaParameterV2), kEnabledPort, "Enabled", "enabled", "", NNAGA_PARAMETER_TOGGLE, 1.0f, 2, kEnabledPoints, 0},
    {sizeof(NnagaParameterV2), kAmountPort, "Shuffle", "shuffle", "%", 0, 1.0f, 0, nullptr, 0},
    {sizeof(NnagaParameterV2), kSeedPort, "Seed", "seed", "", 0, 0.0f, 0, nullptr, 0},
    {sizeof(NnagaParameterV2), kMixPort, "Mix", "mix", "%", 0, 1.0f, 0, nullptr, 0},
    {sizeof(NnagaParameterV2), kBarsPort, "Bars", "bars", "", 0, 0.0f, 0, nullptr, 7},
    {sizeof(NnagaParameterV2), kStartPort, "Shuffle start position", "shuffle_start_position", "", 0, 0.0f, 0, nullptr, 128},
    {sizeof(NnagaParameterV2), kEndPort, "Shuffle end position", "shuffle_end_position", "", 0, 1.0f, 0, nullptr, 128},
    {sizeof(NnagaParameterV2), kGridPort, "Grid", "grid", "", NNAGA_PARAMETER_ENUM, 0.2f, 6, kGridLengths, 0},
    {sizeof(NnagaParameterV2), kFillFromPort, "Fill length from", "fill_length_from", "", 0, 0.2f, 0, nullptr, 5},
    {sizeof(NnagaParameterV2), kFillToPort, "Fill length to", "fill_length_to", "", 0, 0.6f, 0, nullptr, 5},
    {sizeof(NnagaParameterV2), kGridModePort, "Grid mode", "grid_mode", "", NNAGA_PARAMETER_ENUM, 0.0f, 6, kGridModes, 0},
    {sizeof(NnagaParameterV2), kGridSeedPort, "Grid seed", "grid_seed", "", 0, 0.0f, 0, nullptr, 0},
    {sizeof(NnagaParameterV2), kTripletModePort, "Triplet mode", "triplet_mode", "", NNAGA_PARAMETER_ENUM, 0.0f, 2, kTripletModes, 0},
};
constexpr NnagaPluginDescriptorV2 kDescriptor = {
    sizeof(NnagaPluginDescriptorV2), "com.vibes.dsp.shuffle", "shuffle", "NNAGA Shuffle", "NNAGA", "1.2.2",
    2, 2, 13, NNAGA_NATIVE_MAX_FRAMES, NNAGA_REALTIME_CERTIFIED_IN_PROCESS, kParameters, create, destroy, activate,
    deactivate, reset, setParameter, formatParameter, nullptr, process,
};
const NnagaPluginDescriptorV2* getPlugin(uint32_t index) noexcept {
    return index == 0 ? &kDescriptor : nullptr;
}
constexpr NnagaPluginLibraryV2 kLibrary = {
    sizeof(NnagaPluginLibraryV2), NNAGA_NATIVE_ABI_VERSION, 1, getPlugin,
};
} // namespace

extern "C" NNAGA_NATIVE_EXPORT const NnagaPluginLibraryV2*
nnaga_plugin_entry(uint32_t hostAbiVersion) noexcept {
    return hostAbiVersion == NNAGA_NATIVE_ABI_VERSION ? &kLibrary : nullptr;
}

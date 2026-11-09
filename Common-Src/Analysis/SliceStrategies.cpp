#include "Analysis/SliceStrategies.hpp"

#include "Analysis/SplitMix64.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace sms::analysis {

namespace {
constexpr double kSnapSeconds = 0.030;
constexpr double kSilenceRms = 0.01;  // about -40 dBFS

double beatFrames(const SliceContext& ctx)
{
    return ctx.bpm > 0.0 ? 60.0 / ctx.bpm * ctx.analysis.sourceRate : 0.0;
}

// Clamps to the range and length limits, drops short regions, sorts by start.
std::vector<SliceRegion> finalize(std::vector<SliceRegion> regions, const SliceContext& ctx)
{
    std::vector<SliceRegion> out;
    for (SliceRegion r : regions) {
        r.start = std::max(r.start, ctx.rangeStart);
        r.end = std::min(r.end, ctx.rangeEnd);
        if (ctx.maxFrames > 0) {
            r.end = std::min(r.end, r.start + ctx.maxFrames);
        }
        if (r.end - r.start >= std::max<std::int64_t>(ctx.minFrames, 1)) {
            out.push_back(r);
        }
    }
    std::sort(out.begin(), out.end(), [](const SliceRegion& a, const SliceRegion& b) { return a.start < b.start; });
    return out;
}

// Nearest candidate onset frame within `window` of `frame`; `frame` if none.
std::int64_t snapTo(const OnsetAnalysis& a, const std::int64_t frame, const std::int64_t window)
{
    const auto it = std::lower_bound(a.candidates.begin(), a.candidates.end(), frame,
                                     [](const Onset& o, const std::int64_t f) { return o.frame < f; });
    std::int64_t best = frame;
    std::int64_t bestDistance = window + 1;
    auto consider = [&](const std::vector<Onset>::const_iterator i) {
        if (i != a.candidates.end()) {
            const std::int64_t d = std::llabs(i->frame - frame);
            if (d < bestDistance) {
                bestDistance = d;
                best = i->frame;
            }
        }
    };
    consider(it);
    if (it != a.candidates.begin()) {
        consider(it - 1);
    }
    return bestDistance <= window ? best : frame;
}

float strengthAt(const OnsetAnalysis& a, const std::int64_t frame)
{
    const auto it = std::lower_bound(a.candidates.begin(), a.candidates.end(), frame,
                                     [](const Onset& o, const std::int64_t f) { return o.frame < f; });
    return it != a.candidates.end() && it->frame == frame ? it->strength : 0.0F;
}

struct RegionStats {
    double rms = 0.0;
    std::array<double, 3> bands{};
};

RegionStats statsFor(const OnsetAnalysis& a, const std::int64_t start, const std::int64_t end)
{
    RegionStats s;
    if (a.rms.empty() || a.hopSeconds <= 0.0 || a.sourceRate <= 0.0) {
        return s;
    }
    const double framesPerHop = a.hopSeconds * a.sourceRate;
    const std::size_t from = std::min(a.rms.size() - 1, static_cast<std::size_t>(std::max(0.0, static_cast<double>(start) / framesPerHop)));
    const std::size_t to = std::min(a.rms.size(), std::max(from + 1, static_cast<std::size_t>(static_cast<double>(end) / framesPerHop)));
    for (std::size_t i = from; i < to; ++i) {
        s.rms += static_cast<double>(a.rms[i]);
        for (std::size_t b = 0; b < 3; ++b) {
            s.bands[b] += static_cast<double>(a.bands[i][b]);
        }
    }
    const auto n = static_cast<double>(to - from);
    s.rms /= n;
    for (double& b : s.bands) {
        b /= n;
    }
    return s;
}

double cosine(const std::array<double, 3>& x, const std::array<double, 3>& y)
{
    double dot = 0.0;
    double nx = 0.0;
    double ny = 0.0;
    for (std::size_t i = 0; i < 3; ++i) {
        dot += x[i] * y[i];
        nx += x[i] * x[i];
        ny += y[i] * y[i];
    }
    return nx > 0.0 && ny > 0.0 ? dot / std::sqrt(nx * ny) : 0.0;
}
} // namespace

std::vector<SliceRegion> transientSlices(const SliceContext& ctx, const std::size_t maxSlices,
                                         const float sensitivity, const double minGapSeconds)
{
    std::vector<std::int64_t> onsets = pickOnsets(ctx.analysis, sensitivity, minGapSeconds, ctx.rangeStart, ctx.rangeEnd);
    std::vector<SliceRegion> regions;
    for (std::size_t i = 0; i < onsets.size() && regions.size() < maxSlices; ++i) {
        const std::int64_t end = i + 1 < onsets.size() ? onsets[i + 1] : ctx.rangeEnd;
        regions.push_back({onsets[i], end});
    }
    return finalize(std::move(regions), ctx);
}

std::vector<SliceRegion> equalSlices(const SliceContext& ctx, const std::size_t count)
{
    std::vector<SliceRegion> regions;
    const std::int64_t length = ctx.rangeEnd - ctx.rangeStart;
    if (count == 0 || length <= 0) {
        return regions;
    }
    const auto n = static_cast<std::int64_t>(count);
    for (std::int64_t i = 0; i < n; ++i) {
        regions.push_back({ctx.rangeStart + length * i / n, ctx.rangeStart + length * (i + 1) / n});
    }
    return finalize(std::move(regions), ctx);
}

std::vector<SliceRegion> beatSlices(const SliceContext& ctx, const std::size_t count, const double beatsPerSlice,
                                    const std::int64_t anchorFrame, const bool snapToTransients)
{
    std::vector<SliceRegion> regions;
    const double step = beatFrames(ctx) * beatsPerSlice;
    if (!(step >= 1.0) || count == 0) {
        return regions;
    }
    const std::int64_t anchor = std::clamp(anchorFrame, ctx.rangeStart, ctx.rangeEnd);
    const auto snap = static_cast<std::int64_t>(kSnapSeconds * ctx.analysis.sourceRate);
    auto boundary = [&](const std::size_t i) {
        const std::int64_t raw = anchor + std::llround(static_cast<double>(i) * step);
        return snapToTransients ? snapTo(ctx.analysis, raw, snap) : raw;
    };
    std::int64_t previous = boundary(0);
    for (std::size_t i = 1; i <= count && previous < ctx.rangeEnd; ++i) {
        std::int64_t next = std::max(boundary(i), previous + 1);
        regions.push_back({previous, std::min(next, ctx.rangeEnd)});
        previous = next;
    }
    return finalize(std::move(regions), ctx);
}

std::vector<SliceRegion> randomSlices(const SliceContext& ctx, const std::size_t count, const double lengthBeats,
                                      const std::uint64_t seed, const bool snapToTransients)
{
    std::vector<SliceRegion> regions;
    const double beat = beatFrames(ctx);
    const auto length = static_cast<std::int64_t>(std::llround(beat * lengthBeats));
    const std::int64_t span = ctx.rangeEnd - ctx.rangeStart - length;
    if (length < 1 || span < 0 || count == 0) {
        return regions;
    }
    SplitMix64 rng(seed);
    const auto quarterBeat = static_cast<std::int64_t>(beat / 4.0);
    for (std::size_t attempt = 0; attempt < count * 32 && regions.size() < count; ++attempt) {
        std::int64_t start = ctx.rangeStart + static_cast<std::int64_t>(rng.nextBelow(static_cast<std::uint64_t>(span) + 1));
        if (snapToTransients) {
            start = std::clamp(snapTo(ctx.analysis, start, quarterBeat), ctx.rangeStart, ctx.rangeStart + span);
        }
        const bool overlaps = std::any_of(regions.begin(), regions.end(), [&](const SliceRegion& r) {
            return start < r.end && start + length > r.start;
        });
        if (!overlaps) {
            regions.push_back({start, start + length});
        }
    }
    return finalize(std::move(regions), ctx);
}

std::vector<SliceRegion> findSamples(const SliceContext& ctx, const std::size_t count, const std::uint64_t seed)
{
    struct Candidate {
        SliceRegion region;
        double score = 0.0;
        std::array<double, 3> bands{};
    };
    std::vector<SliceRegion> result;
    const OnsetAnalysis& a = ctx.analysis;
    const std::int64_t range = ctx.rangeEnd - ctx.rangeStart;
    if (count == 0 || range <= 0 || a.sourceRate <= 0.0) {
        return result;
    }
    const double beat = ctx.bpm > 0.0 ? beatFrames(ctx) : 0.5 * a.sourceRate;
    const std::vector<std::int64_t> starts = pickOnsets(a, 0.5F, 0.05, ctx.rangeStart, ctx.rangeEnd);
    const std::vector<std::int64_t> strong = pickOnsets(a, 0.15F, 0.05, ctx.rangeStart, ctx.rangeEnd);
    SplitMix64 rng(seed);
    std::vector<Candidate> candidates;
    double maxRms = 0.0;
    for (const std::int64_t s : starts) {
        const auto next = std::upper_bound(strong.begin(), strong.end(), s);
        const std::int64_t limit = next != strong.end() ? *next : ctx.rangeEnd;
        std::int64_t end = 0;
        for (const double beats : {4.0, 2.0, 1.0}) {
            const auto len = static_cast<std::int64_t>(std::llround(beat * beats));
            if (s + len <= limit) {
                end = s + len;
                break;
            }
        }
        if (end == 0 && limit - s >= std::max<std::int64_t>(ctx.minFrames, 1)) {
            end = limit;
        }
        if (end <= s) {
            continue;
        }
        const RegionStats st = statsFor(a, s, end);
        if (st.rms < kSilenceRms) {
            continue;
        }
        const double length = static_cast<double>(end - s);
        const double fit = std::clamp(length / std::max(1.0, static_cast<double>(limit - s)), 0.0, 1.0);
        double score = static_cast<double>(strengthAt(a, s)) * fit * st.rms;
        if (seed != 0) {
            score *= 1.0 + 0.1 * (2.0 * rng.nextUnit() - 1.0);
        }
        maxRms = std::max(maxRms, st.rms);
        candidates.push_back({{s, end}, score, st.bands});
    }
    if (maxRms > 0.0) {
        for (Candidate& c : candidates) {
            c.score /= maxRms;
        }
    }
    const std::size_t quota = (count + 3) / 4;
    std::array<std::size_t, 4> perQuarter{};
    std::vector<const Candidate*> picked;
    std::vector<bool> used(candidates.size(), false);
    while (picked.size() < count) {
        double best = -1e30;
        std::size_t bestIndex = candidates.size();
        for (std::size_t i = 0; i < candidates.size(); ++i) {
            if (used[i]) {
                continue;
            }
            const Candidate& c = candidates[i];
            const std::size_t quarter = std::min<std::size_t>(3, static_cast<std::size_t>((c.region.start - ctx.rangeStart) * 4 / range));
            bool ok = perQuarter[quarter] < quota;
            double similarity = 0.0;
            for (const Candidate* p : picked) {
                const bool overlap = c.region.start < p->region.end && c.region.end > p->region.start;
                const bool tooClose = static_cast<double>(std::llabs(c.region.start - p->region.start)) < beat;
                ok = ok && !overlap && !tooClose;
                similarity = std::max(similarity, cosine(c.bands, p->bands));
            }
            if (!ok) {
                continue;
            }
            const double value = c.score - 0.5 * similarity;
            if (value > best) {
                best = value;
                bestIndex = i;
            }
        }
        if (bestIndex == candidates.size()) {
            break;
        }
        used[bestIndex] = true;
        const Candidate& chosen = candidates[bestIndex];
        ++perQuarter[std::min<std::size_t>(3, static_cast<std::size_t>((chosen.region.start - ctx.rangeStart) * 4 / range))];
        picked.push_back(&chosen);
    }
    for (const Candidate* p : picked) {
        result.push_back(p->region);
    }
    return finalize(std::move(result), ctx);
}

} // namespace sms::analysis

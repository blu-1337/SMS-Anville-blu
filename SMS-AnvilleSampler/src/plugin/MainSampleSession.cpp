#include "MainSampleSession.hpp"

#include <algorithm>

namespace midichopper::plugin {
namespace {

void sortByStart(std::vector<MainSampleSlice>& slices)
{
    std::stable_sort(slices.begin(), slices.end(),
        [](const MainSampleSlice& left, const MainSampleSlice& right) {
            return left.start < right.start;
        });
}

[[nodiscard]] bool sortedByStart(const std::vector<MainSampleSlice>& slices) noexcept
{
    return std::is_sorted(slices.begin(), slices.end(),
        [](const MainSampleSlice& left, const MainSampleSlice& right) {
            return left.start < right.start;
        });
}

} // namespace

void MainSampleSession::reset(const std::uint64_t id, const std::uint32_t frameCount,
                              const std::uint32_t rate, const std::string_view sourceName)
{
    sourceId = id & kMainSampleSourceIdMask;
    frames = sourceId != 0U ? frameCount : 0U;
    sampleRate = sourceId != 0U ? rate : 0U;
    nextSliceId = 1U;
    slices.clear();
    name = sourceId != 0U ? main_sample_detail::truncateUtf8(sourceName, kMainSampleNameBytes)
                          : std::string{};
}

const MainSampleSlice* MainSampleSession::find(const std::uint16_t id) const noexcept
{
    for (const auto& slice : slices)
        if (slice.id == id)
            return &slice;
    return nullptr;
}

bool MainSampleSession::addSlice(const std::uint32_t start, const std::uint32_t end,
                                 MainSampleSlice& created)
{
    if (empty() || start >= end || end > frames || slices.size() >= kMainSampleMaxSlices)
        return false;
    std::uint16_t id = nextSliceId == 0U ? std::uint16_t{1} : nextSliceId;
    while (find(id)) // at most 64 ids are in use, so this ends quickly
        id = id == UINT16_MAX ? std::uint16_t{1} : static_cast<std::uint16_t>(id + 1U);
    created = {id, start, end};
    slices.push_back(created);
    sortByStart(slices);
    nextSliceId = id == UINT16_MAX ? std::uint16_t{1} : static_cast<std::uint16_t>(id + 1U);
    return true;
}

bool MainSampleSession::editSlices(const std::span<const MainSampleSlice> edits)
{
    if (edits.empty() || edits.size() > 2U ||
        (edits.size() == 2U && edits[0].id == edits[1].id))
        return false;
    auto edited = slices;
    for (const auto& edit : edits) {
        const auto found = std::find_if(edited.begin(), edited.end(),
            [&](const MainSampleSlice& slice) { return slice.id == edit.id; });
        if (found == edited.end() || edit.start >= edit.end || edit.end > frames)
            return false;
        *found = edit;
    }
    sortByStart(edited);
    slices = std::move(edited);
    return true;
}

bool MainSampleSession::removeSlice(const std::uint16_t id)
{
    const auto found = std::find_if(slices.begin(), slices.end(),
        [&](const MainSampleSlice& slice) { return slice.id == id; });
    if (found == slices.end())
        return false;
    slices.erase(found);
    return true;
}

std::string encodeMainSampleSession(const MainSampleSession& session)
{
    if (session.empty())
        return {};
    return "MSS1;" + main_sample_detail::hex(session.sourceId) + ";" +
           std::to_string(session.frames) + ";" + std::to_string(session.sampleRate) + ";" +
           std::to_string(session.nextSliceId) + ";" +
           main_sample_detail::encodeSlices(session.slices) + ";" +
           main_sample_detail::truncateUtf8(session.name, kMainSampleNameBytes);
}

bool decodeMainSampleSession(const std::string_view encoded, MainSampleSession& session)
{
    using main_sample_detail::Fields;
    if (!encoded.starts_with("MSS1;"))
        return false;
    Fields fields(encoded.substr(5U));
    MainSampleSession decoded;
    std::string_view slices;
    std::string_view name;
    if (!fields.number(decoded.sourceId, 16) || !fields.number(decoded.frames) ||
        !fields.number(decoded.sampleRate) || !fields.number(decoded.nextSliceId) ||
        !fields.next(slices) || !fields.last(name) || decoded.sourceId == 0U ||
        decoded.sourceId > kMainSampleSourceIdMask || decoded.frames == 0U ||
        decoded.sampleRate < 2U || decoded.sampleRate > 768000U || decoded.nextSliceId == 0U ||
        name.size() > kMainSampleNameBytes ||
        !main_sample_detail::decodeSlices(slices, decoded.frames, decoded.slices) ||
        !sortedByStart(decoded.slices))
        return false;
    decoded.name = std::string(name);
    session = std::move(decoded);
    return true;
}

std::string encodeMainSampleLinks(const std::span<const MainSampleLinkRecord> links)
{
    std::string encoded = "LK1;";
    for (std::size_t index = 0; index < links.size(); ++index) {
        const auto& link = links[index];
        if (index != 0U)
            encoded.push_back(',');
        encoded += std::to_string(link.pad) + ":" + main_sample_detail::hex(link.provenance) +
                   ":" + std::to_string(link.frames) + ":" +
                   main_sample_detail::hex(link.fingerprint);
    }
    return encoded;
}

bool decodeMainSampleLinks(std::string_view encoded, std::vector<MainSampleLinkRecord>& links)
{
    using main_sample_detail::parse;
    if (!encoded.starts_with("LK1;"))
        return false;
    encoded.remove_prefix(4U);
    std::vector<MainSampleLinkRecord> decoded;
    std::uint64_t seenPads = 0U;
    while (!encoded.empty()) {
        const auto separator = encoded.find(',');
        const auto entry = encoded.substr(0U, separator);
        const auto first = entry.find(':');
        const auto second = first == std::string_view::npos ? first : entry.find(':', first + 1U);
        const auto third = second == std::string_view::npos ? second : entry.find(':', second + 1U);
        MainSampleLinkRecord link;
        if (third == std::string_view::npos || decoded.size() == kPadCount ||
            !parse(entry.substr(0U, first), link.pad) ||
            !parse(entry.substr(first + 1U, second - first - 1U), link.provenance, 16) ||
            !parse(entry.substr(second + 1U, third - second - 1U), link.frames) ||
            !parse(entry.substr(third + 1U), link.fingerprint, 16) || link.pad >= kPadCount ||
            link.provenance == 0U || link.frames == 0U ||
            (seenPads & (std::uint64_t{1} << link.pad)) != 0U)
            return false;
        seenPads |= std::uint64_t{1} << link.pad;
        decoded.push_back(link);
        if (separator == std::string_view::npos)
            break;
        encoded.remove_prefix(separator + 1U);
        if (encoded.empty())
            return false;
    }
    links = std::move(decoded);
    return true;
}

std::uint64_t makeMainSampleSourceId(const std::uint64_t seed) noexcept
{
    std::uint64_t value = seed + 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    value ^= value >> 31U;
    const auto id = value & kMainSampleSourceIdMask;
    return id != 0U ? id : 1U;
}

} // namespace midichopper::plugin

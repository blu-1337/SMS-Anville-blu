#pragma once

// Main Sample transient protocols: MS1 requests and status, MW1 waveform tiles
// and MP1 preview commands. Fields are ASCII and ';'-separated; a path, name or
// message is always the last field and may itself contain ';'.

#include "Audio/WaveformSummary.hpp"
#include "Configuration.hpp"
#include "SourcePreviewRequest.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace midichopper::plugin {

inline constexpr std::size_t kMainSampleMaxSlices = 64U;
inline constexpr std::size_t kMainSampleMaxTiles = 4U;
// MG1 spectrogram: columns x bins bytes, Base64 on the UI bus (8191 B per message).
inline constexpr std::size_t kSpectrogramBins = 48U;
inline constexpr std::size_t kSpectrogramMaxColumns = 120U;
inline constexpr std::size_t kMainSampleNameBytes = 128U;
inline constexpr std::size_t kMainSampleTextBytes = 512U;
inline constexpr std::size_t kMainSamplePathBytes = 4096U;
inline constexpr std::uint64_t kMainSampleSourceIdMask = (std::uint64_t{1} << 48U) - 1U;

struct MainSampleSlice {
    std::uint16_t id = 0U;
    std::uint32_t start = 0U; // source frames, [start, end)
    std::uint32_t end = 0U;
    friend bool operator==(const MainSampleSlice&, const MainSampleSlice&) = default;
};

struct MainSampleLink {
    std::uint32_t pad = 0U;
    std::uint16_t sliceId = 0U;
    friend bool operator==(const MainSampleLink&, const MainSampleLink&) = default;
};

namespace main_sample_detail {

template <class Integer>
[[nodiscard]] inline bool parse(const std::string_view text, Integer& value,
                                const int base = 10) noexcept
{
    if (text.empty())
        return false;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value, base);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

[[nodiscard]] inline std::string hex(const std::uint64_t value)
{
    std::array<char, 16> digits{};
    const auto result = std::to_chars(digits.data(), digits.data() + digits.size(), value, 16);
    return std::string(digits.data(), result.ptr);
}

/** Takes ';'-separated fields from the front; last() keeps any ';' in the tail. */
class Fields {
public:
    explicit Fields(const std::string_view text) noexcept : rest_(text) {}

    [[nodiscard]] bool next(std::string_view& field) noexcept
    {
        if (finished_)
            return false;
        const auto separator = rest_.find(';');
        field = rest_.substr(0U, separator);
        if (separator == std::string_view::npos) {
            rest_ = {};
            finished_ = true;
        } else {
            rest_ = rest_.substr(separator + 1U);
        }
        return true;
    }

    template <class Integer>
    [[nodiscard]] bool number(Integer& value, const int base = 10) noexcept
    {
        std::string_view field;
        return next(field) && parse(field, value, base);
    }

    [[nodiscard]] bool last(std::string_view& field) noexcept
    {
        if (finished_)
            return false;
        field = rest_;
        rest_ = {};
        finished_ = true;
        return true;
    }

    [[nodiscard]] bool finished() const noexcept { return finished_; }

private:
    std::string_view rest_;
    bool finished_ = false;
};

/** Cuts at most maxBytes without splitting a UTF-8 sequence. */
[[nodiscard]] inline std::string truncateUtf8(const std::string_view text,
                                              const std::size_t maxBytes)
{
    if (text.size() <= maxBytes)
        return std::string(text);
    std::size_t cut = maxBytes;
    while (cut > 0U && (static_cast<unsigned char>(text[cut]) & 0xC0U) == 0x80U)
        --cut;
    return std::string(text.substr(0U, cut));
}

[[nodiscard]] inline std::string encodeSlices(const std::vector<MainSampleSlice>& slices)
{
    std::string encoded;
    for (const auto& slice : slices) {
        if (!encoded.empty())
            encoded.push_back(',');
        encoded += std::to_string(slice.id) + ":" + std::to_string(slice.start) + ":" +
                   std::to_string(slice.end);
    }
    return encoded;
}

[[nodiscard]] inline bool decodeSlice(const std::string_view text, MainSampleSlice& slice) noexcept
{
    const auto first = text.find(':');
    const auto second = first == std::string_view::npos ? first : text.find(':', first + 1U);
    if (second == std::string_view::npos)
        return false;
    MainSampleSlice decoded;
    if (!parse(text.substr(0U, first), decoded.id) ||
        !parse(text.substr(first + 1U, second - first - 1U), decoded.start) ||
        !parse(text.substr(second + 1U), decoded.end) || decoded.id == 0U ||
        decoded.start >= decoded.end)
        return false;
    slice = decoded;
    return true;
}

/** Valid lists hold at most 64 slices with unique nonzero ids inside [0, frames). */
[[nodiscard]] inline bool decodeSlices(std::string_view text, const std::uint32_t frames,
                                       std::vector<MainSampleSlice>& slices)
{
    std::vector<MainSampleSlice> decoded;
    while (!text.empty()) {
        const auto separator = text.find(',');
        MainSampleSlice slice;
        if (decoded.size() == kMainSampleMaxSlices ||
            !decodeSlice(text.substr(0U, separator), slice) || slice.end > frames)
            return false;
        for (const auto& existing : decoded)
            if (existing.id == slice.id)
                return false;
        decoded.push_back(slice);
        if (separator == std::string_view::npos)
            break;
        text = text.substr(separator + 1U);
        if (text.empty())
            return false;
    }
    slices = std::move(decoded);
    return true;
}

} // namespace main_sample_detail

enum class MainSampleVerb : std::uint8_t {
    load, clear, query, edit, assign, remove, waveform, arrange, spectrogram };

/** One pad whose audio and settings move to another pad. */
struct ArrangeMove {
    std::uint32_t from = 0U;
    std::uint32_t to = 0U;
    friend bool operator==(const ArrangeMove&, const ArrangeMove&) = default;
};

/**
 * Arrange: the linked pads keep their set of pad numbers, but the slices in
 * time order (earliest start, then id) take those pads in ascending order.
 * Returns only the pads that change; the result is a permutation of the set.
 */
[[nodiscard]] inline std::vector<ArrangeMove> arrangeMoves(
    const std::vector<MainSampleSlice>& slices, const std::vector<MainSampleLink>& links)
{
    struct Entry {
        std::uint32_t pad;
        MainSampleSlice slice;
    };
    std::vector<Entry> entries;
    std::vector<std::uint32_t> pads;
    for (const auto& link : links) {
        for (const auto& slice : slices) {
            if (slice.id != link.sliceId)
                continue;
            bool duplicate = false;
            for (const auto pad : pads)
                duplicate = duplicate || pad == link.pad;
            if (!duplicate) {
                entries.push_back({link.pad, slice});
                pads.push_back(link.pad);
            }
            break;
        }
    }
    std::sort(pads.begin(), pads.end());
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        return a.slice.start != b.slice.start ? a.slice.start < b.slice.start
                                              : a.slice.id < b.slice.id;
    });
    std::vector<ArrangeMove> moves;
    for (std::size_t index = 0; index < entries.size(); ++index)
        if (entries[index].pad != pads[index])
            moves.push_back({entries[index].pad, pads[index]});
    return moves;
}

struct MainSampleRequest {
    std::uint64_t sequence = 0U;
    MainSampleVerb verb = MainSampleVerb::query;
    std::string path;                       // load
    std::array<MainSampleSlice, 2> edits{}; // edit: one slice, or two sharing a dragged boundary
    std::uint32_t editCount = 0U;
    std::uint32_t pad = 0U;                 // assign
    std::uint32_t start = 0U;               // assign, waveform
    std::uint32_t end = 0U;
    bool confirm = false;                   // assign
    std::uint16_t sliceId = 0U;             // remove
    std::uint32_t tiles = 0U;               // waveform; spectrogram: columns
};

[[nodiscard]] inline std::string encodeMainSampleRequest(const MainSampleRequest& request)
{
    std::string encoded = "MS1;" + std::to_string(request.sequence) + ";";
    switch (request.verb) {
    case MainSampleVerb::load: return encoded + "L;" + request.path;
    case MainSampleVerb::clear: return encoded + "C";
    case MainSampleVerb::query: return encoded + "Q";
    case MainSampleVerb::edit:
        return encoded + "S;" + main_sample_detail::encodeSlices(std::vector<MainSampleSlice>(
            request.edits.begin(), request.edits.begin() + (request.editCount > 1U ? 2 : 1)));
    case MainSampleVerb::assign:
        return encoded + "A;" + std::to_string(request.pad) + ";" + std::to_string(request.start) +
               ";" + std::to_string(request.end) + ";" + (request.confirm ? "1" : "0");
    case MainSampleVerb::arrange: return encoded + "R";
    case MainSampleVerb::remove: return encoded + "D;" + std::to_string(request.sliceId);
    case MainSampleVerb::spectrogram:
        return encoded + "G;" + std::to_string(request.start) + ";" + std::to_string(request.end) +
               ";" + std::to_string(request.tiles);
    case MainSampleVerb::waveform:
        return encoded + "W;" + std::to_string(request.start) + ";" + std::to_string(request.end) +
               ";" + std::to_string(request.tiles);
    }
    return {};
}

[[nodiscard]] inline bool decodeMainSampleRequest(const std::string_view encoded,
                                                  MainSampleRequest& request)
{
    using main_sample_detail::Fields;
    if (!encoded.starts_with("MS1;"))
        return false;
    Fields fields(encoded.substr(4U));
    MainSampleRequest decoded;
    std::string_view verb;
    if (!fields.number(decoded.sequence) || !fields.next(verb) || verb.size() != 1U)
        return false;
    switch (verb[0]) {
    case 'L': {
        std::string_view path;
        if (!fields.last(path) || path.empty() || path.size() > kMainSamplePathBytes)
            return false;
        decoded.verb = MainSampleVerb::load;
        decoded.path = std::string(path);
        break;
    }
    case 'C':
    case 'Q':
    case 'R':
        if (!fields.finished())
            return false;
        decoded.verb = verb[0] == 'C' ? MainSampleVerb::clear
            : verb[0] == 'Q' ? MainSampleVerb::query : MainSampleVerb::arrange;
        break;
    case 'S': {
        std::string_view list;
        std::vector<MainSampleSlice> edits;
        if (!fields.next(list) || !fields.finished() ||
            !main_sample_detail::decodeSlices(list, UINT32_MAX, edits) || edits.empty() ||
            edits.size() > 2U)
            return false;
        decoded.verb = MainSampleVerb::edit;
        decoded.editCount = static_cast<std::uint32_t>(edits.size());
        for (std::size_t index = 0; index < edits.size(); ++index)
            decoded.edits[index] = edits[index];
        break;
    }
    case 'A': {
        unsigned confirm = 2U;
        if (!fields.number(decoded.pad) || !fields.number(decoded.start) ||
            !fields.number(decoded.end) || !fields.number(confirm) || !fields.finished() ||
            decoded.pad >= kPadCount || decoded.start >= decoded.end || confirm > 1U)
            return false;
        decoded.verb = MainSampleVerb::assign;
        decoded.confirm = confirm == 1U;
        break;
    }
    case 'D':
        if (!fields.number(decoded.sliceId) || !fields.finished() || decoded.sliceId == 0U)
            return false;
        decoded.verb = MainSampleVerb::remove;
        break;
    case 'G':
        if (!fields.number(decoded.start) || !fields.number(decoded.end) ||
            !fields.number(decoded.tiles) || !fields.finished() || decoded.start >= decoded.end ||
            decoded.tiles == 0U || decoded.tiles > kSpectrogramMaxColumns)
            return false;
        decoded.verb = MainSampleVerb::spectrogram;
        break;
    case 'W':
        if (!fields.number(decoded.start) || !fields.number(decoded.end) ||
            !fields.number(decoded.tiles) || !fields.finished() || decoded.start >= decoded.end ||
            decoded.tiles == 0U || decoded.tiles > kMainSampleMaxTiles)
            return false;
        decoded.verb = MainSampleVerb::waveform;
        break;
    default:
        return false;
    }
    request = std::move(decoded);
    return true;
}

enum class MainSampleState : std::uint8_t { empty, loading, ready, error, confirm };

/** One message per operation, never per pad (the UI bus has 64 slots). */
struct MainSampleStatus {
    std::uint64_t sequence = 0U;
    MainSampleState state = MainSampleState::empty;
    std::uint64_t sourceId = 0U;
    std::uint32_t frames = 0U;
    std::uint32_t sampleRate = 0U; // whole hertz
    std::vector<MainSampleSlice> slices;
    std::vector<MainSampleLink> links;
    std::uint64_t affectedMask = 0U; // pads re-cut or unlinked by this operation
    std::string text;                // source name, or the error/confirm message
};

namespace main_sample_detail {
inline constexpr std::array<std::string_view, 5> kStateNames{
    "EMPTY", "LOADING", "READY", "ERROR", "CONFIRM"};
}

[[nodiscard]] inline std::string encodeMainSampleStatus(const MainSampleStatus& status)
{
    using namespace main_sample_detail;
    std::string links;
    for (const auto& link : status.links) {
        if (!links.empty())
            links.push_back(',');
        links += std::to_string(link.pad) + ":" + std::to_string(link.sliceId);
    }
    return "MS1;" + std::to_string(status.sequence) + ";" +
           std::string(kStateNames[static_cast<std::size_t>(status.state)]) + ";" +
           hex(status.sourceId) + ";" + std::to_string(status.frames) + ";" +
           std::to_string(status.sampleRate) + ";" + encodeSlices(status.slices) + ";" + links +
           ";" + hex(status.affectedMask) + ";" + truncateUtf8(status.text, kMainSampleTextBytes);
}

[[nodiscard]] inline bool decodeMainSampleStatus(const std::string_view encoded,
                                                 MainSampleStatus& status)
{
    using namespace main_sample_detail;
    if (!encoded.starts_with("MS1;"))
        return false;
    Fields fields(encoded.substr(4U));
    MainSampleStatus decoded;
    std::string_view state;
    std::string_view slices;
    std::string_view links;
    std::string_view text;
    if (!fields.number(decoded.sequence) || !fields.next(state) ||
        !fields.number(decoded.sourceId, 16) || !fields.number(decoded.frames) ||
        !fields.number(decoded.sampleRate) || !fields.next(slices) || !fields.next(links) ||
        !fields.number(decoded.affectedMask, 16) || !fields.last(text) ||
        decoded.sourceId > kMainSampleSourceIdMask || text.size() > kMainSampleTextBytes ||
        !decodeSlices(slices, decoded.frames, decoded.slices))
        return false;
    std::size_t stateIndex = 0U;
    while (stateIndex < kStateNames.size() && kStateNames[stateIndex] != state)
        ++stateIndex;
    if (stateIndex == kStateNames.size())
        return false;
    decoded.state = static_cast<MainSampleState>(stateIndex);
    std::uint64_t seenPads = 0U;
    while (!links.empty()) {
        const auto separator = links.find(',');
        const auto entry = links.substr(0U, separator);
        const auto colon = entry.find(':');
        MainSampleLink link;
        if (colon == std::string_view::npos || !parse(entry.substr(0U, colon), link.pad) ||
            !parse(entry.substr(colon + 1U), link.sliceId) || link.pad >= kPadCount ||
            link.sliceId == 0U || (seenPads & (std::uint64_t{1} << link.pad)) != 0U)
            return false;
        bool known = false;
        for (const auto& slice : decoded.slices)
            known = known || slice.id == link.sliceId;
        if (!known)
            return false;
        seenPads |= std::uint64_t{1} << link.pad;
        decoded.links.push_back(link);
        if (separator == std::string_view::npos)
            break;
        links = links.substr(separator + 1U);
        if (links.empty())
            return false;
    }
    decoded.text = std::string(text);
    status = std::move(decoded);
    return true;
}

/** Tiles split [start, end) evenly, as SamplerEngine::summarizeSourceRange does. */
struct MainSampleWaveform {
    std::uint64_t sequence = 0U;
    std::uint32_t start = 0U;
    std::uint32_t end = 0U;
    std::uint32_t count = 0U;
    std::array<sms::audio::WaveformSummary, kMainSampleMaxTiles> tiles{};
};

[[nodiscard]] inline std::uint32_t mainSampleTileFrames(const std::uint32_t start,
    const std::uint32_t end, const std::uint32_t count, const std::uint32_t tile) noexcept
{
    const std::uint64_t length = end - start;
    return static_cast<std::uint32_t>(length * (tile + 1U) / count - length * tile / count);
}

[[nodiscard]] inline std::string encodeMainSampleWaveform(const MainSampleWaveform& waveform)
{
    std::string encoded = "MW1;" + std::to_string(waveform.sequence) + ";" +
                          std::to_string(waveform.start) + ";" + std::to_string(waveform.end) +
                          ";" + std::to_string(waveform.count);
    for (std::uint32_t tile = 0; tile < waveform.count && tile < kMainSampleMaxTiles; ++tile)
        encoded += ";" + sms::audio::encodeWaveformSummary(waveform.tiles[tile]);
    return encoded;
}

[[nodiscard]] inline bool decodeMainSampleWaveform(const std::string_view encoded,
                                                   MainSampleWaveform& waveform) noexcept
{
    using main_sample_detail::Fields;
    if (!encoded.starts_with("MW1;"))
        return false;
    const auto firstTile = encoded.find(";WS2;");
    if (firstTile == std::string_view::npos)
        return false;
    Fields fields(encoded.substr(4U, firstTile - 4U));
    MainSampleWaveform decoded;
    if (!fields.number(decoded.sequence) || !fields.number(decoded.start) ||
        !fields.number(decoded.end) || !fields.number(decoded.count) || !fields.finished() ||
        decoded.start >= decoded.end || decoded.count == 0U ||
        decoded.count > kMainSampleMaxTiles)
        return false;
    auto rest = encoded.substr(firstTile + 1U);
    for (std::uint32_t tile = 0; tile < decoded.count; ++tile) {
        const auto next = rest.find(";WS2;");
        if ((next == std::string_view::npos) != (tile + 1U == decoded.count) ||
            !sms::audio::decodeWaveformSummary(rest.substr(0U, next), decoded.tiles[tile]) ||
            decoded.tiles[tile].frames !=
                mainSampleTileFrames(decoded.start, decoded.end, decoded.count, tile))
            return false;
        if (next != std::string_view::npos)
            rest = rest.substr(next + 1U);
    }
    waveform = decoded;
    return true;
}

/** MG1: a column-major (column, bin) 8-bit spectrogram of [start, end), bin 0 = lowest frequency. */
struct MainSampleSpectrogram {
    std::uint64_t sequence = 0U;
    std::uint32_t start = 0U;
    std::uint32_t end = 0U;
    std::uint32_t columns = 0U;
    std::vector<std::uint8_t> data; // columns * kSpectrogramBins
};

namespace main_sample_detail {
inline constexpr std::string_view kBase64 =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
inline void appendBase64(std::string& out, const std::vector<std::uint8_t>& data)
{
    std::size_t i = 0;
    for (; i + 2U < data.size(); i += 3U) {
        const std::uint32_t v = (std::uint32_t{data[i]} << 16U) | (std::uint32_t{data[i + 1U]} << 8U) | data[i + 2U];
        out += kBase64[(v >> 18U) & 63U];
        out += kBase64[(v >> 12U) & 63U];
        out += kBase64[(v >> 6U) & 63U];
        out += kBase64[v & 63U];
    }
    if (i + 1U == data.size()) {
        const std::uint32_t v = std::uint32_t{data[i]} << 16U;
        out += kBase64[(v >> 18U) & 63U];
        out += kBase64[(v >> 12U) & 63U];
        out += "==";
    } else if (i + 2U == data.size()) {
        const std::uint32_t v = (std::uint32_t{data[i]} << 16U) | (std::uint32_t{data[i + 1U]} << 8U);
        out += kBase64[(v >> 18U) & 63U];
        out += kBase64[(v >> 12U) & 63U];
        out += kBase64[(v >> 6U) & 63U];
        out += '=';
    }
}
[[nodiscard]] inline bool decodeBase64(const std::string_view text, std::vector<std::uint8_t>& out)
{
    out.clear();
    if (text.size() % 4U != 0U)
        return false;
    for (std::size_t i = 0; i < text.size(); i += 4U) {
        std::uint32_t v = 0;
        int pad = 0;
        for (std::size_t j = 0; j < 4U; ++j) {
            const char c = text[i + j];
            std::uint32_t digit = 0;
            if (c == '=' && i + 4U == text.size() && j >= 2U) {
                ++pad;
            } else if (pad != 0 || kBase64.find(c) == std::string_view::npos) {
                return false;
            } else {
                digit = static_cast<std::uint32_t>(kBase64.find(c));
            }
            v = (v << 6U) | digit;
        }
        out.push_back(static_cast<std::uint8_t>(v >> 16U));
        if (pad < 2)
            out.push_back(static_cast<std::uint8_t>(v >> 8U));
        if (pad < 1)
            out.push_back(static_cast<std::uint8_t>(v));
    }
    return true;
}
} // namespace main_sample_detail

[[nodiscard]] inline std::string encodeMainSampleSpectrogram(const MainSampleSpectrogram& s)
{
    std::string encoded = "MG1;" + std::to_string(s.sequence) + ";" + std::to_string(s.start) + ";" +
                          std::to_string(s.end) + ";" + std::to_string(s.columns) + ";";
    main_sample_detail::appendBase64(encoded, s.data);
    return encoded;
}

[[nodiscard]] inline bool decodeMainSampleSpectrogram(const std::string_view encoded,
                                                      MainSampleSpectrogram& spectrogram)
{
    using main_sample_detail::Fields;
    if (!encoded.starts_with("MG1;"))
        return false;
    Fields fields(encoded.substr(4U));
    MainSampleSpectrogram decoded;
    std::string_view payload;
    if (!fields.number(decoded.sequence) || !fields.number(decoded.start) ||
        !fields.number(decoded.end) || !fields.number(decoded.columns) || !fields.next(payload) ||
        !fields.finished() || decoded.start >= decoded.end || decoded.columns == 0U ||
        decoded.columns > kSpectrogramMaxColumns ||
        !main_sample_detail::decodeBase64(payload, decoded.data) ||
        decoded.data.size() != decoded.columns * kSpectrogramBins)
        return false;
    spectrogram = std::move(decoded);
    return true;
}

[[nodiscard]] inline std::string encodeSourcePreviewRequest(const SourcePreviewRequest& request)
{
    return std::string("MP1;") + (request.play ? "1;" : "0;") + (request.loop ? "1;" : "0;") +
           main_sample_detail::hex(request.sourceId) + ";" + std::to_string(request.start) + ";" +
           std::to_string(request.end);
}

/** A stop needs no range; a play needs a source id and a non-empty range. */
[[nodiscard]] inline bool decodeSourcePreviewRequest(const std::string_view encoded,
                                                     SourcePreviewRequest& request) noexcept
{
    using main_sample_detail::Fields;
    if (!encoded.starts_with("MP1;"))
        return false;
    Fields fields(encoded.substr(4U));
    unsigned play = 2U;
    unsigned loop = 2U;
    SourcePreviewRequest decoded;
    if (!fields.number(play) || !fields.number(loop) || !fields.number(decoded.sourceId, 16) ||
        !fields.number(decoded.start) || !fields.number(decoded.end) || !fields.finished() ||
        play > 1U || loop > 1U || decoded.sourceId > kMainSampleSourceIdMask ||
        (play == 1U && (decoded.sourceId == 0U || decoded.start >= decoded.end)))
        return false;
    decoded.play = play == 1U;
    decoded.loop = loop == 1U;
    request = decoded;
    return true;
}

} // namespace midichopper::plugin

// Main Sample codecs: MS1 requests/status, MW1 waveform tiles, MP1 preview,
// MSS1 session and LK1 links, plus the session's slice model.
#include "AutoChopProtocol.hpp"
#include "MainSampleProtocol.hpp"
#include "MainSampleSession.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {
using namespace midichopper::plugin;

void check(const bool value, const char* const message)
{
    if (!value) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

MainSampleRequest roundTrip(const MainSampleRequest& request)
{
    MainSampleRequest decoded;
    check(decodeMainSampleRequest(encodeMainSampleRequest(request), decoded), "request round-trip");
    return decoded;
}

void requests()
{
    MainSampleRequest load;
    load.sequence = 7U;
    load.verb = MainSampleVerb::load;
    load.path = "C:\\beats;2024\\song.wav";
    auto decoded = roundTrip(load);
    check(decoded.sequence == 7U && decoded.verb == MainSampleVerb::load &&
          decoded.path == load.path, "a load path keeps its ';'");

    MainSampleRequest edit;
    edit.verb = MainSampleVerb::edit;
    edit.edits = {{{3U, 0U, 100U}, {4U, 100U, 250U}}};
    edit.editCount = 2U;
    decoded = roundTrip(edit);
    check(decoded.editCount == 2U && decoded.edits[0] == edit.edits[0] &&
          decoded.edits[1] == edit.edits[1], "a shared-boundary edit carries two slices");

    MainSampleRequest assign;
    assign.verb = MainSampleVerb::assign;
    assign.pad = 63U;
    assign.start = 10U;
    assign.end = 20U;
    assign.confirm = true;
    decoded = roundTrip(assign);
    check(decoded.pad == 63U && decoded.start == 10U && decoded.end == 20U && decoded.confirm,
          "assign round-trips");
    MainSampleRequest waveform;
    waveform.verb = MainSampleVerb::waveform;
    waveform.start = 5U;
    waveform.end = 9U;
    waveform.tiles = 4U;
    decoded = roundTrip(waveform);
    check(decoded.tiles == 4U && decoded.start == 5U && decoded.end == 9U, "waveform round-trips");
    MainSampleRequest remove;
    remove.verb = MainSampleVerb::remove;
    remove.sliceId = 65535U;
    check(roundTrip(remove).sliceId == 65535U, "remove round-trips");

    MainSampleRequest arrange;
    arrange.verb = MainSampleVerb::arrange;
    check(roundTrip(arrange).verb == MainSampleVerb::arrange, "arrange round-trips");

    MainSampleRequest ignored;
    for (const char* bad : {"MS1;99999999999999999999;Q", "MS1;1;Q;x", "MS1;1;C;", "MS1;1;D;5;",
                            "MS1;1;A;3;0;10", "MS1;1;A;64;0;10;0", "MS1;1;A;3;10;10;0",
                            "MS1;1;A;3;0;10;2", "MS1;1;W;0;10;5", "MS1;1;W;0;10;0",
                            "MS1;1;S;1:0:5,1:5:9", "MS1;1;S;1:0:5,2:5:9,3:9:12", "MS1;1;S;",
                            "MS1;1;S;1:0:5,", "MS1;1;D;0", "MS1;1;L;", "MS1;1;L", "MS1;1;X",
                            "MS1;1;QQ", "MS1;1;R;", "MS2;1;Q", "MS1;-1;Q", "MS1;1;D;65536", "MS1;1;S;0:0:5"})
        check(!decodeMainSampleRequest(bad, ignored), bad);
}

MainSampleStatus worstStatus()
{
    MainSampleStatus status;
    status.sequence = UINT64_MAX;
    status.state = MainSampleState::confirm;
    status.sourceId = kMainSampleSourceIdMask;
    status.frames = 184'320'000U;
    status.sampleRate = 96000U;
    for (std::uint32_t index = 0; index < kMainSampleMaxSlices; ++index) {
        status.slices.push_back({static_cast<std::uint16_t>(65535U - index),
                                 183'000'000U + index, 184'320'000U});
        status.links.push_back({index, static_cast<std::uint16_t>(65535U - index)});
    }
    status.affectedMask = UINT64_MAX;
    status.text = std::string(2000U, 'x');
    return status;
}

void statuses()
{
    MainSampleStatus status;
    status.sequence = 12U;
    status.state = MainSampleState::ready;
    status.sourceId = 0xabcdef123456ULL;
    status.frames = 1000U;
    status.sampleRate = 44100U;
    status.slices = {{1U, 0U, 400U}, {2U, 400U, 1000U}};
    status.links = {{5U, 2U}, {0U, 1U}};
    status.affectedMask = 0x21U;
    status.text = "song; final.wav";
    MainSampleStatus decoded;
    check(decodeMainSampleStatus(encodeMainSampleStatus(status), decoded) &&
          decoded.sequence == 12U && decoded.state == MainSampleState::ready &&
          decoded.sourceId == status.sourceId && decoded.slices == status.slices &&
          decoded.links == status.links && decoded.affectedMask == 0x21U &&
          decoded.text == status.text, "a status round-trips with ';' in the name");
    MainSampleStatus empty;
    check(decodeMainSampleStatus(encodeMainSampleStatus(empty), decoded) &&
          decoded.state == MainSampleState::empty && decoded.slices.empty() &&
          decoded.links.empty() && decoded.text.empty(), "an empty status round-trips");

    const auto worst = encodeMainSampleStatus(worstStatus());
    check(worst.size() < 8191U, "the worst-case status fits a UI message");
    check(decodeMainSampleStatus(worst, decoded) && decoded.slices.size() == 64U &&
          decoded.links.size() == 64U && decoded.text.size() == kMainSampleTextBytes,
          "the worst-case status decodes with its text cut to 512 bytes");

    for (const char* bad : {"MS1;1;READY;1;100;1:0:50;3:2;0;x", // unknown slice
                            "MS1;1;READY;1;100;1:0:50;3:1,3:1;0;x", // duplicate pad
                            "MS1;1;READY;1;100;1:0:150;;0;x", // slice past the end
                            "MS1;1;DONE;1;100;;;0;x", "MS1;1;READY;1;100;;;0",
                            "MS1;1;READY;1000000000000;100;;;0;x", "MS1;1;READY;1;100;;64:1;0;x"})
        check(!decodeMainSampleStatus(bad, decoded), bad);
}

void waveforms()
{
    MainSampleWaveform waveform;
    waveform.sequence = 3U;
    waveform.start = 0U;
    waveform.end = 184'320'000U;
    waveform.count = 4U;
    for (std::uint32_t tile = 0; tile < 4U; ++tile) {
        waveform.tiles[tile].frames = mainSampleTileFrames(0U, 184'320'000U, 4U, tile);
        waveform.tiles[tile].sampleRate = 96000.0;
        waveform.tiles[tile].minimum.fill(-0.5f);
        waveform.tiles[tile].maximum.fill(0.5f);
    }
    const auto encoded = encodeMainSampleWaveform(waveform);
    check(encoded.size() < 8191U, "four worst-case tiles fit a UI message");
    MainSampleWaveform decoded;
    check(decodeMainSampleWaveform(encoded, decoded) && decoded.count == 4U &&
          decoded.tiles[3].frames == waveform.tiles[3].frames &&
          decoded.tiles[2].maximum[7] > 0.49f, "waveform tiles round-trip");
    waveform.count = 3U;
    check(!decodeMainSampleWaveform(encodeMainSampleWaveform(waveform), decoded),
          "tiles must split the range exactly");
    check(!decodeMainSampleWaveform(encoded.substr(0U, encoded.rfind(";WS2;")), decoded),
          "a missing tile is refused");
    check(!decodeMainSampleWaveform(encoded + ";WS2;0;1;2;", decoded), "an extra tile is refused");
    check(!decodeMainSampleWaveform("MW1;1;0;10;1", decoded), "tiles are required");
}

void previews()
{
    const SourcePreviewRequest play{true, true, 0x123456789abcULL, 10U, 4000U};
    SourcePreviewRequest decoded;
    check(decodeSourcePreviewRequest(encodeSourcePreviewRequest(play), decoded) && decoded.play &&
          decoded.loop && decoded.sourceId == play.sourceId && decoded.start == 10U &&
          decoded.end == 4000U, "a play request round-trips");
    check(decodeSourcePreviewRequest(encodeSourcePreviewRequest({}), decoded) && !decoded.play,
          "a stop needs no range");
    for (const char* bad : {"MP1;1;0;0;0;10", "MP1;1;0;5;10;10", "MP1;2;0;5;0;10",
                            "MP1;0;0;0;0;0;", "MP1;0;0;0;0", "MP1;0;0;1000000000000;0;0"})
        check(!decodeSourcePreviewRequest(bad, decoded), bad);
}

void sessions()
{
    MainSampleSession session;
    check(encodeMainSampleSession(session).empty(), "an empty session encodes as clear");
    // 127 ASCII bytes then a two-byte character: the cut must not split it.
    session.reset(0xfeedULL, 48000U, 48000U, std::string(127U, 'a') + "\xC3\xA9;x");
    check(session.name == std::string(127U, 'a'), "names are cut on a UTF-8 boundary");
    session.reset(0xfeedULL, 48000U, 48000U, "drums; take 2 \xE2\x82\xAC");
    MainSampleSlice first, second, third;
    check(session.addSlice(1000U, 2000U, first) && session.addSlice(0U, 1000U, second) &&
          first.id == 1U && second.id == 2U && session.slices.front().id == 2U,
          "new slices get fresh ids and stay sorted");
    check(!session.addSlice(10U, 10U, third) && !session.addSlice(0U, 48001U, third),
          "bad slice ranges are refused");
    const std::vector<MainSampleSlice> bad{{1U, 900U, 2000U}, {2U, 0U, 48001U}};
    check(!session.editSlices(bad) && session.find(1U)->start == 1000U,
          "a two-slice edit is all-or-nothing");
    const std::vector<MainSampleSlice> moved{{1U, 900U, 2000U}, {2U, 0U, 900U}};
    check(session.editSlices(moved) && session.find(2U)->end == 900U, "a shared boundary moves");

    MainSampleSession decoded;
    check(decodeMainSampleSession(encodeMainSampleSession(session), decoded) &&
          decoded.sourceId == 0xfeedULL && decoded.frames == 48000U &&
          decoded.nextSliceId == 3U && decoded.slices == session.slices &&
          decoded.name == session.name, "MSS1 round-trips with ';' and UTF-8 in the name");
    check(session.removeSlice(1U) && !session.removeSlice(1U) && session.slices.size() == 1U,
          "slices can be removed once");

    session.nextSliceId = 65535U;
    check(session.addSlice(0U, 5U, third) && third.id == 65535U && session.nextSliceId == 1U &&
          session.addSlice(5U, 9U, third) && third.id == 1U, "ids wrap past 65535");
    check(session.addSlice(9U, 12U, third) && third.id == 3U, "ids in use are skipped");
    while (session.slices.size() < kMainSampleMaxSlices)
        check(session.addSlice(0U, 1U, third), "fill the session");
    check(!session.addSlice(0U, 1U, third), "a session holds at most 64 slices");

    for (const char* bad : {"MSS1;0;100;48000;1;;x", "MSS1;1;0;48000;1;;x", "MSS1;1;100;1;1;;x",
                            "MSS1;1;100;48000;0;;x", "MSS1;1;100;48000;1;2:50:60,1:0:10;x",
                            "MSS1;1;100;48000;1;1:0:200;x", "MSS1;1;100;48000;1;", "MSS1;1;100"})
        check(!decodeMainSampleSession(bad, decoded), bad);
}

void links()
{
    const std::vector<MainSampleLinkRecord> records{
        {0U, (0xfeedULL << 16U) | 1U, 1000U, 0x0123456789abcdefULL},
        {63U, (0xfeedULL << 16U) | 2U, 7U, 0U}};
    std::vector<MainSampleLinkRecord> decoded;
    check(decodeMainSampleLinks(encodeMainSampleLinks(records), decoded) && decoded == records,
          "LK1 round-trips");
    check(decodeMainSampleLinks("LK1;", decoded) && decoded.empty(), "LK1 may hold no links");
    for (const char* bad : {"LK1;0:1:5:9,0:2:5:9", "LK1;0:0:5:9", "LK1;64:1:5:9", "LK1;0:1:0:9",
                            "LK1;0:1:5:9,", "LK1;0:1:5", "LK1;0:1:5:9:1", "LK0;0:1:5:9"})
        check(!decodeMainSampleLinks(bad, decoded), bad);
    const auto a = makeMainSampleSourceId(1U);
    const auto b = makeMainSampleSourceId(2U);
    check(a != 0U && b != 0U && a != b && a <= kMainSampleSourceIdMask &&
          b <= kMainSampleSourceIdMask, "source ids are distinct nonzero 48-bit values");
}

} // namespace

void autoChop()
{
    AutoChopRequest r;
    r.sequence = 9U;
    r.firstPad = 3U;
    r.count = 12U;
    r.confirm = true;
    r.rangeStart = 100U;
    r.rangeEnd = 90000U;
    for (const auto mode : {AutoChopMode::transient, AutoChopMode::findSamples, AutoChopMode::equal,
                            AutoChopMode::beat, AutoChopMode::random, AutoChopMode::spectral}) {
        r.mode = mode;
        r.sensitivityPermille = 700U;
        r.minGapMs = 80U;
        r.seed = 123456789012ULL;
        r.bpmCenti = 12050U;
        r.beatsMilli = 500U;
        r.anchor = 777U;
        r.snap = true;
        AutoChopRequest d;
        const auto encoded = encodeAutoChopRequest(r);
        check(decodeAutoChopRequest(encoded, d) && encodeAutoChopRequest(d) == encoded &&
              d.mode == mode && d.firstPad == 3U && d.count == 12U && d.confirm &&
              d.rangeEnd == 90000U, "AC1 request round-trip");
    }
    for (const char* bad : {"", "AC1;", "AC2;1;E;0;4;0;0;0", "AC1;1;X;0;4;0;0;0", "AC1;1;E;64;4;0;0;0",
                            "AC1;1;E;0;0;0;0;0", "AC1;1;E;0;65;0;0;0", "AC1;1;E;0;4;2;0;0",
                            "AC1;1;E;0;4;0;50;40", "AC1;1;E;0;4;0;0;0;9", "AC1;1;T;0;4;0;0;0;1001;5", "AC1;1;S;0;4;0;0;0;500", "AC1;1;S;0;4;0;0;0;1001;5",
                            "AC1;1;T;0;4;0;0;0;500", "AC1;1;B;0;4;0;0;0;0;0;0;2", "AC1;1;B;0;4;0;0;0;0;0;0;0",
                            "AC1;1;R;0;4;0;0;0;12000;1000;5", "AC1;x;E;0;4;0;0;0"}) {
        AutoChopRequest d;
        check(!decodeAutoChopRequest(bad, d), "AC1 malformed request is refused");
    }
    AutoChopStatus s;
    s.sequence = 4U;
    s.state = AutoChopState::ok;
    s.mask = 0xf0f0ULL;
    s.found = 8U;
    s.requested = 12U;
    AutoChopStatus d;
    check(encodeAutoChopStatus(s) == "AC1;4;OK;f0f0;8;12" &&
          decodeAutoChopStatus(encodeAutoChopStatus(s), d) && d.mask == 0xf0f0ULL && d.found == 8U &&
          d.requested == 12U, "AC1 OK status round-trip");
    s.state = AutoChopState::confirm;
    check(decodeAutoChopStatus(encodeAutoChopStatus(s), d) && d.state == AutoChopState::confirm &&
          d.mask == 0xf0f0ULL, "AC1 CONFIRM status round-trip");
    s.state = AutoChopState::busy;
    check(encodeAutoChopStatus(s) == "AC1;4;BUSY" && decodeAutoChopStatus("AC1;4;BUSY", d) &&
          d.state == AutoChopState::busy, "AC1 BUSY status round-trip");
    s.state = AutoChopState::error;
    s.text = "No tempo";
    check(decodeAutoChopStatus(encodeAutoChopStatus(s), d) && d.text == "No tempo",
          "AC1 ERROR status round-trip");
    check(!decodeAutoChopStatus("AC1;4;OK;zz;1;1", d) && !decodeAutoChopStatus("AC1;4;MAYBE", d) &&
          !decodeAutoChopStatus("AC1;4;BUSY;1", d), "AC1 malformed status is refused");
}

void arrangement()
{
    // Pads 2, 5, 9 hold the slices at times 600, 0, 300: time order must be 5, 9, 2 -> pads 2, 5, 9.
    const std::vector<MainSampleSlice> slices{{1U, 600U, 800U}, {2U, 0U, 300U}, {3U, 300U, 600U}};
    const std::vector<MainSampleLink> links{{2U, 1U}, {5U, 2U}, {9U, 3U}};
    const auto moves = arrangeMoves(slices, links);
    check(moves.size() == 3U && moves[0] == ArrangeMove{5U, 2U} && moves[1] == ArrangeMove{9U, 5U} &&
          moves[2] == ArrangeMove{2U, 9U}, "arrange gives slices in time order the lowest used pads");
    std::vector<std::uint32_t> from;
    std::vector<std::uint32_t> to;
    for (const auto& move : moves) { from.push_back(move.from); to.push_back(move.to); }
    std::sort(from.begin(), from.end());
    std::sort(to.begin(), to.end());
    check(from == to, "arrange permutes the used pad set");
    check(arrangeMoves(slices, {{2U, 2U}, {5U, 3U}, {9U, 1U}}).empty(), "ordered pads need no moves");
    check(arrangeMoves(slices, {{7U, 99U}}).empty() && arrangeMoves(slices, {}).empty(),
          "unknown slices and empty links are ignored");
    const auto swapped = arrangeMoves(slices, {{4U, 2U}, {1U, 1U}});
    check(swapped.size() == 2U && swapped[0] == ArrangeMove{4U, 1U} && swapped[1] == ArrangeMove{1U, 4U},
          "unlinked pads stay out of the set and two pads can swap");
}

void spectrogramCodec()
{
    MainSampleSpectrogram s;
    s.sequence = 77U;
    s.start = 1000U;
    s.end = 5000000U;
    s.columns = static_cast<std::uint32_t>(kSpectrogramMaxColumns);
    s.data.resize(s.columns * kSpectrogramBins);
    for (std::size_t i = 0; i < s.data.size(); ++i)
        s.data[i] = static_cast<std::uint8_t>(i * 37U + 11U);  // every byte value, any padding
    const auto encoded = encodeMainSampleSpectrogram(s);
    MainSampleSpectrogram d;
    check(decodeMainSampleSpectrogram(encoded, d) && d.sequence == 77U && d.start == 1000U &&
          d.end == 5000000U && d.columns == s.columns && d.data == s.data, "MG1 round-trip");
    // Worst case: 120 columns, 10-digit numbers. The UI bus carries 8191 bytes per message.
    s.sequence = 18446744073709551615ULL;
    s.start = 4294967294U;
    s.end = 4294967295U;
    check(encodeMainSampleSpectrogram(s).size() < 8191U, "MG1 worst case fits the UI bus");
    check(encodeMainSampleSpectrogram(s).size() < 7800U, "MG1 worst case keeps headroom");
    for (std::size_t columns = 1; columns <= 3; ++columns) {  // Base64 padding for 1..3 byte tails
        MainSampleSpectrogram t;
        t.sequence = 1U; t.start = 0U; t.end = 10U; t.columns = static_cast<std::uint32_t>(columns);
        t.data.assign(columns * kSpectrogramBins - 1U, 7U);
        t.data.push_back(255U);
        MainSampleSpectrogram r;
        check(decodeMainSampleSpectrogram(encodeMainSampleSpectrogram(t), r) && r.data == t.data, "MG1 small round-trip");
    }
    for (const char* bad : {"", "MG1;", "MG2;1;0;10;1;AAAA", "MG1;1;10;10;1;AAAA", "MG1;1;0;10;0;AAAA",
                            "MG1;1;0;10;121;AAAA", "MG1;1;0;10;1;AAA", "MG1;1;0;10;1;AA*A",
                            "MG1;1;0;10;1;AAAA;x"}) {
        MainSampleSpectrogram r;
        check(!decodeMainSampleSpectrogram(bad, r), "MG1 malformed message is refused");
    }
    std::string wrongSize = encoded.substr(0U, encoded.size() - 4U);
    MainSampleSpectrogram r;
    check(!decodeMainSampleSpectrogram(wrongSize, r), "MG1 payload size must match columns x bins");
    MainSampleRequest request;
    request.sequence = 5U;
    request.verb = MainSampleVerb::spectrogram;
    request.start = 10U;
    request.end = 900U;
    request.tiles = 120U;
    MainSampleRequest decoded;
    check(decodeMainSampleRequest(encodeMainSampleRequest(request), decoded) &&
          decoded.verb == MainSampleVerb::spectrogram && decoded.start == 10U && decoded.end == 900U &&
          decoded.tiles == 120U, "MS1 G request round-trip");
    check(!decodeMainSampleRequest("MS1;5;G;10;900;121", decoded) && !decodeMainSampleRequest("MS1;5;G;10;10;5", decoded) &&
          !decodeMainSampleRequest("MS1;5;G;10;900;0", decoded), "bad G requests are refused");
}

int main()
{
    requests();
    arrangement();
    statuses();
    waveforms();
    previews();
    sessions();
    links();
    autoChop();
    spectrogramCodec();
    std::cout << "main sample protocol tests passed\n";
    return 0;
}

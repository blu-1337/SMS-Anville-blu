#include "FileDropController.hpp"
#include "Parameters.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace midichopper;
using namespace midichopper::plugin;
using namespace midichopper::ui;
using namespace std::chrono_literals;
using Clock = FileDropController::Clock;

void check(const bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

struct Host final : PadActionHost, ChopEditorHost, FileDropHost {
    std::vector<std::pair<std::string, std::string>> messages;
    std::string status;
    int repaints = 0;
    std::vector<std::string> released;
    std::vector<std::string> mainSampleLoads;
    PadActionController* actions = nullptr;

    void sendPadActionState(const char* key, const std::string& value) override
    { messages.emplace_back(key, value); }
    bool showPadFileBrowser(bool, const char*) override { return true; }
    void setPadActionStatus(const char* message) override { status = message; }
    void repaintPadActions() override {}
    void clearPadActionHover() noexcept override {}
    void beginPadSampleEditor(int) override {}
    void refreshPadWaveform() override {}
    void setPadColor(int, int) override {}

    void sendChopState(const char* key, const std::string& value) override
    { sendPadActionState(key, value); }
    void selectChopPad(int) override {}
    void leaveChopEditor() override {}
    void resetChopViewport(std::uint64_t) override {}
    void setChopStatus(const char* message) override { status = message; }
    void setChopStructureBusy(const bool busy) override { actions->setStructureBusy(busy); }
    void repaintChopEditor() override {}

    void setFileDropStatus(const char* message) override { status = message; }
    void repaintFileDrop() override { ++repaints; }
    void releaseDroppedFile(const std::string& path) override { released.push_back(path); }
    bool loadDroppedMainSample(const std::string& path) override { mainSampleLoads.push_back(path); return true; }
};

struct Fixture {
    Host host;
    ChopEditorController chop{host};
    PadActionController actions{host, chop, 9};
    FileDropController drop{host};
    std::array<char, kPadsPerBank> pads{};
    DropContext context;
    bool alternate = false;
    Clock::time_point now{};

    Fixture()
    {
        host.actions = &actions;
        pads.fill('0');
        context.pads.padState = pads;
    }
    // Bank and layout as SamplerUiSession projects them.
    void view(const int bank, const int visibleCount, const MidiBankMode mode)
    {
        context.pads.visibleCount = visibleCount;
        context.pads.bankMode = mode;
        context.pads.firstPad = bank * static_cast<int>(
            bankStride(static_cast<std::uint8_t>(visibleCount), mode));
    }
    [[nodiscard]] DropTarget local(const int pad) const
    { return FileDropController::padTarget(context.pads, pad); }
    void dropFiles(const int localPad, std::vector<std::string> paths)
    { drop.enqueue(local(localPad), std::move(paths), context, now); }
    void idle() { drop.idle(actions, context.pads, now); }
    // The pad of the request sent by the last idle, or kPadCount when none was sent.
    [[nodiscard]] std::uint32_t sentPad(const std::size_t before) const
    {
        if (host.messages.size() == before)
            return kPadCount;
        PadFileRequest request;
        check(host.messages.back().first == "pad_file_request" &&
              decodePadFileRequest(host.messages.back().second, request) &&
              request.action == PadFileAction::import, "drop sends an import request");
        return request.pad;
    }
    void complete(const PadFileResultCode result)
    {
        check(actions.acceptParameter(context.pads, kParameterPadFileResultEvent,
                                      padFileResultEventValue(result, alternate)),
              "result event was handled");
        alternate = !alternate;
    }
    // Runs the FIFO to the end and returns the pads imported, in order.
    std::vector<std::uint32_t> drain(const PadFileResultCode result = PadFileResultCode::importSucceeded)
    {
        std::vector<std::uint32_t> imported;
        for (int guard = 0; guard < 200; ++guard) {
            const auto before = host.messages.size();
            idle();
            const auto pad = sentPad(before);
            if (pad == kPadCount) {
                if (!drop.active())
                    break;
                continue;
            }
            imported.push_back(pad);
            complete(result);
        }
        idle();
        return imported;
    }
};

[[nodiscard]] std::vector<std::string> files(const int count, const char* prefix = "C:\\Drops\\take")
{
    std::vector<std::string> paths;
    for (int index = 0; index < count; ++index)
        paths.push_back(std::string(prefix) + std::to_string(index) + ".wav");
    return paths;
}

void acceptAndReject()
{
    Fixture f;
    f.view(1, 16, MidiBankMode::SelectedBank);
    check(f.local(3).kind == DropTarget::Kind::pad && f.local(3).globalPad == 19U,
          "a visible pad maps through the current bank");
    check(f.local(16).kind == DropTarget::Kind::none && f.local(-1).kind == DropTarget::Kind::none,
          "points outside the visible grid are not pads");
    const auto wav = files(1);
    check(f.drop.hover(f.local(3), wav, f.context) && f.drop.highlightedPad() == 19U &&
          f.host.repaints == 1, "a WAV over a pad is accepted and highlighted");
    check(f.drop.hover(f.local(3), wav, f.context) && f.host.repaints == 1,
          "an unchanged highlight does not repaint");

    f.view(1, 12, MidiBankMode::AllBanks);
    check(f.drop.hover(f.local(3), wav, f.context) && f.drop.highlightedPad() == 15U,
          "the highlight follows the 12-pad layout across banks");
    f.view(1, 12, MidiBankMode::SelectedBank);
    check(f.drop.hover(f.local(3), wav, f.context) && f.drop.highlightedPad() == 19U,
          "selected-bank mode keeps the 16-pad page stride");
    check(f.local(12).kind == DropTarget::Kind::none, "hidden layout slots are not targets");

    const std::vector<std::string> mp3{"C:\\Drops\\song.mp3"};
    check(!f.drop.hover(f.local(3), mp3, f.context) && !f.drop.highlightedPad(),
          "no supported file, no drop");
    check(!f.drop.hover({}, wav, f.context), "a point off the pads is rejected");
    const auto rejectedWhen = [&](auto&& change, const char* message) {
        Fixture g;
        change(g.context);
        check(!g.drop.hover(g.local(0), wav, g.context) && !g.drop.highlightedPad(), message);
    };
    rejectedWhen([](DropContext& c) { c.pads.armed = true; }, "armed rejects drops");
    rejectedWhen([](DropContext& c) { c.captureActive = true; }, "an active capture rejects drops");
    rejectedWhen([](DropContext& c) { c.chopEditorActive = true; }, "the cut editor rejects drops");
    rejectedWhen([](DropContext& c) { c.padActionsBusy = true; }, "a busy pad action rejects drops");

    check(f.drop.hover(f.local(3), wav, f.context), "hover again");
    f.drop.leave();
    check(!f.drop.highlightedPad(), "leaving clears the highlight");

    f.context.pads.armed = true;
    f.dropFiles(3, files(2));
    check(f.host.status == "Disarm recording to drop files" && f.host.released.size() == 2U &&
          !f.drop.active(), "a drop that became invalid releases its files and explains why");
}

void consecutiveVisiblePads()
{
    Fixture f;
    f.view(0, 16, MidiBankMode::SelectedBank);
    f.dropFiles(14, files(3));
    check(f.drop.confirmationPending() && !f.drop.active(),
          "a pad in another bank has unknown occupancy, so the batch asks first");
    f.dropFiles(14, files(3));
    check(f.drain() == std::vector<std::uint32_t>{14U, 15U, 16U},
          "three files on local pad 14 fill 14, 15, then the next bank's pad 0");
    check(f.host.status == "Loaded 3 files" && f.host.released.size() == 6U,
          "the batch reports its count and releases every path, including the replaced batch");

    Fixture hidden;
    hidden.view(0, 12, MidiBankMode::SelectedBank);
    hidden.dropFiles(10, files(3));
    hidden.dropFiles(10, files(3));
    check(hidden.drain() == std::vector<std::uint32_t>{10U, 11U, 16U},
          "a 12-pad layout skips the hidden slots 12-15");

    Fixture visible;
    visible.view(0, 16, MidiBankMode::SelectedBank);
    visible.dropFiles(0, files(2));
    check(visible.drop.active() && !visible.drop.confirmationPending() &&
          visible.host.status == "Loading 2 files...",
          "empty visible pads load without confirmation");
    check(visible.drain() == std::vector<std::uint32_t>{0U, 1U}, "two files fill pads 0 and 1");

    Fixture single;
    single.pads[5] = '1';
    single.dropFiles(5, {"C:\\Drops\\kick.wav", "C:\\Drops\\notes.txt"});
    check(single.drop.active() && single.host.released == std::vector<std::string>{"C:\\Drops\\notes.txt"},
          "one WAV replaces its pad without confirmation; other files are released at once");
    check(single.drain() == std::vector<std::uint32_t>{5U} && single.host.status == "WAV imported",
          "a single file keeps the import status");
}

void fifoIsGatedAndAdvancesOnFailure()
{
    Fixture f;
    f.dropFiles(0, files(3));
    auto before = f.host.messages.size();
    f.idle();
    check(f.sentPad(before) == 0U && f.actions.importInFlight(), "the first file is sent");
    before = f.host.messages.size();
    f.idle();
    f.idle();
    check(f.sentPad(before) == kPadCount, "nothing more is sent while an import is in flight");
    f.complete(PadFileResultCode::failed);
    f.actions.setStructureBusy(true);
    before = f.host.messages.size();
    f.idle();
    check(f.sentPad(before) == kPadCount && f.host.released.size() == 1U,
          "a completed file is released, but another busy action holds the queue");
    f.actions.setStructureBusy(false);
    before = f.host.messages.size();
    f.idle();
    check(f.sentPad(before) == 1U, "a failed import still advances the queue");
    f.complete(PadFileResultCode::importSucceeded);
    check(f.drain() == std::vector<std::uint32_t>{2U}, "the last file follows");
    check(f.host.status == "Loaded 2 of 3 files", "the summary counts successful imports");
    check(!f.drop.active() && f.host.released.size() == 3U, "every path is released once");

    // The plugin sends busy=0 before the result event; a late busy=0 from the
    // previous file must not release the next file early.
    Fixture late;
    late.dropFiles(0, files(3));
    before = late.host.messages.size();
    late.idle();
    check(late.sentPad(before) == 0U, "first file sent");
    late.complete(PadFileResultCode::importSucceeded);
    before = late.host.messages.size();
    late.idle();
    check(late.sentPad(before) == 1U, "second file sent");
    check(late.actions.acceptState(late.context.pads, "pad_file_busy", "0") && !late.actions.busy(),
          "a late busy flag clears the file busy state");
    before = late.host.messages.size();
    late.idle();
    check(late.sentPad(before) == kPadCount && late.actions.importInFlight(),
          "the queue still waits for the in-flight import's result");
}

void overflowAndCap()
{
    Fixture f;
    f.view(3, 16, MidiBankMode::SelectedBank);
    f.dropFiles(15, files(3));
    check(f.host.released.size() == 2U, "files past the last pad are released at once");
    check(f.drain() == std::vector<std::uint32_t>{63U} && f.host.status == "Loaded 1 of 3 files",
          "overflow past the last pad reports Loaded k of n");

    Fixture many;
    many.dropFiles(0, files(70));
    many.dropFiles(0, files(70));
    const auto imported = many.drain();
    check(imported.size() == kPadCount && imported.back() == kPadCount - 1U,
          "at most 64 files are imported");
    check(many.host.status == "Loaded 64 of 70 files" && many.host.released.size() == 140U,
          "the cap is reported and every path is released");
}

void confirmation()
{
    Fixture f;
    f.pads[1] = '1';
    f.dropFiles(0, files(2, "C:\\A\\"));
    check(f.drop.confirmationPending() &&
          f.host.status == "Pads in use: drop the same files again within 5 s to replace",
          "replacing another occupied pad asks first");
    auto before = f.host.messages.size();
    f.idle();
    check(f.sentPad(before) == kPadCount, "nothing loads before confirmation");
    f.now += 3s;
    f.dropFiles(0, files(2, "C:\\B\\"));
    check(!f.drop.confirmationPending() && f.drop.active() &&
          f.host.released == std::vector<std::string>{"C:\\A\\0.wav", "C:\\A\\1.wav"},
          "a second matching drop confirms and releases the first drop's files");
    check(f.drain() == std::vector<std::uint32_t>{0U, 1U}, "the confirmed batch loads");

    Fixture expiry;
    expiry.pads[1] = '1';
    expiry.dropFiles(0, files(2));
    expiry.now += 6s;
    expiry.idle();
    check(!expiry.drop.confirmationPending() && expiry.host.released.size() == 2U &&
          expiry.host.status == "File drop cancelled", "an unconfirmed batch expires");
    expiry.dropFiles(0, files(2));
    check(expiry.drop.confirmationPending(), "an expired confirmation asks again");

    Fixture other;
    other.pads[1] = '1';
    other.pads[2] = '1';
    other.dropFiles(0, files(2));
    other.dropFiles(1, files(2));
    check(other.drop.confirmationPending() && other.host.released.size() == 2U,
          "a drop on another pad asks again instead of confirming");
    other.dropFiles(1, files(3));
    check(other.drop.confirmationPending() && other.host.released.size() == 4U,
          "a different file count asks again");
}

void armingStopsTheQueue()
{
    Fixture f;
    f.dropFiles(0, files(3));
    auto before = f.host.messages.size();
    f.idle();
    check(f.sentPad(before) == 0U, "first file sent");
    f.complete(PadFileResultCode::importSucceeded);
    f.context.pads.armed = true;
    f.idle();
    check(!f.drop.active() && f.host.released.size() == 3U && f.host.status == "Loaded 1 of 3 files",
          "arming stops the remaining files");
}

void cancelAndDestruction()
{
    Fixture f;
    f.pads[1] = '1';
    f.dropFiles(0, files(2, "C:\\P\\"));
    f.drop.cancel();
    check(f.host.released.size() == 2U && !f.drop.confirmationPending(),
          "cancel releases a pending batch");

    Host host;
    ChopEditorController chop{host};
    PadActionController actions{host, chop, 9};
    host.actions = &actions;
    std::array<char, kPadsPerBank> pads{};
    pads.fill('0');
    DropContext context;
    context.pads.padState = pads;
    auto drop = std::make_unique<FileDropController>(host);
    drop->enqueue(FileDropController::padTarget(context.pads, 0), files(3, "C:\\Q\\"), context);
    drop->idle(actions, context.pads);
    check(actions.importInFlight(), "first file in flight");
    drop.reset();
    const auto count = [&](const char* path) {
        return std::count(host.released.begin(), host.released.end(), std::string(path));
    };
    check(count("C:\\Q\\0.wav") == 0 && count("C:\\Q\\1.wav") == 1 && count("C:\\Q\\2.wav") == 1,
          "destruction releases queued files and leaves the in-flight file to the stale sweep");
}

} // namespace

void mainSampleTarget()
{
    Fixture f;
    const DropTarget target{DropTarget::Kind::mainSample, kPadCount};
    const std::vector<std::string> one{"C:/drop/a.wav"};
    check(f.drop.hover(target, one, f.context) && !f.drop.highlightedPad(),
          "a Main Sample target accepts WAV files without a pad highlight");
    const std::vector<std::string> text{"C:/drop/a.txt"};
    check(!f.drop.hover(target, text, f.context), "a Main Sample target rejects non-WAV files");

    f.drop.enqueue(target, {"C:/drop/a.wav", "C:/drop/b.wav", "C:/drop/c.txt"}, f.context, f.now);
    check(f.host.mainSampleLoads.size() == 1U && f.host.mainSampleLoads[0] == "C:/drop/a.wav",
          "only the first file loads");
    check(f.host.status == "Loading the first dropped file only", "the status says only one file is used");
    check(f.host.released.size() == 2U, "unused files are released at once");
    check(f.drop.mainSampleLoadPending() && !f.drop.hover(target, one, f.context),
          "a second drop waits for the load");
    f.drop.mainSampleSettled();
    check(!f.drop.mainSampleLoadPending() && f.host.released.back() == "C:/drop/a.wav",
          "the loaded file is released once the load settles");
    check(!f.drop.active(), "no pad import was queued");
}

int main()
{
    mainSampleTarget();
    acceptAndReject();
    consecutiveVisiblePads();
    fifoIsGatedAndAdvancesOnFailure();
    overflowAndCap();
    confirmation();
    armingStopsTheQueue();
    cancelAndDestruction();
    std::cout << "File drop controller tests passed\n";
}

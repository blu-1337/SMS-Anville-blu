#include "FileDropEffect.hpp"
#include "FileDropTarget.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>

namespace {
using namespace sms::ui;

void check(const bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

// Every allowed mask from the drag source, with and without acceptance.
void effectChooser()
{
    struct Case {
        std::uint32_t allowed;
        std::uint32_t expected;
    };
    constexpr std::array cases{
        Case{kDropNone, kDropNone},
        Case{kDropCopy, kDropCopy},
        Case{kDropMove, kDropMove},
        Case{kDropCopy | kDropMove, kDropCopy},
        Case{kDropLink, kDropLink},
        Case{kDropCopy | kDropLink, kDropCopy},
        Case{kDropMove | kDropLink, kDropMove},
        Case{kDropCopy | kDropMove | kDropLink, kDropCopy},
    };
    for (const auto& entry : cases) {
        check(chooseDropEffect(entry.allowed, true) == entry.expected,
              "accepted drops choose COPY, then MOVE, then LINK from the allowed mask");
        check(chooseDropEffect(entry.allowed, false) == kDropNone,
              "a rejected drop never reports an effect");
        check((chooseDropEffect(entry.allowed, true) & ~entry.allowed) == 0U,
              "the chosen effect is always one the source allows");
    }
    static_assert(chooseDropEffect(kDropMove, true) == kDropMove,
                  "the Edison case: a MOVE-only source is accepted as MOVE, never as COPY");
    check(chooseDropEffect(0x80000000U | kDropMove, true) == kDropMove,
          "unknown source bits (DROPEFFECT_SCROLL) are ignored");

    check(reportedDropEffect(kDropCopy) == kDropCopy, "a copy is reported as COPY");
    check(reportedDropEffect(kDropMove) == kDropNone,
          "a move is reported as NONE so the source never deletes its original");
    check(reportedDropEffect(kDropLink) == kDropNone && reportedDropEffect(kDropNone) == kDropNone,
          "link and none are reported as NONE");
    check(!dropNeedsPrivateCopy(kDropCopy) && dropNeedsPrivateCopy(kDropMove) &&
          dropNeedsPrivateCopy(kDropLink) && !dropNeedsPrivateCopy(kDropNone),
          "only MOVE and LINK sources are copied before the asynchronous import");
}

void extensionFilter()
{
    check(isSupportedAudioPath("C:\\Samples\\kick.wav"), "lowercase .wav");
    check(isSupportedAudioPath("/tmp/KICK.WAV"), "uppercase .WAV");
    check(isSupportedAudioPath("C:\\Samples\\loop.Wave"), "mixed-case .wave");
    check(isSupportedAudioPath("C:\\Samples\\音源.wav"), "non-ASCII name");
    check(!isSupportedAudioPath("C:\\Samples\\song.mp3"), "mp3 is rejected");
    check(!isSupportedAudioPath("C:\\Samples\\wav"), "no dot");
    check(!isSupportedAudioPath("C:\\Samples\\.wav"), "a bare extension is not a file name");
    check(!isSupportedAudioPath("/tmp/.wav"), "a bare extension after a slash is not a file name");
    check(!isSupportedAudioPath(".wav") && !isSupportedAudioPath(""), "empty or bare names");
    check(!isSupportedAudioPath("C:\\Samples\\kick.wav.txt"), "only the final extension counts");
    check(!isSupportedAudioPath("C:\\Samples\\kick.wa"), "truncated extension");
}

void nullTargetIsSafe()
{
#if !defined(_WIN32)
    struct Listener final : FileDropListener {
        bool fileDragOver(FileDropPoint, std::span<const std::string>) override { return true; }
        void fileDragLeave() override {}
        void filesDropped(FileDropPoint, std::span<const std::string>) override {}
    } listener;
    check(FileDropTarget::attach(1U, listener) == nullptr,
          "platforms without native drop support report no target");
#endif
    FileDropTarget::releaseDroppedFile("");
    FileDropTarget::releaseDroppedFile("C:\\not-a-private-copy.wav");
}

} // namespace

int main()
{
    effectChooser();
    extensionFilter();
    nullTargetIsSafe();
    std::cout << "File drop tests passed\n";
}

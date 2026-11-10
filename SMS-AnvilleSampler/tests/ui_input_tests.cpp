#include "SamplerInputController.hpp"
#include "EditorSnapshotProtocol.hpp"
#include "StateKeys.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace midichopper;
using namespace midichopper::plugin;
using namespace midichopper::ui;

void check(const bool condition, const char* message)
{
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(EXIT_FAILURE); }
}

sms::ui::Point center(const sms::ui::Rect rect)
{ return {rect.x + rect.width * 0.5f, rect.y + rect.height * 0.5f}; }

struct Fixture final : SamplerUiHost, MixerInteractionHost, ApplicationMenuHost, SamplerInputHost {
    std::vector<std::pair<std::string, std::string>> states;
    std::vector<std::pair<int, int>> notes;
    std::vector<std::pair<std::uint32_t, bool>> gestures;
    std::vector<std::pair<std::uint32_t, float>> parameters;
    bool ignoringRepeat = false;
    SamplerUiSession session{*this};
    MixerInteractionController mixer{*this};
    ApplicationMenuController menu{*this, "main"};
    SamplerInputController input{*this, session, mixer, menu, 9};

    void sendUiParameter(std::uint32_t parameter, float value) override { parameters.emplace_back(parameter, value); }
    void sendUiState(const char* key, const std::string& value) override { states.emplace_back(key, value); }
    void sendUiNote(std::uint8_t note, std::uint8_t velocity) override { notes.emplace_back(note, velocity); }
    void repaintUi() override {}
    bool showUiPadFileBrowser(bool, const char*) override { return true; }
    void invalidateUiInteraction(UiInteractionInvalidation reason) override { input.invalidate(reason); }
    void editMixerParameter(std::uint32_t p, bool begin) override { gestures.emplace_back(p, begin); }
    void setMixerParameter(std::uint32_t p, float value) override { session.setControlValue(p, value); }
    void commitPadMixer(int pad, const sms::dsp::SampleMixerSettings& settings) override
    { check(pad == session.state().selectedPad, "mixer commits only the current pad"); session.commitMixerSettings(settings); }
    void saveGlobalMixerSettings(std::uint32_t, const GlobalValueSettings&) override {}
    void setMixerStatus(const char* text) override { session.setLocalStatus(text); }
    void repaintMixer() override {}
    void focusMixer() override {}
    void clearMixerHover() noexcept override { input.clearHover(); }
    void chooseLayout(int layout) override { session.selectLayout(layout); }
    void chooseMidiBankMode(int mode) override { session.selectMidiBankMode(mode); }
    bool chooseSkin(std::size_t) override { return true; }
    void clearAllPads() override { session.clearAllPadContents(); }
    void setStatus(const char* text) override { session.setLocalStatus(text); }
    void repaintApplicationMenu() override {}
    void clearHover() noexcept override { input.clearHover(); }
    bool openLink(const char*) override { return true; }
    void openMainSample() override { session.openMainSample(); }
    void focusSamplerInput() override {}
    void ignoreSamplerKeyRepeat(bool ignore) override { ignoringRepeat = ignore; }
    void editSamplerParameter(std::uint32_t p, bool begin) override { gestures.emplace_back(p, begin); }
    void repaintSamplerInput() override {}
    ApplicationMenuContext applicationMenuContext() const override { return {true, 4, 0}; }

    void click(const sms::ui::Point p, const InputButton button = InputButton::left)
    { check(input.mouse(p, button, true), "press was handled"); input.mouse(p, button, false); }
    void readyEditor()
    {
        session.acceptState("pad_mask", "1111111111111111");
        session.openSampleEditor(0);
        const auto request = std::find_if(states.rbegin(), states.rend(), [](const auto& state) {
            return state.first == "waveform_request";
        });
        check(request != states.rend(), "opening editor requests its snapshot");
        EditorSnapshotReply reply;
        check(decodeEditorSnapshotRequest(request->second, reply.request), "snapshot request decodes");
        reply.waveform.pad = 0;
        reply.waveform.frames = 48000;
        reply.waveform.sampleRate = 48000;
        reply.playback.start = 0.1f;
        reply.playback.end = 0.8f;
        check(session.acceptState("waveform_data", encodeEditorSnapshotReply(reply).c_str()), "snapshot accepted");
    }
};

void padOwnershipAndSpace()
{
    Fixture f;
    const auto pad = center(f.session.padLayout().grid(layout::mainPadBounds, 10.0f).cell(
        f.session.padLayout().visualIndex(0)));
    check(f.input.mouse(pad, InputButton::left, true), "pad press handled");
    check(f.notes == std::vector<std::pair<int, int>>{{36, 127}}, "pad sends mapped note-on");
    f.session.acceptParameter(kParameterActiveBank, 2.0f);
    check(f.input.mouse({0, 0}, InputButton::left, false), "release captured across bank change");
    check(f.notes.back() == std::pair<int, int>{36, 0}, "release uses original MIDI note");

    f.session.acceptParameter(kParameterMode, 1.0f);
    f.notes.clear();
    f.input.keyboard(InputKey::space, true);
    f.input.keyboard(InputKey::space, true);
    check(f.ignoringRepeat && f.notes.size() == 2 && f.notes[0].second == 127 && f.notes[1].second == 0,
          "Space capture sends one balanced pair and suppresses repeat");
    f.input.focus(false);
    check(!f.ignoringRepeat, "focus loss restores repeat handling");
    f.input.keyboard(InputKey::space, true);
    check(f.notes.size() == 4, "Space can be pressed again after focus loss");
    f.input.keyboard(InputKey::space, false);
    f.session.acceptParameter(kParameterMode, 0.0f);

    f.click(center(layout::menuButton));
    const auto count = f.states.size();
    f.input.keyboard(InputKey::space, true);
    f.input.keyboard(InputKey::space, false);
    check(f.menu.open() && f.states.size() == count, "open application menu blocks Space");
    check(f.input.keyboard(InputKey::escape, true) && !f.menu.open(), "Escape closes application menu");
}

void overlaysAndFaderCapture()
{
    Fixture f;
    f.session.acceptState("pad_mask", "1000000000000000");
    const auto pad = center(f.session.padLayout().grid(layout::mainPadBounds, 10.0f).cell(
        f.session.padLayout().visualIndex(0)));
    f.click(pad, InputButton::right);
    check(f.session.padActions().menuOpen(), "right-click opens pad menu");
    const auto fader = center(layout::levelFader(0));
    f.click(fader);
    check(!f.session.padActions().menuOpen() && !f.mixer.faderDragging() && f.gestures.empty(),
          "pad overlay consumes outside click before underlying fader");
    check(f.input.mouse(fader, InputButton::left, true), "fader press handled after dismissal");
    check(f.mixer.faderDragging(), "fader captures the next press");
    f.input.motion({fader.x, fader.y + 20.0f});
    check(f.input.mouse({0, 0}, InputButton::left, false), "fader release captured outside its bounds");
    check(!f.mixer.faderDragging() && f.gestures.front() == std::pair<std::uint32_t, bool>{kParameterMonitorGainDb, true} &&
          f.gestures.back() == std::pair<std::uint32_t, bool>{kParameterMonitorGainDb, false},
          "fader host edit is balanced");
    f.click(pad, InputButton::right);
    check(f.input.keyboard(InputKey::escape, true) && !f.session.padActions().menuOpen(),
          "Escape closes the pad overlay");
}

void editorWheelAndPointerRoutes()
{
    Fixture f;
    f.readyEditor();
    const auto decay = center(layout::editorSlider(1));
    check(f.input.scroll(decay, 1.0f, false, false), "envelope wheel handled");
    check(f.session.state().editorSettings.decaySeconds == 0.01f, "wheel commits edited ADSR");
    f.click(decay, InputButton::middle);
    check(f.session.state().editorSettings.decaySeconds == 0.0f, "middle press resets envelope slider");

    const auto& waveform = layout::editorWaveform;
    const auto start = sms::ui::Point{waveform.x + waveform.width * 0.1f, waveform.y + 10.0f};
    check(isTarget(f.input.targetAt(start), InteractiveType::regionHandle), "start marker resolves correctly");
    check(f.input.mouse(start, InputButton::left, true), "region marker press handled");
    f.input.motion({waveform.x + waveform.width * 0.25f, start.y});
    f.input.mouse({0, 0}, InputButton::left, false);
    check(std::abs(f.session.state().editorSettings.start - 0.25f) < 1.0f / 48000.0f,
          "region pointer motion commits local sample coordinates");

    const auto knob = center(layout::mixerKnob(0));
    f.input.mouse(knob, InputButton::left, true);
    f.input.mouse(knob, InputButton::left, false);
    f.input.mouse(knob, InputButton::left, true);
    check(f.mixer.entry().target().valid(), "knob double-click starts numeric entry");
    f.input.mouse(knob, InputButton::left, false);
    check(f.input.keyboard(InputKey::deleteText, true), "numeric Delete consumed");
    f.input.character('-'); f.input.character('6');
    check(f.input.keyboard(InputKey::commit, true), "numeric commit consumed");
    check(f.session.state().mixerSettings.gainDecibels == -6.0f && !f.mixer.entry().target().valid(),
          "numeric commit reaches session settings");
}
} // namespace

std::vector<MainSampleRequest> mainSampleRequests(const Fixture& f)
{
    std::vector<MainSampleRequest> result;
    for (const auto& [key, value] : f.states) {
        MainSampleRequest request;
        if (key == kMainSampleRequestKey && decodeMainSampleRequest(value, request))
            result.push_back(request);
    }
    return result;
}

std::size_t mainSampleVerbs(const Fixture& f, const MainSampleVerb verb)
{
    const auto requests = mainSampleRequests(f);
    return static_cast<std::size_t>(std::count_if(requests.begin(), requests.end(),
        [&](const MainSampleRequest& request) { return request.verb == verb; }));
}

void mainSampleViewRoutes()
{
    Fixture f;
    f.session.acceptState("pad_mask", "1111111111111111");
    check(!f.session.state().mainSampleMode, "the main view is the default");
    check(isTarget(f.input.targetAt(center(layout::openMainSample)),
                                InteractiveType::mainSampleOpen), "SAMPLE button is a target");
    f.click(center(layout::openMainSample));
    check(f.session.state().mainSampleMode, "SAMPLE opens the Main Sample view");
    check(mainSampleVerbs(f, MainSampleVerb::query) >= 1U, "opening queries the session");

    MainSampleStatus status;
    status.sequence = f.session.mainSample().lastSentSequence();
    status.state = MainSampleState::ready;
    status.sourceId = 0xABCU;
    status.frames = 1000U;
    status.sampleRate = 48000U;
    status.slices = {{1U, 0U, 400U}, {2U, 400U, 1000U}};
    status.text = "loop.wav";
    f.session.acceptState(kMainSampleStatusKey, encodeMainSampleStatus(status).c_str());
    check(f.session.mainSample().hasSource(), "status mirrors the source");

    const auto waveformPoint = sms::ui::Point{700.0f, 260.0f};
    check(isTarget(f.input.targetAt(waveformPoint), InteractiveType::mainSampleWaveform),
          "empty waveform is a target");
    check(isTarget(f.input.targetAt(center(layout::mainSampleOverview)),
                                InteractiveType::mainSampleOverview), "overview is a target");
    check(isTarget(f.input.targetAt(center(layout::mainSampleLoad)),
                                InteractiveType::mainSampleLoad), "Load is a target");
    const sms::ui::Point shared{layout::mainSampleWaveform.x + layout::mainSampleWaveform.width * 0.4f, 260.0f};
    const auto marker = f.input.targetAt(shared);
    check(isTarget(marker, InteractiveType::mainSampleMarker, 2),
          "a shared boundary is one marker, the later slice's start edge");

    // A click on empty waveform previews from there to the end of its slice.
    f.click(waveformPoint);
    check(std::any_of(f.states.begin(), f.states.end(),
          [](const auto& state) { return state.first == kMainPreviewRequestKey; }),
          "clicking empty waveform previews");

    // One marker drag sends exactly one S, on mouse-up.
    const auto edits = mainSampleVerbs(f, MainSampleVerb::edit);
    check(f.input.mouse(shared, InputButton::left, true), "marker press handled");
    for (float dx = 4.0f; dx <= 60.0f; dx += 4.0f)
        f.input.motion({shared.x + dx, shared.y});
    check(mainSampleVerbs(f, MainSampleVerb::edit) == edits, "no S while dragging");
    f.input.mouse({shared.x + 60.0f, shared.y}, InputButton::left, false);
    check(mainSampleVerbs(f, MainSampleVerb::edit) == edits + 1U, "one S per drag");
    check(f.session.mainSample().selectedSlice() == 2U, "the dragged marker selects its slice");

    // Delete removes the selected slice (the controller's mirror still has slice 2).
    check(f.input.keyboard(InputKey::deleteText, true), "Delete handled");
    check(mainSampleVerbs(f, MainSampleVerb::remove) == 1U, "Delete sends D");

    // A drag on the waveform selects; releasing over a pad assigns it.
    f.input.mouse({100.0f, 240.0f}, InputButton::left, true);
    f.input.motion({200.0f, 240.0f});
    f.input.motion({300.0f, 240.0f});
    check(f.session.mainSample().hasSelection(), "dragging selects");
    const auto pad = center(f.session.padLayout().grid(layout::mainSamplePads, 8.0f).cell(
        f.session.padLayout().visualIndex(2)));
    f.input.mouse(pad, InputButton::left, false);
    check(mainSampleVerbs(f, MainSampleVerb::assign) == 1U && !f.session.mainSample().hasSelection(),
          "releasing a selection over a pad assigns it");

    // Space toggles the source preview instead of the pad play/stop.
    f.states.clear();
    f.input.keyboard(InputKey::space, true);
    f.input.keyboard(InputKey::space, false);
    check(!f.states.empty() && f.states.back().first == kMainPreviewRequestKey, "Space previews");

    // Double-clicking a pad opens no editor or menu: each click just auditions the pad.
    f.click(pad);
    f.click(pad);
    check(!f.session.state().editorMode && !f.session.padActions().menuOpen(),
          "pad double-click opens neither the Sample Editor nor a menu");

    // Right-clicking a pad opens the same pad context menu as the main view.
    check(f.input.mouse(pad, InputButton::right, true), "right-click on a Main Sample pad is handled");
    f.input.mouse(pad, InputButton::right, false);
    check(f.session.padActions().menuOpen(), "right-click opens the pad context menu");
    f.input.keyboard(InputKey::escape, true);
    check(!f.session.padActions().menuOpen() && f.session.state().mainSampleMode,
          "Escape closes the menu and stays in the Main Sample");

    check(f.input.keyboard(InputKey::escape, true) && !f.session.state().mainSampleMode,
          "Escape leaves the Main Sample view");
    check(!isTarget(f.input.targetAt(waveformPoint), InteractiveType::mainSampleWaveform),
          "main view targets return");
}

void applicationMenuOpensMainSample()
{
    Fixture f;
    f.click(center(layout::menuButton));
    check(isTarget(f.input.targetAt(center(layout::mainSampleMenuButton(true))),
                                InteractiveType::menuMainSample), "menu offers Main Sample");
    f.click(center(layout::mainSampleMenuButton(true)));
    check(f.session.state().mainSampleMode && !f.menu.open(), "menu item opens the view and closes the menu");
}

void undoRedoButtonsAndKeys()
{
    Fixture f;
    namespace L = midichopper::ui::layout;
    check(L::historyUndo.x + L::historyUndo.width <= L::historyRedo.x &&
          L::historyRedo.x + L::historyRedo.width == L::mainSampleWaveform.x + L::mainSampleWaveform.width &&
          L::historyRedo.y + L::historyRedo.height <= 122.0f && L::historyUndo.y >= L::mainPanel.y,
          "undo/redo end at the waveform's right edge, just above the hint line");
    check(!isTarget(f.input.targetAt(center(L::historyUndo)), InteractiveType::historyUndo),
          "no undo button outside the Main Sample view");
    f.session.openMainSample();
    f.states.clear();
    check(!f.session.canUndo() && !f.session.canRedo(), "buttons start disabled");
    f.click(center(L::historyUndo));
    check(f.states.empty(), "disabled undo button sends nothing");
    f.session.acceptState("undo_status", "UD1;1;2;1;0102000000000000000000000000000000000000000000000000000000000000");
    check(f.session.canUndo() && f.session.canRedo() && f.session.state().padColors[3] == 2,
          "UD1 status enables the buttons and applies colours");
    f.click(center(L::historyUndo));
    check(!f.states.empty() && f.states.back().first == "undo_request" && f.states.back().second == "undo",
          "UNDO button sends the undo request");
    check(!f.input.keyboard(InputKey::other, true), "Ctrl+Z is no longer consumed by the plugin");
    f.click(center(L::historyRedo));
    check(f.states.back().second == "redo", "REDO button sends the redo request");
    f.session.acceptState("undo_status", "UD1;2;0;0;0000000000000000000000000000000000000000000000000000000000000000");
    check(!f.session.canUndo() && !f.session.canRedo(), "empty history disables the buttons again");
}

int main()
{
    padOwnershipAndSpace();
    undoRedoButtonsAndKeys();
    overlaysAndFaderCapture();
    editorWheelAndPointerRoutes();
    mainSampleViewRoutes();
    applicationMenuOpensMainSample();
    std::cout << "UI input routing tests passed\n";
}

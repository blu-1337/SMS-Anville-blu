#include "SamplerInputController.hpp"

#include "MidichopperLayout.hpp"
#include "../plugin/CaptureActionProtocol.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace midichopper::ui {
namespace {
namespace uiLayout = layout;
using namespace plugin;
constexpr auto confirmationTimeout = std::chrono::seconds(2);

MixerPointerButton mixerButton(InputButton button) noexcept
{
    return button == InputButton::left ? MixerPointerButton::left
        : button == InputButton::middle ? MixerPointerButton::middle : MixerPointerButton::other;
}
} // namespace

SamplerInputController::SamplerInputController(SamplerInputHost& host, SamplerUiSession& session,
    MixerInteractionController& mixer, ApplicationMenuController& menu, const int colorCount) noexcept
    : fHost(host), fSession(session), fMixer(mixer), fMenu(menu), fColorCount(colorCount) {}

MixerInteractionContext SamplerInputController::mixerContext() const noexcept
{
    const auto& s = fSession.state();
    return {s.selectedPad, s.mixerSettings,
        fSession.chopEditor().active() || s.armed ? MixerValueEntryView::cutPoints
            : s.editorMode ? MixerValueEntryView::sampleEditor : MixerValueEntryView::main,
        s.monitorGain, {s.gain, s.globalPan, s.globalTune, s.globalLowpass,
                       s.globalHighpass, s.globalFilterSlope, s.globalDirty}};
}

sms::ui::waveform::EnvelopeGeometry SamplerInputController::envelopeGeometry() const noexcept
{
    return sms::ui::waveform::envelopeGeometry(uiLayout::envelopeGraph,
        fSession.state().waveform, fSession.displayEditorSettings());
}

sms::ui::InteractiveTarget SamplerInputController::targetAt(const sms::ui::Point position) const noexcept
{
    const auto& s = fSession.state();
    const auto& actions = fSession.padActions();
    const auto& chop = fSession.chopEditor();
    const auto menu = fHost.applicationMenuContext();
    std::array<bool, kPadMenuEntries.size()> enabled{};
    for (std::size_t i = 0; i < enabled.size(); ++i)
        enabled[i] = kPadMenuEntries[i].kind == sms::ui::ContextMenuItemKind::action &&
            actions.actionEnabled(fSession.padActionContext(), kPadMenuEntries[i].action);
    const auto envelope = envelopeGeometry();
    const auto playback = fSession.displayEditorSettings();
    InteractionContext context;
    context.editorMode = s.editorMode;
    context.chopEditorMode = chop.active();
    context.chopSplitMode = chop.split();
    context.mainSampleMode = s.mainSampleMode;
    context.mainSampleHasSource = fSession.mainSample().hasSource();
    context.holdPlayback = fSession.state().playbackMode >= 0.5f;
    context.dropdown = fSession.dropdown();
    context.perBankMidi = fSession.state().midiBankMode == 0;
    context.mainSampleConfirm = fSession.mainSample().confirmPending();
    context.mainSampleSlices = fSession.mainSample().displaySlices();
    context.mainSampleViewport = fSession.mainSample().viewport();
    context.menuOpen = fMenu.open();
    context.resetEnabled = fSession.resetEnabled();
    context.skinsAvailable = menu.skinsSelectable;
    context.menuSubmenu = fMenu.open() ? fMenu.submenu() : uiLayout::MenuSubmenu::none;
    context.skinMenu = fMenu.skinMenu();
    context.skinMenuItemCount = uiLayout::skinMenuVisibleCount(menu.skinCount);
    context.globalControlMenu = fMixer.menu();
    context.globalControlSettings = fMixer.settings(fMixer.menu().parameter);
    context.padContextMenuOpen = actions.menuOpen();
    context.padColorMenuOpen = actions.colorMenuOpen();
    context.armed = s.armed;
    context.fixedCapture = s.captureMode >= 0.5f;
    context.captureActive = s.armed && s.currentPad >= 0;
    context.chopReady = chop.ready();
    context.chopApplying = chop.applying();
    context.chopActionEnabled = chop.ready() && chop.dirty() && !chop.applying();
    context.chopPreviousEnabled = chop.canNavigate(-1, fSession.globalPad(0),
        fSession.visiblePadCount(), fSession.midiBankMode()) && !chop.applying();
    context.chopExitEnabled = !chop.applying();
    context.chopNextEnabled = chop.canNavigate(1, fSession.globalPad(0),
        fSession.visiblePadCount(), fSession.midiBankMode()) && !chop.applying();
    context.padLayout = s.layout;
    context.padContextMenu = actions.menu();
    context.padContextMenuEnabled = enabled;
    context.padColorMenu = actions.colorMenu();
    context.padColorMenuItemCount = fColorCount;
    context.editorSettings = &playback;
    context.envelope = &envelope;
    context.chopWaveforms = chop.waveforms();
    context.chopOffsets = chop.offsets();
    context.waveformViewport = fSession.detail().viewport();
    return interactiveTargetAt(position, context);
}

bool SamplerInputController::mouse(const sms::ui::Point position, const InputButton button,
                                  const bool press, const KnobAdjustment adjustment)
{
    if (press) fHost.focusSamplerInput();
    const float x = position.x;
    const auto& s = fSession.state();
    auto& actions = fSession.padActions();
    auto& chop = fSession.chopEditor();

    if (fMixer.menu().active() && press)
        return fMixer.menuPress(mixerButton(button), targetAt(position), position);
    if (button == InputButton::middle) {
        if (!press && fMainSampleDrag == MainSampleDrag::pan) {
            fMainSampleDrag = MainSampleDrag::none;
            return true;
        }
        if (press && s.mainSampleMode && !s.editorMode && !fMenu.open() && !actions.menuOpen() &&
            !fSession.mainSample().confirmPending() && fSession.mainSample().hasSource() &&
            uiLayout::mainSampleWaveform.contains(position)) {
            // Wheel-click and drag pans a zoomed waveform sideways.
            fMainSampleDrag = MainSampleDrag::pan;
            fMainSampleOriginX = position.x;
            fPanOriginStart = static_cast<double>(fSession.mainSample().viewport().start);
            return true;
        }
        if (!press) {
            if (fMixer.releaseCaptured(MixerPointerButton::middle)) return true;
            const bool captured = fResetPointerCaptured;
            fResetPointerCaptured = false;
            return captured;
        }
        const auto clicked = targetAt(position);
        fMixer.cancelEntry();
        if (fMixer.resetControl(mixerContext(), clicked)) return true;
        if (resetEnvelope(clicked)) {
            fResetPointerCaptured = true;
            return true;
        }
        return false;
    }
    if (button == InputButton::right) {
        if (!press && fMainSampleRightDrag) {
            fMainSampleRightDrag = false;
            fMainSampleDrag = MainSampleDrag::none;
            fSession.mainSample().endDrag();
            fHost.repaintSamplerInput();
            return true;
        }
        if (press && s.mainSampleMode && !s.editorMode && !fMenu.open() && !actions.menuOpen() &&
            !fSession.mainSample().confirmPending() && fSession.mainSample().hasSource() &&
            uiLayout::mainSampleWaveform.contains(position) && !isTarget(targetAt(position), InteractiveType::mainSamplePad)) {
            auto& main = fSession.mainSample();
            const auto& viewport = main.viewport();
            const double perPixel = static_cast<double>(viewport.end - viewport.start) /
                                    static_cast<double>(uiLayout::mainSampleWaveform.width);
            const auto frame = static_cast<std::uint32_t>(viewport.frameAt(position.x, uiLayout::mainSampleWaveform));
            const auto tolerance = static_cast<std::uint32_t>(perPixel * 8.0);
            if (main.beginSelectionHalfDrag(frame) || main.beginEdgeDrag(frame, tolerance)) {
                fMainSampleDrag = MainSampleDrag::marker;
                fMainSampleRightDrag = true;
                fHost.repaintSamplerInput();
            }
            return true; // A plain right-click on the waveform does nothing.
        }
        if (fMenu.open()) {
            if (press) fMenu.rightPress();
            return true;
        }
        if (!press) return actions.menuOpen() || fMixer.menu().active();
        fMixer.cancelEntry();
        const auto clicked = targetAt(position);
        if (globalValueParameterForTarget(clicked) != kParameterCount) {
            actions.closeMenu();
            fMixer.openMenu(clicked, position);
            return true;
        }
        const bool mainSampleView = s.mainSampleMode && !s.editorMode;
        if (s.armed || chop.active() ||
            (mainSampleView && fSession.mainSample().confirmPending())) return false;
        const auto grid = mainSampleView
            ? fSession.padLayout().grid(uiLayout::mainSamplePads, 8.0f)
            : fSession.padLayout().grid(s.editorMode
                ? uiLayout::editorPadBounds : uiLayout::mainPadBounds, s.editorMode ? 6.0f : 10.0f);
        const int localPad = fSession.localPadFromVisualIndex(grid.hit(position));
        if (localPad >= 0) {
            fMenu.dismiss();
            const int pad = fSession.globalPad(localPad);
            if (s.editorMode) fSession.selectEditorPad(pad);
            else fSession.selectMainPadForContextMenu(pad);
            actions.openMenu(fSession.padActionContext(), pad, position);
            fHost.repaintSamplerInput();
            return true;
        }
        if (actions.menuOpen()) {
            actions.closeMenu();
            fHost.repaintSamplerInput();
            return true;
        }
        return false;
    }
    if (button != InputButton::left) return false;

    if (press) {
        const auto clicked = targetAt(position);
        fMixer.cancelEntryUnless(clicked);
        if (fHover.update(clicked)) fHost.repaintSamplerInput();
        if (fMixer.faderPress(mixerContext(), clicked, position)) return true;
        if (actions.menuOpen()) {
            fPadContextPointerCaptured = true;
            if (isTarget(clicked, InteractiveType::padColorItem)) {
                actions.selectColor(fSession.padActionContext(), clicked.index);
            } else if (isTarget(clicked, InteractiveType::padContextItem)) {
                const auto row = static_cast<std::size_t>(clicked.index);
                if (row < kPadMenuEntries.size())
                    actions.invoke(fSession.padActionContext(), kPadMenuEntries[row]);
            } else {
                actions.closeMenu();
                fHost.repaintSamplerInput();
            }
            return true;
        }
        if (isTarget(clicked, InteractiveType::historyUndo) || isTarget(clicked, InteractiveType::historyRedo)) {
        fSession.requestHistory(isTarget(clicked, InteractiveType::historyUndo));
        return true;
    }
    if (fMenu.press(fHost.applicationMenuContext(), clicked, Clock::now())) return true;
        if (chop.active()) {
            if (beginViewportDrag(clicked, position)) return true;
            if (isTarget(clicked, InteractiveType::chopBoundary)) chop.beginBoundaryDrag(clicked.index, x);
            else if (isTarget(clicked, InteractiveType::chopPadPreview)) chop.preview(clicked.index);
            else if (isTarget(clicked, InteractiveType::chopAction)) {
                if (chop.split()) chop.apply(); else chop.cancelChanges();
            } else if (isTarget(clicked, InteractiveType::chopPrevious))
                chop.navigate(-1, fSession.globalPad(0), fSession.visiblePadCount(), fSession.midiBankMode());
            else if (isTarget(clicked, InteractiveType::chopExit)) chop.exit();
            else if (isTarget(clicked, InteractiveType::chopNext))
                chop.navigate(1, fSession.globalPad(0), fSession.visiblePadCount(), fSession.midiBankMode());
            else return false;
            return true;
        }
        if (s.editorMode) {
            if (fSession.dropdown() != 0) {
                if (isTarget(clicked, InteractiveType::dropdownItem))
                    fSession.chooseDropdown(clicked.index);
                else if (isTarget(clicked, InteractiveType::padChoke))
                    fSession.toggleDropdown(1);
                else if (isTarget(clicked, InteractiveType::padOutput))
                    fSession.toggleDropdown(2);
                else
                    fSession.closeDropdown();
                return true; // an open list swallows the click that dismisses it
            }
            if (beginViewportDrag(clicked, position)) return true;
            if (isTarget(clicked, InteractiveType::playStop)) {
                fPlayButtonPressCaptured = true;
                fSession.togglePlayStop();
                return true;
            }
            if (isTarget(clicked, InteractiveType::padDirection)) {
                fSession.flipSampleDirection();
                return true;
            }
            if (isTarget(clicked, InteractiveType::padGlobalMixer)) {
                fSession.toggleSampleGlobalMixer();
                return true;
            }
            if (isTarget(clicked, InteractiveType::padOutput)) {
                fSession.toggleDropdown(2);
                return true;
            }
            if (isTarget(clicked, InteractiveType::padLoop)) {
                fSession.toggleSampleHoldLoop();
                return true;
            }
            if (isTarget(clicked, InteractiveType::padChoke)) {
                fSession.toggleDropdown(1);
                return true;
            }
            if (fMixer.sampleMixerPress(mixerContext(), clicked, position, adjustment)) {
                if (isTarget(clicked, InteractiveType::mixerKnob) && fMixer.entry().target().valid())
                    fDragTarget = EditTarget::none;
                return true;
            }
            if (isTarget(clicked, InteractiveType::closeEditor)) {
                fSession.closeSampleEditor();
                return true;
            }
            if (isTarget(clicked, InteractiveType::bank)) {
                fSession.selectBank(clicked.index);
                return true;
            }
            if (isTarget(clicked, InteractiveType::pad)) {
                fSession.selectEditorPad(fSession.globalPad(clicked.index));
                fSession.auditionSelectedEditorPad();
                return true;
            }
            if (isTarget(clicked, InteractiveType::playOnSelect)) {
                fSession.cycleEditorAuditionMode();
                return true;
            }
            if (isTarget(clicked, InteractiveType::regionHandle)) {
                fDragTarget = static_cast<EditTarget>(clicked.index);
                updateEditorDrag(position);
                return true;
            }
            if (isTarget(clicked, InteractiveType::envelopeNode)) {
                fDragTarget = static_cast<EditTarget>(clicked.index);
                fDragStart = position;
                fDragStartSettings = fSession.displayEditorSettings();
                return true;
            }
            if (isTarget(clicked, InteractiveType::envelopeSlider)) {
                constexpr std::array targets{EditTarget::attackSlider, EditTarget::decaySlider,
                    EditTarget::sustainSlider, EditTarget::releaseSlider};
                fDragTarget = targets[static_cast<std::size_t>(clicked.index)];
                updateEditorDrag(position);
                return true;
            }
            return false;
        }

        if (s.mainSampleMode) {
            auto& main = fSession.mainSample();
            if (isTarget(clicked, InteractiveType::mainSampleConfirm)) {
                main.confirmAssign(clicked.index == 0);
                fHost.repaintSamplerInput();
                return true;
            }
            if (main.countEditing() &&
                !(isTarget(clicked, InteractiveType::mainSampleAutoChop) && clicked.index == 6))
                main.commitCount();
            if (main.confirmPending()) return true;
            if (isTarget(clicked, InteractiveType::mainSampleClose)) { fSession.closeMainSample(); return true; }
            if (isTarget(clicked, InteractiveType::mainSampleAutoChop)) {
                switch (clicked.index) {
                case 0: main.cycleAutoChopMode(); break;
                case 1: main.adjustAutoChopCount(-1); break;
                case 2: main.adjustAutoChopCount(1); break;
                case 3: main.adjustAutoChopFirstPad(-1); break;
                case 4: main.adjustAutoChopFirstPad(1); break;
                case 6: main.clickCountField(Clock::now()); break;
                default: main.autoChop(); break;
                }
                return true;
            }
            if (isTarget(clicked, InteractiveType::mainSampleTransport)) {
                if (clicked.index == 0) main.togglePlay();
                else if (clicked.index == 1) main.toggleLoop();
                else if (clicked.index == 4) main.zoomFit(Clock::now());
                else if (clicked.index == 5) main.toggleSnap();
                else if (clicked.index == 6) main.toggleSpectrogram(Clock::now());
                else if (clicked.index >= 2) {
                    const auto& waveform = uiLayout::mainSampleWaveform;
                    main.zoom(clicked.index == 3 ? 1.0f : -1.0f, waveform.x + waveform.width * 0.5f,
                              waveform, Clock::now());
                }
                fHost.repaintSamplerInput();
                return true;
            }
            if (isTarget(clicked, InteractiveType::mainSampleAction)) {
                if (clicked.index == 0) assignSelectionToFreePad();
                else if (clicked.index == 1 && !main.arrange())
                    fSession.setLocalStatus("Pads already in order");
                else if (clicked.index == 2) fSession.autoColorPads();
                else if (clicked.index == 3) fSession.clearPadColors();
                fHost.repaintSamplerInput();
                return true;
            }
            if (isTarget(clicked, InteractiveType::mainSampleLoad)) { fSession.browseMainSample(); return true; }
            if (isTarget(clicked, InteractiveType::mainSampleClear)) { main.clear(); return true; }
            if (isTarget(clicked, InteractiveType::bank)) { fSession.selectBank(clicked.index); return true; }
            if (isTarget(clicked, InteractiveType::mainSamplePad)) return pressMainSamplePad(clicked.index);
            if (isTarget(clicked, InteractiveType::mainSampleOverview)) {
                fMainSampleDrag = MainSampleDrag::overview;
                updateMainSampleDrag(position);
                return true;
            }
            if (isTarget(clicked, InteractiveType::mainSampleBadge)) {
                const auto& slices = main.displaySlices();
                const auto index = static_cast<std::size_t>(clicked.index);
                if (index >= slices.size()) return false;
                main.selectSlice(slices[index].id);
                if (main.beginRegionDrag(slices[index].id, static_cast<std::uint32_t>(
                        main.viewport().frameAt(position.x, uiLayout::mainSampleWaveform))))
                    fMainSampleDrag = MainSampleDrag::marker;
                fHost.repaintSamplerInput();
                return true;
            }
            if (isTarget(clicked, InteractiveType::mainSampleMarker)) {
                const auto& slices = main.displaySlices();
                const auto index = static_cast<std::size_t>(clicked.index / 2);
                if (index >= slices.size()) return false;
                const bool hadSelection = main.selectedSlice() != 0U;
                if (hadSelection && slices[index].id != main.selectedSlice()) {
                    // The active slice keeps priority: grab its edge if one is in reach.
                    const auto& viewport = main.viewport();
                    const double perPixel = static_cast<double>(viewport.end - viewport.start) /
                                            static_cast<double>(uiLayout::mainSampleWaveform.width);
                    if (main.beginEdgeDrag(static_cast<std::uint32_t>(
                            viewport.frameAt(position.x, uiLayout::mainSampleWaveform)),
                            static_cast<std::uint32_t>(perPixel * 8.0))) {
                        fMainSampleDrag = MainSampleDrag::marker;
                        fHost.repaintSamplerInput();
                        return true;
                    }
                }
                main.selectSlice(slices[index].id);
                if (main.beginMarkerDrag(slices[index].id, (clicked.index & 1) != 0, hadSelection))
                    fMainSampleDrag = MainSampleDrag::marker;
                fHost.repaintSamplerInput();
                return true;
            }
            if (isTarget(clicked, InteractiveType::mainSampleWaveform))
                return beginMainSampleWaveform(position);
            return false;
        }

        if (isTarget(clicked, InteractiveType::midiBankHint)) {
            fSession.selectMidiBankMode(1); // All Banks: one continuous range across A-D
            fHost.repaintSamplerInput();
            return true;
        }
        if (isTarget(clicked, InteractiveType::mainSampleOpen)) { fSession.openMainSample(); return true; }
        if (isTarget(clicked, InteractiveType::playStop)) {
            fPlayButtonPressCaptured = true;
            fSession.togglePlayStop();
            return true;
        }
        if (isTarget(clicked, InteractiveType::globalDirection)) {
            fHost.editSamplerParameter(kParameterGlobalReverse, true);
            fSession.setControlValue(kParameterGlobalReverse, s.globalReverse ? 0.0f : 1.0f);
            fHost.editSamplerParameter(kParameterGlobalReverse, false);
            return true;
        }
        if (fMixer.globalMixerPress(mixerContext(), clicked, position, adjustment)) return true;
        if (isTarget(clicked, InteractiveType::openEditor)) {
            fSession.openSampleEditor(!fSession.hasSelectedPad() ||
                fSession.bankForGlobalPad(s.selectedPad) != s.bank ? fSession.globalPad(0) : s.selectedPad);
            return true;
        }
        if (isTarget(clicked, InteractiveType::bank)) { fSession.selectBank(clicked.index); return true; }
        if (isTarget(clicked, InteractiveType::pad)) {
            if (s.armed) fSession.selectCaptureTarget(clicked.index);
            else fSession.pressPlaybackPad(clicked.index);
            fHost.repaintSamplerInput();
            return true;
        }
        switch (static_cast<InteractiveType>(clicked.type)) {
        case InteractiveType::modeToggle: fSession.setControlValue(kParameterMode, s.armed ? 0.0f : 1.0f); break;
        case InteractiveType::sequentialMode: fSession.setControlValue(kParameterCaptureMode, 0.0f); break;
        case InteractiveType::fixedMode: fSession.setControlValue(kParameterCaptureMode, 1.0f); break;
        case InteractiveType::oneShotMode: fSession.setControlValue(kParameterPlaybackMode, 0.0f); break;
        case InteractiveType::holdMode: fSession.setControlValue(kParameterPlaybackMode, 1.0f); break;
        case InteractiveType::monitor: fSession.setControlValue(kParameterInputMonitor, nextInputMonitorMode(s.monitor)); break;
        case InteractiveType::fixedLength:
        case InteractiveType::voiceLimit:
        case InteractiveType::preRoll: {
            fMainSliderDrag = clicked.type;
            const auto parameter = isTarget(clicked, InteractiveType::fixedLength) ? kParameterFixedLengthSeconds
                : isTarget(clicked, InteractiveType::voiceLimit) ? kParameterMaxVoices : kParameterPreRollMs;
            fHost.editSamplerParameter(parameter, true);
            updateMainSliderDrag(x);
            break;
        }
        case InteractiveType::finalizeAction:
            fSession.captureAction("finalize");
            fSession.setLocalStatus("Chop finalized");
            break;
        case InteractiveType::undoAction:
            fSession.captureAction("undo");
            fSession.setLocalStatus("Last chop undone");
            break;
        case InteractiveType::clearAction:
            if (!fClearArmed) {
                fClearArmed = true;
                fClearDeadline = Clock::now() + confirmationTimeout;
                fSession.setLocalStatus("Click CLEAR again to confirm");
            } else {
                fSession.captureAction("clear");
                fClearArmed = false;
                fSession.setLocalStatus("Pads cleared");
            }
            fHost.repaintSamplerInput();
            break;
        default: return false;
        }
        return true;
    }

    if (fMixer.releaseCaptured()) return true;
    if (fResetPointerCaptured) { fResetPointerCaptured = false; return true; }
    if (fPadContextPointerCaptured) { fPadContextPointerCaptured = false; return true; }
    if (chop.active() && chop.activeBoundary() >= 0) {
        chop.endBoundaryDrag();
        fHost.repaintSamplerInput();
        return true;
    }
    if (fViewportDrag >= 0) { fViewportDrag = -1; return true; }
    if (fMainSampleDrag != MainSampleDrag::none) return endMainSampleDrag(position);
    if (fPlayButtonPressCaptured) { fPlayButtonPressCaptured = false; return true; }
    if (fMixer.releaseFader()) return true;
    if (s.editorMode && fDragTarget != EditTarget::none) {
        fSession.commitPlaybackSettings(s.editorSettings);
        fDragTarget = EditTarget::none;
        fHost.repaintSamplerInput();
        return true;
    }
    if ((s.editorMode && fMixer.sampleDragging()) || fMixer.globalDragging())
        return fMixer.releaseKnob(mixerContext());
    if (fMainSliderDrag >= 0) {
        const auto type = static_cast<InteractiveType>(fMainSliderDrag);
        fHost.editSamplerParameter(type == InteractiveType::fixedLength ? kParameterFixedLengthSeconds
            : type == InteractiveType::voiceLimit ? kParameterMaxVoices : kParameterPreRollMs, false);
        fMainSliderDrag = -1;
        return true;
    }
    if (fSession.padPress().pad() >= 0) {
        fSession.releasePlaybackPad();
        fHost.repaintSamplerInput();
        return true;
    }
    return false;
}

bool SamplerInputController::motion(const sms::ui::Point position)
{
    const auto& s = fSession.state();
    auto& chop = fSession.chopEditor();
    if (fMixer.faderDragging()) return fMixer.motion(mixerContext(), position);
    if (fViewportDrag >= 0) { updateViewportDrag(position); return true; }
    if (fMainSampleDrag != MainSampleDrag::none) { updateMainSampleDrag(position); return true; }
    if (chop.active() && chop.activeBoundary() >= 0) {
        chop.dragBoundary(position.x, uiLayout::chopWaveform.width, fSession.detail().viewport());
        return true;
    }
    if (s.editorMode && fDragTarget != EditTarget::none) { updateEditorDrag(position); return true; }
    if ((s.editorMode && fMixer.sampleDragging()) || fMixer.globalDragging())
        return fMixer.motion(mixerContext(), position);
    if (fMainSliderDrag >= 0) { updateMainSliderDrag(position.x); return true; }
    const auto hovered = targetAt(position);
    fMenu.hover(fHost.applicationMenuContext(), hovered);
    auto& actions = fSession.padActions();
    if (actions.menuOpen()) {
        if (isTarget(hovered, InteractiveType::padContextItem, kPadColorMenuRow))
            actions.openColorMenu(fSession.padActionContext());
        else if (actions.colorMenuOpen() && isTarget(hovered, InteractiveType::padContextItem)) {
            actions.hideColorMenu();
            fHost.repaintSamplerInput();
        }
    }
    fMixer.hover(hovered);
    if (fHover.update(hovered)) fHost.repaintSamplerInput();
    return hovered.valid() || fMenu.open() || actions.menuOpen() || fMixer.menu().active();
}

void SamplerInputController::wheelParameter(const std::uint32_t parameter, const float value)
{
    fHost.editSamplerParameter(parameter, true);
    fSession.setControlValue(parameter, value);
    fHost.editSamplerParameter(parameter, false);
}

bool SamplerInputController::scroll(const sms::ui::Point position, const float delta,
    const bool control, const bool shift, const KnobAdjustment adjustment)
{
    if (fMixer.menu().active()) return true;
    const auto hovered = targetAt(position);
    if (fHover.update(hovered)) fHost.repaintSamplerInput();
    if (delta == 0.0f || !std::isfinite(delta)) return false;
    if (fMenu.scroll(fHost.applicationMenuContext(), position, delta)) return true;
    const auto& s = fSession.state();
    if (isTarget(hovered, InteractiveType::levelFader) || isTarget(hovered, InteractiveType::levelValueLabel))
        return fMixer.wheel(mixerContext(), hovered, delta, adjustment);
    auto& chop = fSession.chopEditor();
    auto& detail = fSession.detail();
    if (s.mainSampleMode && !s.editorMode && !chop.active() &&
        fSession.mainSample().hasSource() && !fSession.mainSample().confirmPending() &&
        (uiLayout::mainSampleWaveform.contains(position) || uiLayout::mainSampleOverview.contains(position))) {
        auto& main = fSession.mainSample();
        // The wheel zooms around the pointer; Shift pans.
        const bool changed = shift ? main.pan(delta, Clock::now())
            : main.zoom(delta, position.x, uiLayout::mainSampleWaveform, Clock::now());
        if (changed) fHost.repaintSamplerInput();
        return true;
    }
    const auto bounds = chop.active() ? uiLayout::chopWaveform : uiLayout::editorWaveform;
    if ((s.editorMode || (chop.active() && chop.ready() && !chop.applying())) &&
        bounds.contains(position) && detail.viewport().total != 0U && (control || shift)) {
        const bool changed = control ? detail.zoom(delta, position.x, bounds, Clock::now())
            : detail.pan(delta, Clock::now());
        if (changed) fHost.repaintSamplerInput();
        return true;
    }
    if (isTarget(hovered, InteractiveType::chopBoundary)) {
        chop.wheelBoundary(hovered.index, delta, uiLayout::chopWaveform, detail.viewport());
        return true;
    }
    if (isTarget(hovered, InteractiveType::regionHandle)) {
        auto displayed = fSession.displayEditorSettings();
        sms::ui::waveform::adjustRegionByWheel(displayed, static_cast<EditTarget>(hovered.index),
            delta, s.waveform.frames, uiLayout::editorWaveform, detail.viewport());
        fSession.commitPlaybackSettings(sampleEditorLocalEdit(s.editorSettings, displayed,
            fSession.effectiveGlobalReverse(), static_cast<EditTarget>(hovered.index) == EditTarget::regionStart,
            static_cast<EditTarget>(hovered.index) == EditTarget::regionEnd));
        fHost.repaintSamplerInput();
        return true;
    }
    if (isTarget(hovered, InteractiveType::fixedLength)) {
        wheelParameter(kParameterFixedLengthSeconds, sms::ui::wheelAdjustedValue(s.fixedLength,
            delta, 0.1f, parameterRanges::fixedLengthSeconds.minimum, parameterRanges::fixedLengthSeconds.maximum));
        return true;
    }
    if (isTarget(hovered, InteractiveType::voiceLimit)) {
        wheelParameter(kParameterMaxVoices, sms::ui::wheelAdjustedValue(static_cast<float>(s.maxVoices),
            delta, 1.0f, parameterRanges::maxVoices.minimum, parameterRanges::maxVoices.maximum, true));
        return true;
    }
    if (isTarget(hovered, InteractiveType::preRoll)) {
        wheelParameter(kParameterPreRollMs, sms::ui::wheelAdjustedValue(s.preRoll, delta, 1.0f,
            parameterRanges::preRollMs.minimum, parameterRanges::preRollMs.maximum, true));
        return true;
    }
    if (fMixer.wheel(mixerContext(), hovered, delta, adjustment)) return true;
    if (!isTarget(hovered, InteractiveType::envelopeSlider)) return false;
    auto playback = s.editorSettings;
    switch (hovered.index) {
    case 0: playback.attackSeconds = sms::ui::wheelAdjustedValue(playback.attackSeconds, delta, 0.01f,
        0.0f, sms::ui::waveform::kEnvelopeControlMaximumSeconds); break;
    case 1: playback.decaySeconds = sms::ui::wheelAdjustedValue(playback.decaySeconds, delta, 0.01f,
        0.0f, sms::ui::waveform::kEnvelopeControlMaximumSeconds); break;
    case 2: playback.sustainLevel = sms::ui::wheelAdjustedValue(playback.sustainLevel, delta, 0.01f, 0.0f, 1.0f); break;
    case 3: playback.releaseSeconds = sms::ui::wheelAdjustedValue(playback.releaseSeconds, delta, 0.01f,
        0.0f, sms::ui::waveform::kEnvelopeControlMaximumSeconds); break;
    default: return false;
    }
    fSession.commitPlaybackSettings(sms::dsp::sanitize(playback));
    fHost.repaintSamplerInput();
    return true;
}

bool SamplerInputController::keyboard(const InputKey key, const bool press, const bool backward)
{
    if (fSession.state().mainSampleMode && fSession.mainSample().countEditing()) {
        auto& main = fSession.mainSample();
        if (press && key == InputKey::commit) main.commitCount();
        else if (press && key == InputKey::escape) main.cancelCount();
        else if (press && (key == InputKey::backspace || key == InputKey::deleteText)) main.countBackspace();
        fHost.repaintSamplerInput();
        return true; // typing owns the keyboard; digits arrive through character()
    }
    if (key == InputKey::space) {
        fHost.ignoreSamplerKeyRepeat(press);
        const bool blocked = fMixer.entry().target().valid() || fMenu.open() ||
            fSession.padActions().menuOpen() || fMixer.menu().active() ||
            fSession.chopEditor().active() || fSession.padActions().dialogPending();
        const auto action = fSpaceKey.update(press, blocked, fSession.state().armed);
        const auto& s = fSession.state();
        if (action == SpaceAction::playStop && s.mainSampleMode && !s.editorMode) toggleMainSamplePreview();
        else if (action == SpaceAction::playStop) fSession.togglePlayStop();
        else if (action == SpaceAction::chop) fSession.captureSpace();
        return true;
    }
    const auto entryKey = key == InputKey::escape ? MixerEntryKey::escape
        : key == InputKey::commit ? MixerEntryKey::commit : key == InputKey::tab ? MixerEntryKey::tab
        : key == InputKey::backspace ? MixerEntryKey::backspace
        : key == InputKey::deleteText ? MixerEntryKey::deleteText : MixerEntryKey::other;
    if (fMixer.keyboard(mixerContext(), entryKey, press, backward)) return true;
    const auto& state = fSession.state();
    const bool mainSampleTop = state.mainSampleMode && !state.editorMode && !fSession.chopEditor().active();
    if (mainSampleTop && press && key == InputKey::deleteText && !fSession.mainSample().confirmPending() &&
        fSession.mainSample().deleteSlice(fSession.mainSample().selectedSlice())) {
        fSession.mainSample().selectSlice(0U);
        fHost.repaintSamplerInput();
        return true;
    }
    if (mainSampleTop && press && key >= InputKey::digit1 && key <= InputKey::digit9 &&
        !fSession.mainSample().confirmPending() && !fMenu.open() && !fSession.padActions().menuOpen()) {
        // Keys 1-9 pick slice 1-9; Space or Play then plays exactly that slice.
        fSession.mainSample().selectSliceNumber(
            static_cast<std::size_t>(key) - static_cast<std::size_t>(InputKey::digit1) + 1U);
        fHost.repaintSamplerInput();
        return true;
    }
    if (!press || key != InputKey::escape) return false;
    if (fSession.dropdown() != 0) {
        fSession.closeDropdown();
        return true;
    }
    if (fSession.padActions().menuOpen()) {
        fSession.padActions().closeMenu();
        fHost.repaintSamplerInput();
        return true;
    }
    if (fMenu.escape()) return true;
    auto& chop = fSession.chopEditor();
    if (chop.active() && !chop.applying()) { chop.exit(); return true; }
    if (mainSampleTop) {
        if (fSession.mainSample().confirmPending()) fSession.mainSample().confirmAssign(false);
        else fSession.closeMainSample();
        fHost.repaintSamplerInput();
        return true;
    }
    return false;
}

bool SamplerInputController::character(const std::uint32_t codepoint)
{
    if (fSession.state().mainSampleMode && fSession.mainSample().countCharacter(codepoint)) {
        fHost.repaintSamplerInput();
        return true;
    }
    return codepoint == ' ' || fMixer.character(codepoint);
}

void SamplerInputController::focus(const bool focused)
{
    if (focused) return;
    fMixer.cancelScope(MixerCancelScope::focus);
    if (fMainSampleDrag != MainSampleDrag::none) {
        fSession.mainSample().cancelDrag();
        fMainSampleDrag = MainSampleDrag::none;
    }
    fSpaceKey.reset();
    fHost.ignoreSamplerKeyRepeat(false);
}

void SamplerInputController::idle(const Clock::time_point now)
{
    if (fClearArmed && now >= fClearDeadline) {
        fClearArmed = false;
        fHost.repaintSamplerInput();
    }
    fMenu.idle(now);
    fMixer.idle(now);
}

void SamplerInputController::invalidate(const UiInteractionInvalidation reason)
{
    switch (reason) {
    case UiInteractionInvalidation::armChanged: fMixer.cancelScope(MixerCancelScope::mode); break;
    case UiInteractionInvalidation::armEntered:
    case UiInteractionInvalidation::sampleEditorClosed:
        fDragTarget = EditTarget::none;
        fMixer.cancelScope(MixerCancelScope::sample);
        break;
    case UiInteractionInvalidation::globalDirectionChanged:
    case UiInteractionInvalidation::sampleGlobalMixerChanged: fDragTarget = EditTarget::none; break;
    case UiInteractionInvalidation::clearPads: fMixer.closeMenu(); fMixer.cancelEntry(); break;
    case UiInteractionInvalidation::clearPadsInputs:
        fClearArmed = false;
        fMenu.reset();
        clearHover();
        break;
    case UiInteractionInvalidation::padActionHover: clearHover(); break;
    }
}

bool SamplerInputController::pressMainSamplePad(const int localPad)
{
    auto& main = fSession.mainSample();
    const int pad = fSession.globalPad(localPad);
    if (main.hasSelection()) {
        main.assignSelection(static_cast<std::uint32_t>(pad));
        main.clearSelection();
        fHost.repaintSamplerInput();
        return true;
    }
    const auto now = Clock::now();
    if (const int slice = main.sliceForPad(static_cast<std::uint32_t>(pad)); slice >= 0) {
        const auto& linked = main.status().slices[static_cast<std::size_t>(slice)];
        main.selectSlice(linked.id);
        main.revealRange(linked.start, linked.end, now);
    }
    fSession.pressPlaybackPad(localPad);
    fHost.repaintSamplerInput();
    return true;
}

void SamplerInputController::assignSelectionToFreePad()
{
    auto& main = fSession.mainSample();
    if (!main.hasSelection()) {
        fSession.setLocalStatus("Select an area first");
        return;
    }
    const auto& padState = fSession.state().padState;
    const int local = MainSampleController::firstFreePad(
        std::string_view(padState.data(), padState.size()), fSession.visiblePadCount());
    if (local < 0) {
        fSession.setLocalStatus("No free pad in this bank");
        return;
    }
    main.assignSelection(static_cast<std::uint32_t>(fSession.globalPad(local)));
    main.clearSelection();
}

void SamplerInputController::toggleMainSamplePreview()
{
    fSession.mainSample().togglePlay();
}

bool SamplerInputController::beginMainSampleWaveform(const sms::ui::Point position)
{
    auto& main = fSession.mainSample();
    main.beginSelection(snappedFrame(position.x));
    fMainSampleOriginX = position.x;
    fMainSampleMoved = false;
    const auto now = Clock::now();
    fMainSampleDouble = now - fMainSampleClickTime < std::chrono::milliseconds(400) &&
                        std::abs(position.x - fMainSampleClickX) < 5.0f;
    fMainSampleClickTime = fMainSampleDouble ? Clock::time_point{} : now;
    fMainSampleClickX = position.x;
    fMainSampleDrag = MainSampleDrag::selecting;
    fHost.repaintSamplerInput();
    return true;
}

std::uint32_t SamplerInputController::snappedFrame(const float x) const
{
    const auto& main = fSession.mainSample();
    const auto& viewport = main.viewport();
    const double framesPerPixel = static_cast<double>(viewport.end - viewport.start) /
                                  static_cast<double>(uiLayout::mainSampleWaveform.width);
    return main.snapFrame(static_cast<std::uint32_t>(viewport.frameAt(x, uiLayout::mainSampleWaveform)),
                          framesPerPixel);
}

void SamplerInputController::updateMainSampleDrag(const sms::ui::Point position)
{
    auto& main = fSession.mainSample();
    if (fMainSampleDrag == MainSampleDrag::marker && main.regionDragging()) {
        const auto& viewport = main.viewport();
        main.dragRegionTo(static_cast<std::uint32_t>(viewport.frameAt(position.x, uiLayout::mainSampleWaveform)),
                          static_cast<double>(viewport.end - viewport.start) /
                              static_cast<double>(uiLayout::mainSampleWaveform.width));
        return;
    }
    const auto frame = snappedFrame(position.x);
    if (fMainSampleDrag == MainSampleDrag::marker) main.dragTo(frame);
    else if (fMainSampleDrag == MainSampleDrag::selecting) {
        if (!fMainSampleMoved && std::abs(position.x - fMainSampleOriginX) < 3.0f) return;
        fMainSampleMoved = true;
        main.extendSelection(frame);
    } else if (fMainSampleDrag == MainSampleDrag::pan) {
        const auto& viewport = main.viewport();
        const auto length = viewport.end - viewport.start;
        if (viewport.total <= length) return;
        const double perPixel = static_cast<double>(length) / static_cast<double>(uiLayout::mainSampleWaveform.width);
        const double start = fPanOriginStart - static_cast<double>(position.x - fMainSampleOriginX) * perPixel;
        main.setScrollPosition(static_cast<float>(std::clamp(
            start / static_cast<double>(viewport.total - length), 0.0, 1.0)), Clock::now());
    } else if (fMainSampleDrag == MainSampleDrag::overview) {
        const auto& viewport = main.viewport();
        const auto length = viewport.end - viewport.start;
        if (viewport.total <= length) return;
        const auto& track = uiLayout::mainSampleOverview;
        const double center = std::clamp(static_cast<double>((position.x - track.x) / track.width), 0.0, 1.0) *
            static_cast<double>(viewport.total);
        main.setScrollPosition(static_cast<float>(std::clamp(
            (center - static_cast<double>(length) * 0.5) / static_cast<double>(viewport.total - length),
            0.0, 1.0)), Clock::now());
    }
    fHost.repaintSamplerInput();
}

bool SamplerInputController::endMainSampleDrag(const sms::ui::Point position)
{
    auto& main = fSession.mainSample();
    const auto drag = fMainSampleDrag;
    fMainSampleDrag = MainSampleDrag::none;
    if (drag == MainSampleDrag::marker) main.endDrag();
    else if (drag == MainSampleDrag::selecting) {
        const auto frame = static_cast<std::uint32_t>(
            main.viewport().frameAt(position.x, uiLayout::mainSampleWaveform));
        if (!fMainSampleMoved && fMainSampleDouble) {
            // Double-click zooms to the slice under the pointer, or fits when there is none.
            main.clearSelection();
            main.stopPreview();
            const int index = main.sliceIndexAt(frame);
            if (index >= 0) {
                const auto& slice = main.displaySlices()[static_cast<std::size_t>(index)];
                main.selectSlice(slice.id);
                main.zoomToRange(slice.start, slice.end, Clock::now());
            } else {
                main.zoomFit(Clock::now());
            }
        } else if (!fMainSampleMoved) {
            main.clearSelection();
            const int index = main.sliceIndexAt(frame);
            main.selectSlice(index >= 0 ? main.displaySlices()[static_cast<std::size_t>(index)].id : 0U);
            main.previewFrom(frame);
        } else if (main.hasSelection()) {
            const auto dropped = targetAt(position);
            if (isTarget(dropped, InteractiveType::mainSamplePad)) {
                main.assignSelection(static_cast<std::uint32_t>(fSession.globalPad(dropped.index)));
                main.clearSelection();
            }
        }
    }
    fHost.repaintSamplerInput();
    return true;
}

bool SamplerInputController::beginViewportDrag(const sms::ui::InteractiveTarget clicked,
                                             const sms::ui::Point position)
{
    if (!isTarget(clicked, InteractiveType::waveformZoom) && !isTarget(clicked, InteractiveType::waveformScroll))
        return false;
    fViewportDrag = clicked.type;
    if (isTarget(clicked, InteractiveType::waveformScroll)) {
        const auto track = fSession.chopEditor().active() ? uiLayout::chopScroll : uiLayout::editorScroll;
        const auto thumb = fSession.detail().viewport().scrollThumb(track);
        fViewportScrollGrabX = thumb.contains(position) ? position.x - thumb.x : thumb.width * 0.5f;
    }
    updateViewportDrag(position);
    return true;
}

void SamplerInputController::updateViewportDrag(const sms::ui::Point position)
{
    auto& detail = fSession.detail();
    bool changed = false;
    if (fViewportDrag == static_cast<int>(InteractiveType::waveformZoom)) {
        const auto track = fSession.chopEditor().active() ? uiLayout::chopZoom : uiLayout::editorZoom;
        changed = detail.setZoomPosition(std::clamp((track.y + track.height - 6.0f - position.y) /
            (track.height - 12.0f), 0.0f, 1.0f), Clock::now());
    } else if (fViewportDrag == static_cast<int>(InteractiveType::waveformScroll)) {
        const auto track = fSession.chopEditor().active() ? uiLayout::chopScroll : uiLayout::editorScroll;
        const auto thumb = detail.viewport().scrollThumb(track);
        if (track.width > thumb.width)
            changed = detail.setScrollPosition((position.x - fViewportScrollGrabX - track.x) /
                (track.width - thumb.width), Clock::now());
    }
    if (changed) fHost.repaintSamplerInput();
}

void SamplerInputController::updateEditorDrag(const sms::ui::Point position)
{
    auto displayed = fSession.displayEditorSettings();
    if (fDragTarget == EditTarget::regionStart || fDragTarget == EditTarget::regionEnd)
        sms::ui::waveform::updateRegion(displayed, fDragTarget, position.x,
            uiLayout::editorWaveform, fSession.detail().viewport());
    else if (fDragTarget >= EditTarget::attackSlider && fDragTarget <= EditTarget::releaseSlider) {
        const int slider = static_cast<int>(fDragTarget) - static_cast<int>(EditTarget::attackSlider);
        sms::ui::waveform::updateEnvelopeSlider(displayed, fDragTarget, position.x, uiLayout::editorSlider(slider));
    } else if (fDragTarget >= EditTarget::attackNode && fDragTarget <= EditTarget::releaseNode)
        sms::ui::waveform::updateEnvelopeNode(displayed, fDragStartSettings, fDragTarget,
            position, fDragStart, envelopeGeometry());
    fSession.commitPlaybackSettings(sampleEditorLocalEdit(fSession.state().editorSettings,
        displayed, fSession.effectiveGlobalReverse(), fDragTarget == EditTarget::regionStart,
        fDragTarget == EditTarget::regionEnd));
    fHost.repaintSamplerInput();
}

void SamplerInputController::updateMainSliderDrag(const float x)
{
    const auto type = static_cast<InteractiveType>(fMainSliderDrag);
    if (type == InteractiveType::fixedLength)
        fSession.setControlValue(kParameterFixedLengthSeconds,
            mainSliderValueAtX(x, uiLayout::fixedLength, parameterRanges::fixedLengthSeconds));
    else if (type == InteractiveType::voiceLimit)
        fSession.setControlValue(kParameterMaxVoices,
            mainSliderValueAtX(x, uiLayout::voiceLimit, parameterRanges::maxVoices, true));
    else if (type == InteractiveType::preRoll)
        fSession.setControlValue(kParameterPreRollMs, mainSliderValueAtX(x,
            uiLayout::preRoll(fSession.state().captureMode >= 0.5f), parameterRanges::preRollMs, true));
}

bool SamplerInputController::resetEnvelope(const sms::ui::InteractiveTarget clicked)
{
    if (!isTarget(clicked, InteractiveType::envelopeSlider)) return false;
    auto playback = fSession.state().editorSettings;
    sms::ui::waveform::resetEnvelopeSlider(playback, clicked.index);
    fSession.commitPlaybackSettings(playback);
    fHost.repaintSamplerInput();
    return true;
}

} // namespace midichopper::ui

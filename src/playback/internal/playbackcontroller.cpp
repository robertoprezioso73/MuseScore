/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-Studio-CLA-applies
 *
 * MuseScore Studio
 * Music Composition & Notation
 *
 * Copyright (C) 2025 MuseScore Limited and others
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#include "playbackcontroller.h"

#include "async/notifylist.h"

#include "modularity/ioc.h"
#include "onlinesoundscontroller.h"
#include "midirecordingcontroller.h"

#include "playbacktypes.h"

#include "engraving/dom/masterscore.h"
#include "engraving/dom/chord.h"
#include "engraving/dom/measure.h"
#include "engraving/dom/note.h"
#include "engraving/dom/staff.h"
#include "engraving/dom/stafftext.h"
#include "engraving/dom/utils.h"
#include "engraving/dom/factory.h"

#include "audio/common/audioutils.h"
#include "audio/devtools/inputlag.h"

#include "containers.h"
#include "defer.h"
#include "log.h"

using namespace muse;
using namespace muse::actions;
using namespace muse::async;
using namespace muse::audio;
using namespace muse::midi;
using namespace mu::engraving;
using namespace mu::notation;
using namespace mu::playback;

static const muse::Uri NOTATION_REVIEW_PAGE_URI("musescore://notation/review");

static const ActionCode PLAY_CODE("play");
static const ActionCode PLAY_FROM_SELECTION("play-from-selection");
static const ActionCode PAUSE_CODE("pause");
static const ActionCode STOP_CODE("stop");
static const ActionCode PAUSE_AND_SELECT_CODE("pause-and-select");
static const ActionCode REWIND_CODE("rewind");
static const ActionCode LOOP_CODE("loop");
static const ActionCode LOOP_IN_CODE("loop-in");
static const ActionCode LOOP_OUT_CODE("loop-out");
static const ActionCode METRONOME_CODE("metronome");
static const ActionCode MIDI_ON_CODE("midi-on");
static const ActionCode INPUT_WRITTEN_PITCH("midi-input-written-pitch");
static const ActionCode INPUT_SOUNDING_PITCH("midi-input-sounding-pitch");
static const ActionCode COUNT_IN_CODE("countin");
static const ActionCode MIDI_RECORD_CODE("midi-record");
static const ActionCode MIDI_RECORD_REQUANTIZE_CODE("midi-record-requantize");
static const ActionCode MIDI_RECORD_TUPLETS_CODE("midi-record-tuplets");
static const ActionCode MIDI_RECORD_LEGATO_CODE("midi-record-legato");
static const ActionCode MIDI_RECORD_LATENCY_CODE("midi-record-latency");
static const ActionCode MIDI_RECORD_ONE_STAFF_CODE("midi-record-staves-1");
static const ActionCode MIDI_RECORD_TWO_STAVES_CODE("midi-record-staves-2");
static const ActionCode MIDI_RECORD_SPLIT_POINT_CODE("midi-record-split-point");

//! <action code, minimum quantization in ticks>
static const std::vector<std::pair<ActionCode, int> > MIDI_RECORD_QUANTIZATION_CODES {
    { "midi-record-quantization-4", Constants::DIVISION },
    { "midi-record-quantization-8", Constants::DIVISION / 2 },
    { "midi-record-quantization-16", Constants::DIVISION / 4 },
    { "midi-record-quantization-32", Constants::DIVISION / 8 },
};

//! <action code, count-in measures>
static const std::vector<std::pair<ActionCode, int> > MIDI_RECORD_COUNT_IN_CODES {
    { "midi-record-count-in-0", 0 },
    { "midi-record-count-in-1", 1 },
    { "midi-record-count-in-2", 2 },
};
static const ActionCode PAN_CODE("pan");
static const ActionCode REPEAT_CODE("repeat");
static const ActionCode PLAY_CHORD_SYMBOLS_CODE("play-chord-symbols");
static const ActionCode PLAYBACK_SETUP("playback-setup");
static const ActionCode TOGGLE_HEAR_PLAYBACK_WHEN_EDITING_CODE("toggle-hear-playback-when-editing");

static AudioOutputParams makeReverbOutputParams()
{
    AudioFxParams reverbParams;
    reverbParams.resourceMeta = makeReverbMeta();
    reverbParams.categories.insert(AudioFxCategory::FxReverb);
    reverbParams.chainOrder = 0;
    reverbParams.active = true;

    AudioOutputParams result;
    result.fxChain.emplace(reverbParams.chainOrder, std::move(reverbParams));

    return result;
}

static std::string resolveAuxTrackTitle(aux_channel_idx_t index, const AudioOutputParams& params, bool considerFx = true)
{
    if (considerFx && params.fxChain.size() == 1) {
        const AudioResourceMeta& meta = params.fxChain.cbegin()->second.resourceMeta;
        if (meta.id == MUSE_REVERB_ID) {
            return muse::trc("playback", "Reverb");
        }

        return meta.id;
    }

    return muse::mtrc("playback", "Aux %1").arg(index + 1).toStdString();
}

PlaybackController::PlaybackController(const muse::modularity::ContextPtr& iocCtx)
    : muse::Contextable(iocCtx), m_drumsetLoader(iocCtx), m_onlineSoundsController(std::make_unique<OnlineSoundsController>(iocCtx)),
    m_midiRecordingController(std::make_unique<MidiRecordingController>(iocCtx))
{
}

PlaybackController::~PlaybackController() = default;

void PlaybackController::init()
{
    dispatcher()->reg(this, PLAY_CODE, [this]() { PlaybackController::togglePlay(); });
    dispatcher()->reg(this, PLAY_FROM_SELECTION, [this]() { PlaybackController::playFromSelection(); });
    dispatcher()->reg(this, PAUSE_CODE, [this]() { PlaybackController::pause(/*select*/ false); });
    dispatcher()->reg(this, PAUSE_AND_SELECT_CODE, [this]() { PlaybackController::pause(/*select*/ true); });
    dispatcher()->reg(this, STOP_CODE, this, &PlaybackController::stop);
    dispatcher()->reg(this, REWIND_CODE, this, &PlaybackController::rewind);
    dispatcher()->reg(this, LOOP_CODE, this, &PlaybackController::toggleLoopPlayback);
    dispatcher()->reg(this, LOOP_IN_CODE, [this]() { addLoopBoundary(LoopBoundaryType::LoopIn); });
    dispatcher()->reg(this, LOOP_OUT_CODE, [this]() { addLoopBoundary(LoopBoundaryType::LoopOut); });
    dispatcher()->reg(this, REPEAT_CODE, this, &PlaybackController::togglePlayRepeats);
    dispatcher()->reg(this, PLAY_CHORD_SYMBOLS_CODE, this, &PlaybackController::togglePlayChordSymbols);
    dispatcher()->reg(this, PAN_CODE, this, &PlaybackController::toggleAutomaticallyPan);
    dispatcher()->reg(this, METRONOME_CODE, this, &PlaybackController::toggleMetronome);
    dispatcher()->reg(this, COUNT_IN_CODE, this, &PlaybackController::toggleCountIn);
    dispatcher()->reg(this, MIDI_ON_CODE, this, &PlaybackController::toggleMidiInput);
    dispatcher()->reg(this, INPUT_WRITTEN_PITCH, [this]() { PlaybackController::setMidiUseWrittenPitch(true); });
    dispatcher()->reg(this, INPUT_SOUNDING_PITCH, [this]() { PlaybackController::setMidiUseWrittenPitch(false); });
    dispatcher()->reg(this, PLAYBACK_SETUP, this, &PlaybackController::openPlaybackSetupDialog);
    dispatcher()->reg(this, TOGGLE_HEAR_PLAYBACK_WHEN_EDITING_CODE, this, &PlaybackController::toggleHearPlaybackWhenEditing);
    dispatcher()->reg(this, "playback-reload-cache", this, &PlaybackController::reloadPlaybackCache);
    dispatcher()->reg(this, MIDI_RECORD_CODE, this, &PlaybackController::toggleMidiRecording);
    dispatcher()->reg(this, MIDI_RECORD_REQUANTIZE_CODE, this, &PlaybackController::requantizeLastMidiRecording);
    dispatcher()->reg(this, MIDI_RECORD_TUPLETS_CODE, this, &PlaybackController::toggleMidiRecordingTuplets);
    dispatcher()->reg(this, MIDI_RECORD_LEGATO_CODE, this, &PlaybackController::toggleMidiRecordingLegato);
    dispatcher()->reg(this, MIDI_RECORD_LATENCY_CODE, this, &PlaybackController::openMidiRecordingLatencySettings);
    dispatcher()->reg(this, MIDI_RECORD_ONE_STAFF_CODE, [this]() { setMidiRecordingTwoStaves(false); });
    dispatcher()->reg(this, MIDI_RECORD_TWO_STAVES_CODE, [this]() { setMidiRecordingTwoStaves(true); });
    dispatcher()->reg(this, MIDI_RECORD_SPLIT_POINT_CODE, this, &PlaybackController::openMidiRecordingSplitPointDialog);

    for (const auto& [code, ticks] : MIDI_RECORD_QUANTIZATION_CODES) {
        dispatcher()->reg(this, code, [this, ticks = ticks]() { setMidiRecordingMinQuantization(ticks); });
    }

    for (const auto& [code, measures] : MIDI_RECORD_COUNT_IN_CODES) {
        dispatcher()->reg(this, code, [this, measures = measures]() { setMidiRecordingCountInMeasures(measures); });
    }

    m_onlineSoundsController->regActions();
    m_midiRecordingController->init();
    m_midiRecordingController->setPlayNoteCallback([this](int pitch, int velocity) {
        playMidiRecordingNote(pitch, velocity);
    });
    m_midiRecordingController->lastRecordingChanged().onNotify(this, [this]() {
        m_lastMidiRecordingChanged.notify();
    });

    globalContext()->currentNotationChanged().onNotify(this, [this]() {
        onNotationChanged();
    });

    globalContext()->currentProjectChanged().onNotify(this, [this]() {
        m_midiRecordingController->clearLastRecording();

        if (m_currentSequenceId != -1) {
            resetCurrentSequence();
        }

        if (!globalContext()->currentProject()) {
            return;
        }

        m_loadingProgress.start();

        playback()->addSequence().onResolve(this, [this](const TrackSequenceId& sequenceId) {
            setupNewCurrentSequence(sequenceId);
        });
    });

    m_totalPlayTimeChanged.onNotify(this, [this]() {
        updateCurrentTempo();

        updateLoop();
    });

    notationConfiguration()->isMidiInputEnabledChanged().onNotify(this, [this]() {
        notifyActionCheckedChanged(MIDI_ON_CODE);
    });

    configuration()->playNotesWhenEditingChanged().onNotify(this, [this]() {
        notifyActionCheckedChanged(TOGGLE_HEAR_PLAYBACK_WHEN_EDITING_CODE);
    });

    m_measureInputLag = configuration()->shouldMeasureInputLag();
}

void PlaybackController::updateCurrentTempo()
{
    if (!notationPlayback()) {
        return;
    }

    const Tempo& newTempo = notationPlayback()->multipliedTempo(m_currentTick);

    if (newTempo == m_currentTempo) {
        return;
    }

    m_currentTempo = newTempo;
    m_currentTempoChanged.notify();
}

bool PlaybackController::isPlayAllowed() const
{
    bool allowed = m_currentSequenceId != -1 && m_notation != nullptr && m_notation->hasVisibleParts() && isLoaded();
    return allowed;
}

Notification PlaybackController::isPlayAllowedChanged() const
{
    return m_isPlayAllowedChanged;
}

bool PlaybackController::isPlaying() const
{
    if (!currentPlayer()) {
        return false;
    }
    return currentPlayer()->playbackStatus() == PlaybackStatus::Running;
}

bool PlaybackController::isPaused() const
{
    if (!currentPlayer()) {
        return false;
    }
    return currentPlayer()->playbackStatus() == PlaybackStatus::Paused;
}

bool PlaybackController::isLoaded() const
{
    return m_loadingTrackCount == 0;
}

bool PlaybackController::isLoopEnabled() const
{
    return notationPlayback() && notationPlayback()->isLoopEnabled();
}

bool PlaybackController::loopBoundariesSet() const
{
    return notationPlayback() && !notationPlayback()->loopBoundaries().isNull();
}

Notification PlaybackController::isPlayingChanged() const
{
    return m_isPlayingChanged;
}

void PlaybackController::reset()
{
    stop();
}

void PlaybackController::seekRawTick(const midi::tick_t tick, const bool flushSound)
{
    if (m_currentTick == tick) {
        return;
    }

    RetVal<midi::tick_t> playedTick = notationPlayback()->playPositionTickByRawTick(tick);
    if (!playedTick.ret) {
        return;
    }

    seek(playedTickToSecs(playedTick.val), flushSound);
}

void PlaybackController::seek(const audio::secs_t secs, const bool flushSound)
{
    IF_ASSERT_FAILED(currentPlayer()) {
        return;
    }

    currentPlayer()->seek(secs, flushSound);
}

muse::async::Channel<secs_t, tick_t> PlaybackController::currentPlaybackPositionChanged() const
{
    return m_currentPlaybackPositionChanged;
}

TrackSequenceId PlaybackController::currentTrackSequenceId() const
{
    return m_currentSequenceId;
}

Notification PlaybackController::currentTrackSequenceIdChanged() const
{
    return m_currentSequenceIdChanged;
}

const IPlaybackController::InstrumentTrackIdMap& PlaybackController::instrumentTrackIdMap() const
{
    return m_instrumentTrackIdMap;
}

const IPlaybackController::AuxTrackIdMap& PlaybackController::auxTrackIdMap() const
{
    return m_auxTrackIdMap;
}

Channel<TrackId> PlaybackController::trackAdded() const
{
    return m_trackAdded;
}

Channel<TrackId> PlaybackController::trackRemoved() const
{
    return m_trackRemoved;
}

std::string PlaybackController::auxChannelName(aux_channel_idx_t index) const
{
    return resolveAuxTrackTitle(index, audioSettings()->auxOutputParams(index));
}

Channel<aux_channel_idx_t, std::string> PlaybackController::auxChannelNameChanged() const
{
    return m_auxChannelNameChanged;
}

Promise<SoundPresetList> PlaybackController::availableSoundPresets(const InstrumentTrackId& instrumentTrackId) const
{
    auto it = m_instrumentTrackIdMap.find(instrumentTrackId);
    if (it == m_instrumentTrackIdMap.end()) {
        return Promise<SoundPresetList>([](auto, auto reject) {
            return reject(static_cast<int>(Ret::Code::UnknownError), "invalid instrumentTrackId");
        });
    }

    const AudioInputParams& params = audioSettings()->trackInputParams(instrumentTrackId);
    return playback()->availableSoundPresets(params.resourceMeta);
}

const PlaybackController::SoloMuteState& PlaybackController::trackSoloMuteState(const InstrumentTrackId& trackId) const
{
    return m_notation->soloMuteState()->trackSoloMuteState(trackId);
}

void PlaybackController::setTrackSoloMuteState(const InstrumentTrackId& trackId, const SoloMuteState& state)
{
    if (trackId == notationPlayback()->metronomeTrackId()) {
        if (state.mute != notationConfiguration()->isMetronomeEnabled()) {
            return;
        }

        toggleMetronome();
        return;
    }

    m_notation->soloMuteState()->setTrackSoloMuteState(trackId, state);
}

void PlaybackController::playElements(const std::vector<const notation::EngravingItem*>& elements, const PlayParams& params, bool isMidi)
{
    IF_ASSERT_FAILED(notationPlayback()) {
        return;
    }

    if ((!configuration()->playNotesWhenEditing()) || (isMidi && !configuration()->playNotesOnMidiInput())) {
        return;
    }

    if (isMidiRecording()) {
        //! NOTE During a MIDI recording the keys are played by playMidiRecordingNote, with the instrument of the recorded staff
        if (isMidi) {
            return;
        }
    } else if (isPlaying()) {
        //! NOTE The audio engine plays these events also during playback (see AbstractEventSequencer::movePlaybackForward),
        //! but this was not the case before: keep the previous behaviour outside of MIDI recording
        return;
    }

    if (m_measureInputLag) {
        START_INPUT_LAG_TIMER;
    }

    std::vector<const notation::EngravingItem*> elementsForPlaying;
    elementsForPlaying.reserve(elements.size());

    bool playChordWhenEditing = configuration()->playChordWhenEditing();
    bool playHarmonyWhenEditing = configuration()->playHarmonyWhenEditing();

    for (const EngravingItem* element : elements) {
        IF_ASSERT_FAILED(element) {
            continue;
        }

        if (!element->isPlayable()) {
            continue;
        }

        if (element->isChord() && !playChordWhenEditing) {
            continue;
        }

        if (element->isHarmony() && !playHarmonyWhenEditing) {
            continue;
        }

        elementsForPlaying.push_back(element);
    }

    const mpe::duration_t duration = params.duration.has_value() ? params.duration.value()
                                     : notationConfiguration()->notePlayDurationMilliseconds() * 1000;

    notationPlayback()->triggerEventsForItems(elementsForPlaying, duration, params.flushSound);
}

void PlaybackController::playNotes(const NoteValList& notes, staff_idx_t staffIdx, const Segment* segment, const PlayParams& params)
{
    Segment* seg = const_cast<Segment*>(segment);
    Chord* chord = engraving::Factory::createChord(seg);
    chord->setParent(seg);

    std::vector<const EngravingItem*> elements;
    elements.reserve(notes.size());

    for (const NoteVal& nval : notes) {
        Note* note = engraving::Factory::createNote(chord);
        note->setParent(chord);
        note->setStaffIdx(staffIdx);
        note->setNval(nval);
        elements.push_back(note);
    }

    playElements(elements, params);

    delete chord;
    DeleteAll(elements);
}

void PlaybackController::playMetronome(int tick)
{
    notationPlayback()->triggerMetronome(tick);
}

void PlaybackController::triggerControllers(const muse::mpe::ControllerChangeEventList& list, staff_idx_t staffIdx, int tick)
{
    if (isPlaying() && !isMidiRecording()) {
        return; // see playElements
    }

    notationPlayback()->triggerControllers(list, staffIdx, tick);
}

void PlaybackController::seekElement(const notation::EngravingItem* element, bool flushSound)
{
    IF_ASSERT_FAILED(notationPlayback()) {
        return;
    }

    if (!element) {
        return;
    }

    RetVal<midi::tick_t> tick = notationPlayback()->playPositionTickByElement(element);
    if (!tick.ret) {
        return;
    }

    seek(playedTickToSecs(tick.val), flushSound);
}

void PlaybackController::seekBeat(int measureIndex, int beatIndex, bool flushSound)
{
    secs_t targetSecs = beatToSecs(measureIndex, beatIndex);
    seek(targetSecs, flushSound);
}

void PlaybackController::seekRangeSelection()
{
    if (!selection()->isRange()) {
        return;
    }

    midi::tick_t startTick = selectionRange()->startTick().ticks();

    seekRawTick(startTick);
}

void PlaybackController::onAudioResourceChanged(const TrackId trackId, const InstrumentTrackId& instrumentTrackId,
                                                const AudioResourceMeta& oldMeta, const AudioResourceMeta& newMeta)
{
    INotationPlaybackPtr notationPlayback = this->notationPlayback();
    if (!notationPlayback) {
        return;
    }

    if (oldMeta.type == newMeta.type && oldMeta.id == newMeta.id) {
        return;
    }

    if (shouldLoadDrumset(instrumentTrackId, oldMeta, newMeta)) {
        m_drumsetLoader.loadDrumset(m_notation, instrumentTrackId, newMeta);
    }

    notationPlayback->removeSoundFlags({ instrumentTrackId });

    if (audio::isOnlineAudioResource(newMeta)) {
        m_onlineSoundsController->addOnlineTrack(trackId, newMeta);
        tours()->onEvent(u"online_sounds_added");
        notationPlayback->setSendEventsOnScoreChange(instrumentTrackId, true);
    } else if (audio::isOnlineAudioResource(oldMeta)) {
        m_onlineSoundsController->removeOnlineTrack(trackId);
        notationPlayback->setSendEventsOnScoreChange(instrumentTrackId, false);
    }
}

bool PlaybackController::shouldLoadDrumset(const engraving::InstrumentTrackId& instrumentTrackId,
                                           const AudioResourceMeta& oldMeta, const AudioResourceMeta& newMeta) const
{
    if (oldMeta.type == newMeta.type && oldMeta.id == newMeta.id) {
        return false;
    }

    const Part* part = masterNotationParts()->part(instrumentTrackId.partId);
    const Instrument* instrument = part ? part->instrumentById(instrumentTrackId.instrumentId) : nullptr;
    if (!instrument || !instrument->useDrumset()) {
        return false;
    }

    return oldMeta.type == AudioResourceType::MuseSamplerSoundPack || newMeta.type == AudioResourceType::MuseSamplerSoundPack;
}

void PlaybackController::addSoundFlagsIfNeed(const std::vector<EngravingItem*>& selection)
{
    if (selection.empty()) {
        return;
    }

    std::vector<StaffText*> staffTextList;

    for (EngravingItem* item : selection) {
        if (!item || !item->isStaffText()) {
            continue;
        }

        InstrumentTrackId trackId = mu::engraving::makeInstrumentTrackId(item);
        const AudioInputParams& params = audioSettings()->trackInputParams(trackId);

        bool supportsSoundFlags = params.type() == AudioSourceType::MuseSampler;
        if (supportsSoundFlags) {
            staffTextList.push_back(toStaffText(item));
        }
    }

    if (!staffTextList.empty()) {
        notationPlayback()->addSoundFlags(staffTextList);
    }
}

muse::audio::IPlayerPtr PlaybackController::currentPlayer() const
{
    return m_player;
}

INotationPlaybackPtr PlaybackController::notationPlayback() const
{
    return m_masterNotation ? m_masterNotation->playback() : nullptr;
}

INotationPartsPtr PlaybackController::masterNotationParts() const
{
    return m_masterNotation ? m_masterNotation->parts() : nullptr;
}

INotationSelectionPtr PlaybackController::selection() const
{
    return m_notation ? m_notation->interaction()->selection() : nullptr;
}

INotationSelectionRangePtr PlaybackController::selectionRange() const
{
    INotationSelectionPtr selection = this->selection();
    return selection ? selection->range() : nullptr;
}

INotationInteractionPtr PlaybackController::interaction() const
{
    return m_notation ? m_notation->interaction() : nullptr;
}

uint64_t PlaybackController::notationPlaybackKey() const
{
    return reinterpret_cast<uint64_t>(notationPlayback().get());
}

void PlaybackController::onNotationChanged()
{
    cancelMidiRecording();
    setNotation(globalContext()->currentNotation());
}

void PlaybackController::onPartChanged(const Part* part)
{
    if (!m_notation->hasVisibleParts()) {
        pause();
    }
    m_isPlayAllowedChanged.notify();

    if (!configuration()->muteHiddenInstruments()) {
        return;
    }

    for (const InstrumentTrackId& instrumentTrackId : part->instrumentTrackIdList()) {
        auto soloMuteState = trackSoloMuteState(instrumentTrackId);
        soloMuteState.mute = !part->show();
        setTrackSoloMuteState(instrumentTrackId, soloMuteState);
    }

    if (part->hasChordSymbol()) {
        InstrumentTrackId chordSymbolsTrackId = notationPlayback()->chordSymbolsTrackId(part->id());
        auto chordsSoloMuteState = trackSoloMuteState(chordSymbolsTrackId);
        chordsSoloMuteState.mute = !part->show();
        setTrackSoloMuteState(chordSymbolsTrackId, chordsSoloMuteState);
    }
}

void PlaybackController::onSelectionChanged()
{
    const INotationSelectionPtr selection = this->selection();
    if (!selection || !m_player) {
        return;
    }

    bool selectionTypeChanged = m_isRangeSelection && !selection->isRange();
    m_isRangeSelection = selection->isRange();

    if (!m_isRangeSelection) {
        if (selectionTypeChanged) {
            updateLoop();
            updateSoloMuteStates();
        }

        addSoundFlagsIfNeed(selection->elements());
        return;
    }

    m_player->resetLoop();

    seekRangeSelection();
    updateSoloMuteStates();
}

void PlaybackController::togglePlay(bool showErrors)
{
    if (!isPlayAllowed()) {
        LOGW() << "playback not allowed";
        return;
    }

    if (showErrors && m_onlineSoundsController->shouldShowOnlineSoundsProcessingError(isPlaying())) {
        m_onlineSoundsController->showOnlineSoundsProcessingError([this]() { togglePlay(false /*showErrors*/); });
        return;
    }

    interaction()->endEditElement();

    if (isPlaying()) {
        pause();
    } else if (isPaused()) {
        notationPlayback()->sendEventsForChangedTracks();

        if (currentPlayer()) {
            secs_t pos = currentPlayer()->playbackPosition();
            secs_t endSecs = totalPlayTime();
            if (pos == endSecs) {
                secs_t startSecs = playbackStartSecs();
                seek(startSecs);
            }

            resume();
        }
    } else {
        notationPlayback()->sendEventsForChangedTracks();

        play();
    }
}

void PlaybackController::playFromSelection(bool showErrors)
{
    if (showErrors && m_onlineSoundsController->shouldShowOnlineSoundsProcessingError(isPlaying())) {
        m_onlineSoundsController->showOnlineSoundsProcessingError([this]() { playFromSelection(false /*showErrors*/); });
        return;
    }

    int startTick = 0;

    if (!selection()->isNone()) {
        startTick = INT_MAX;
        for (const EngravingItem* item : selection()->elements()) {
            startTick = std::min(startTick, item->tick().ticks());
        }
    } else {
        // Selection is none - fall back to last element hit...
        if (const EngravingItem* lastElementHit = selection()->lastElementHit()) {
            startTick = lastElementHit->tick().ticks();
        }
    }

    const LoopBoundaries& loop = notationPlayback()->loopBoundaries();
    if (loop.enabled) {
        if (startTick < loop.loopInTick.ticks() || startTick > loop.loopOutTick.ticks()) {
            startTick = loop.loopInTick.ticks();
        }
    }

    const RetVal<midi::tick_t> retval = notationPlayback()->playPositionTickByRawTick(startTick);
    if (!retval.ret) {
        return;
    }

    seek(playedTickToSecs(retval.val));

    if (isPaused()) {
        resume();
    } else if (!isPlaying()) {
        play();
    }
}

void PlaybackController::play()
{
    IF_ASSERT_FAILED(currentPlayer()) {
        return;
    }

    if (isLoopEnabled() && !isMidiRecording()) {
        secs_t startSecs = playbackStartSecs();
        seek(startSecs);
    }

    currentPlayer()->prepareToPlay().onResolve(this, [this](const Ret& ret) {
        if (!currentPlayer()) {
            return;
        }

        if (!ret) {
            LOGE() << ret.toString();
        }

        secs_t delay = 0.;
        if (isMidiRecording()) {
            //! NOTE The count-in of the MIDI recording has its own setting (0, 1 or 2 measures)
            const int countInMeasures = notationConfiguration()->midiRecordingCountInMeasures();
            if (countInMeasures > 0) {
                notationPlayback()->triggerCountIn(m_currentTick, delay, countInMeasures);
            }
        } else if (notationConfiguration()->isCountInEnabled()) {
            notationPlayback()->triggerCountIn(m_currentTick, delay);
        }

        if (isMidiRecording()) {
            const auto countInDuration = std::chrono::duration_cast<MidiRecordingController::Clock::duration>(
                std::chrono::duration<double>(static_cast<double>(delay)));
            m_midiRecordingController->setMusicStartTime(MidiRecordingController::Clock::now() + countInDuration);
        }

        currentPlayer()->play(delay);
    });
}

void PlaybackController::rewind(const ActionData& args)
{
    secs_t startSecs = playbackStartSecs();
    secs_t endSecs = totalPlayTime();
    secs_t newPosition = !args.empty() ? args.arg<secs_t>(0) : secs_t{ 0 };
    newPosition = std::clamp(newPosition, startSecs, endSecs);

    seek(newPosition);
}

void PlaybackController::pause(bool select)
{
    IF_ASSERT_FAILED(currentPlayer()) {
        return;
    }

    if (isPaused()) {
        return;
    }

    currentPlayer()->pause();

    if (select && m_notation) {
        const Fraction playPositionFrac = Fraction::fromTicks(m_currentTick);
        interaction()->findAndSelectChordRest(playPositionFrac);
    }
}

void PlaybackController::stop()
{
    IF_ASSERT_FAILED(currentPlayer()) {
        return;
    }

    currentPlayer()->stop();
}

void PlaybackController::resume()
{
    IF_ASSERT_FAILED(currentPlayer()) {
        return;
    }

    currentPlayer()->prepareToPlay().onResolve(this, [this](const Ret& ret) {
        if (!currentPlayer()) {
            return;
        }

        if (!ret) {
            LOGE() << ret.toString();
        }

        secs_t delay = 0.;
        if (notationConfiguration()->isCountInEnabled()) {
            notationPlayback()->triggerCountIn(m_currentTick, delay);
        }

        currentPlayer()->resume(delay);
    });
}

void PlaybackController::onPlaybackStatusChanged()
{
    if (!notationPlayback()) {
        return;
    }

    bool playing = isPlaying();
    const auto& onlineSounds = m_onlineSoundsController->onlineSounds();

    for (const auto& pair : m_instrumentTrackIdMap) {
        bool shouldSendOnScoreChange = playing || muse::contains(onlineSounds, pair.second);
        notationPlayback()->setSendEventsOnScoreChange(pair.first, shouldSendOnScoreChange);
    }
}

secs_t PlaybackController::playbackStartSecs() const
{
    if (!m_notation) {
        return 0;
    }

    const LoopBoundaries& loop = notationPlayback()->loopBoundaries();
    if (loop.enabled) {
        // Convert from raw ticks (visual tick != playback tick due to repeats etc)
        RetVal<tick_t> startTick = notationPlayback()->playPositionTickByRawTick(loop.loopInTick.ticks());
        if (!startTick.ret) {
            return 0;
        }
        return playedTickToSecs(startTick.val);
    }

    return 0;
}

InstrumentTrackIdSet PlaybackController::instrumentTrackIdSetForRangePlayback() const
{
    std::vector<const Part*> selectedParts = selectionRange()->selectedParts();
    Fraction startTick = selectionRange()->startTick();
    int startTicks = startTick.ticks();

    InstrumentTrackIdSet result;

    for (const Part* part : selectedParts) {
        if (const Instrument* startInstrument = part->instrument(startTick)) {
            result.insert({ part->id(), startInstrument->id() });
        }

        for (auto [tick, instrument] : part->instruments()) {
            if (tick > startTicks) {
                result.insert({ part->id(), instrument->id() });
            }
        }

        if (part->hasChordSymbol()) {
            result.insert(notationPlayback()->chordSymbolsTrackId(part->id()));
        }
    }

    return result;
}

void PlaybackController::togglePlayRepeats()
{
    bool playRepeatsEnabled = notationConfiguration()->isPlayRepeatsEnabled();
    notationConfiguration()->setIsPlayRepeatsEnabled(!playRepeatsEnabled);
    notifyActionCheckedChanged(REPEAT_CODE);
}

void PlaybackController::togglePlayChordSymbols()
{
    bool playChordSymbolsEnabled = notationConfiguration()->isPlayChordSymbolsEnabled();
    notationConfiguration()->setIsPlayChordSymbolsEnabled(!playChordSymbolsEnabled);
    notifyActionCheckedChanged(PLAY_CHORD_SYMBOLS_CODE);

    for (auto it = m_instrumentTrackIdMap.cbegin(); it != m_instrumentTrackIdMap.cend(); ++it) {
        if (notationPlayback()->isChordSymbolsTrack(it->first)) {
            setTrackActivity(it->first, !playChordSymbolsEnabled);
        }
    }
}

void PlaybackController::toggleAutomaticallyPan()
{
    bool panEnabled = notationConfiguration()->isAutomaticallyPanEnabled();
    notationConfiguration()->setIsAutomaticallyPanEnabled(!panEnabled);
    notifyActionCheckedChanged(PAN_CODE);
}

void PlaybackController::toggleMetronome()
{
    bool metronomeEnabled = notationConfiguration()->isMetronomeEnabled();
    bool countInEnabled = notationConfiguration()->isCountInEnabled();

    notationConfiguration()->setIsMetronomeEnabled(!metronomeEnabled);
    notifyActionCheckedChanged(METRONOME_CODE);

    setTrackActivity(notationPlayback()->metronomeTrackId(), !metronomeEnabled || countInEnabled);
}

void PlaybackController::toggleCountIn()
{
    bool metronomeEnabled = notationConfiguration()->isMetronomeEnabled();
    bool countInEnabled = notationConfiguration()->isCountInEnabled();

    notationConfiguration()->setIsCountInEnabled(!countInEnabled);
    notifyActionCheckedChanged(COUNT_IN_CODE);

    setTrackActivity(notationPlayback()->metronomeTrackId(), metronomeEnabled || !countInEnabled);
}

bool PlaybackController::isMidiRecording() const
{
    return m_midiRecordingController && m_midiRecordingController->isRecording();
}

void PlaybackController::toggleMidiRecording()
{
    if (isMidiRecording()) {
        // The recorded notes are written when the playback stops (see onMidiRecordingPlaybackStatusChanged)
        stop();
        return;
    }

    startMidiRecording();
}

void PlaybackController::startMidiRecording()
{
    if (!isPlayAllowed() || !currentPlayer() || !m_notation) {
        LOGW() << "playback not allowed";
        return;
    }

    if (!notationConfiguration()->isMidiInputEnabled()) {
        interactive()->info(muse::trc("playback", "MIDI input is disabled"),
                            muse::trc("playback", "Enable MIDI input in the playback settings to record from a MIDI keyboard."));
        return;
    }

    if (isLoopEnabled()) {
        interactive()->info(muse::trc("playback", "Loop playback is enabled"),
                            muse::trc("playback", "Disable loop playback to record from a MIDI keyboard."));
        return;
    }

    // The recording starts at the beginning of the selected measure, on the selected staff
    staff_idx_t staffIdx = muse::nidx;
    Fraction selectionTick;

    if (selection()->isRange()) {
        staffIdx = selectionRange()->startStaffIndex();
        selectionTick = selectionRange()->startTick();
    } else if (const EngravingItem* element = selection()->element()) {
        staffIdx = element->staffIdx();
        selectionTick = element->tick();
    }

    const Score* score = m_notation->elements()->msScore();
    const Measure* measure = (score && staffIdx != muse::nidx) ? score->tick2measure(selectionTick) : nullptr;
    const Staff* staff = measure ? score->staff(staffIdx) : nullptr;

    if (!measure || !staff) {
        interactive()->info(muse::trc("playback", "No measure selected"),
                            muse::trc("playback", "Select the measure where the recording should start, then press Record."));
        return;
    }

    if (staff->isDrumStaff(measure->tick())) {
        interactive()->info(muse::trc("playback", "Percussion staff selected"),
                            muse::trc("playback", "MIDI recording on percussion staves is not supported yet."));
        return;
    }

    // Two staves: the lower notes go to the staff below, which must belong to the same instrument
    staff_idx_t lowerStaffIdx = muse::nidx;
    if (notationConfiguration()->midiRecordingTwoStaves()) {
        const Staff* lowerStaff = staffIdx + 1 < score->nstaves() ? score->staff(staffIdx + 1) : nullptr;
        if (!lowerStaff || lowerStaff->part() != staff->part() || !lowerStaff->isPitchedStaff(measure->tick())) {
            interactive()->info(muse::trc("playback", "No staff below for the split"),
                                muse::trc("playback", "Recording on two staves needs a staff of the same instrument below the selected one "
                                                      "(e.g. the left hand of a piano). Select the upper staff, "
                                                      "or choose “One staff” in the Recording menu."));
            return;
        }

        lowerStaffIdx = staffIdx + 1;
    }

    const RetVal<tick_t> startPlayedTick = notationPlayback()->playPositionTickByRawTick(measure->tick().ticks());
    if (!startPlayedTick.ret) {
        return;
    }

    if (isPlaying() || isPaused()) {
        stop();
    }

    // Avoid that the notes played on the keyboard start note input on the selection:
    // during the recording, the keyboard only plays the sounds
    INotationInteractionPtr notationInteraction = interaction();
    if (notationInteraction->noteInput()->isNoteInputMode()) {
        notationInteraction->noteInput()->endNoteInput();
    }
    notationInteraction->clearSelection();

    MidiRecordingController::Session session;
    session.notation = m_notation;
    session.notationPlayback = notationPlayback();
    session.staffIdx = staffIdx;
    session.startTick = measure->tick();
    session.startPlayedTick = startPlayedTick.val;
    session.startSecs = playedTickToSecs(startPlayedTick.val);
    session.lowerStaffIdx = lowerStaffIdx;
    session.splitPitch = notationConfiguration()->midiRecordingSplitPitch();

    const secs_t estimatedLatency = MidiRecordingController::estimateOutputLatency(audioConfiguration()->driverBufferSize(),
                                                                                    audioConfiguration()->sampleRate());
    const secs_t userCompensation = secs_t(notationConfiguration()->midiRecordingLatencyCompensationMilliseconds() / 1000.0);
    session.outputLatency = estimatedLatency + userCompensation;

    LOGI() << "MIDI recording: estimated output latency " << static_cast<double>(estimatedLatency) * 1000.0
           << " ms, user compensation " << static_cast<double>(userCompensation) * 1000.0 << " ms";

    seek(session.startSecs);
    m_currentTick = measure->tick().ticks(); // used by the count-in

    m_midiRecordingPlaybackStarted = false;
    m_midiRecordingController->start(session);
    forceMetronomeForMidiRecording();
    notifyActionCheckedChanged(MIDI_RECORD_CODE);

    notationPlayback()->sendEventsForChangedTracks();
    play();
}

void PlaybackController::onMidiRecordingPlaybackStatusChanged()
{
    if (!isMidiRecording()) {
        return;
    }

    if (isPlaying()) {
        m_midiRecordingPlaybackStarted = true;
        return;
    }

    if (!m_midiRecordingPlaybackStarted) {
        return; // e.g. the playback that was stopped before starting the recording
    }

    // The playback was stopped or paused (Stop, Space, Record, end of the score): end of the recording
    m_midiRecordingPlaybackStarted = false;
    restoreMetronomeAfterMidiRecording();

    if (isPaused()) {
        stop();
    }

    m_midiRecordingController->finishAndWrite();
    notifyActionCheckedChanged(MIDI_RECORD_CODE);
}

void PlaybackController::cancelMidiRecording()
{
    if (!isMidiRecording()) {
        return;
    }

    m_midiRecordingController->cancel();
    m_midiRecordingPlaybackStarted = false;
    restoreMetronomeAfterMidiRecording();
    notifyActionCheckedChanged(MIDI_RECORD_CODE);
}

void PlaybackController::forceMetronomeForMidiRecording()
{
    //! NOTE Only the audio track is unmuted: the metronome setting of the user is not changed
    setTrackActivity(notationPlayback()->metronomeTrackId(), true);
}

void PlaybackController::restoreMetronomeAfterMidiRecording()
{
    if (!notationPlayback()) {
        return;
    }

    const bool active = notationConfiguration()->isMetronomeEnabled() || notationConfiguration()->isCountInEnabled();
    setTrackActivity(notationPlayback()->metronomeTrackId(), active);
}

void PlaybackController::playMidiRecordingNote(int pitch, int velocity)
{
    const bool noteOn = velocity > 0;

    auto it = m_midiRecordingSoundingNotes.find(pitch);
    if (it != m_midiRecordingSoundingNotes.end()) {
        Note* note = it->second;
        Chord* chord = note->chord();
        m_midiRecordingSoundingNotes.erase(it);

        if (notationPlayback()) {
            note->setUserVelocity(0);
            notationPlayback()->triggerEventsForItems({ note }, 0 /*note off*/, false /*flushSound*/);
        }

        delete note;
        delete chord;
    }

    if (!noteOn || !notationPlayback()) {
        return;
    }

    const MidiRecordingController::Session& session = m_midiRecordingController->session();
    Score* score = session.notation ? session.notation->elements()->msScore() : nullptr;
    const Measure* measure = score ? score->tick2measure(session.startTick) : nullptr;
    Segment* segment = measure ? measure->first(SegmentType::ChordRest) : nullptr;
    if (!segment) {
        return;
    }

    // A temporary note on the recorded staff, so that it is played with the instrument of that staff
    const staff_idx_t staffIdx = session.staffForPitch(pitch);
    const track_idx_t track = staffIdx * VOICES;

    Chord* chord = Factory::createChord(segment);
    chord->setParent(segment);
    chord->setTrack(track);

    Note* note = Factory::createNote(chord);
    note->setParent(chord);
    note->setTrack(track);
    note->setNval(score->noteVal(pitch, staffIdx, false /*MIDI pitches are sounding pitches*/));
    note->setUserVelocity(velocity);

    notationPlayback()->triggerEventsForItems({ note }, muse::mpe::INFINITE_DURATION, false /*flushSound*/);

    m_midiRecordingSoundingNotes[pitch] = note;
}

const MidiRecordingPreviewNoteList& PlaybackController::midiRecordingPreviewNotes() const
{
    return m_midiRecordingController->previewNotes();
}

muse::async::Notification PlaybackController::midiRecordingPreviewNotesChanged() const
{
    return m_midiRecordingController->previewNotesChanged();
}

bool PlaybackController::canRequantizeLastMidiRecording() const
{
    return !isMidiRecording() && m_midiRecordingController->hasLastRecording(m_notation);
}

muse::async::Notification PlaybackController::lastMidiRecordingChanged() const
{
    return m_lastMidiRecordingChanged;
}

void PlaybackController::requantizeLastMidiRecording()
{
    if (!canRequantizeLastMidiRecording()) {
        interactive()->info(muse::trc("playback", "No MIDI recording to re-quantize"),
                            muse::trc("playback", "Record from a MIDI keyboard first: the last recording of this score can then be "
                                                  "written again with the current recording settings."));
        return;
    }

    if (isPlaying() || isPaused()) {
        stop();
    }

    m_midiRecordingController->requantizeLastRecording();
}

void PlaybackController::setMidiRecordingMinQuantization(int ticks)
{
    notationConfiguration()->setMidiRecordingMinQuantizationTicks(ticks);

    for (const auto& pair : MIDI_RECORD_QUANTIZATION_CODES) {
        notifyActionCheckedChanged(pair.first);
    }
}

void PlaybackController::setMidiRecordingCountInMeasures(int measures)
{
    notationConfiguration()->setMidiRecordingCountInMeasures(measures);

    for (const auto& pair : MIDI_RECORD_COUNT_IN_CODES) {
        notifyActionCheckedChanged(pair.first);
    }
}

void PlaybackController::toggleMidiRecordingTuplets()
{
    notationConfiguration()->setMidiRecordingSearchTuplets(!notationConfiguration()->midiRecordingSearchTuplets());
    notifyActionCheckedChanged(MIDI_RECORD_TUPLETS_CODE);
}

void PlaybackController::toggleMidiRecordingLegato()
{
    notationConfiguration()->setMidiRecordingLegato(!notationConfiguration()->midiRecordingLegato());
    notifyActionCheckedChanged(MIDI_RECORD_LEGATO_CODE);
}

void PlaybackController::setMidiRecordingTwoStaves(bool twoStaves)
{
    notationConfiguration()->setMidiRecordingTwoStaves(twoStaves);
    notifyActionCheckedChanged(MIDI_RECORD_ONE_STAFF_CODE);
    notifyActionCheckedChanged(MIDI_RECORD_TWO_STAVES_CODE);
}

void PlaybackController::openMidiRecordingSplitPointDialog()
{
    if (isMidiRecording()) {
        return;
    }

    interactive()->open("musescore://playback/midirecordingsplitpoint");
}

void PlaybackController::openMidiRecordingLatencySettings()
{
    if (isMidiRecording() || isPlaying()) {
        stop();
    }

    //! NOTE The latency compensation is in Preferences > Note input > MIDI devices > Recording with metronome
    UriQuery preferencesUri("muse://preferences");
    preferencesUri.addParam("currentPageId", Val("note-input"));
    interactive()->open(preferencesUri);
}

void PlaybackController::toggleMidiInput()
{
    bool wasMidiInputEnabled = notationConfiguration()->isMidiInputEnabled();
    notationConfiguration()->setIsMidiInputEnabled(!wasMidiInputEnabled);
}

void PlaybackController::setMidiUseWrittenPitch(bool useWrittenPitch)
{
    notationConfiguration()->setMidiUseWrittenPitch(useWrittenPitch);
    notifyActionCheckedChanged(INPUT_WRITTEN_PITCH);
    notifyActionCheckedChanged(INPUT_SOUNDING_PITCH);
}

void PlaybackController::toggleLoopPlayback()
{
    if (isLoopEnabled()) {
        disableLoop();
        return;
    }

    if (loopBoundariesSet() && !selection()->isRange()) {
        enableLoop();
        return;
    }

    int loopInTick = 0;
    int loopOutTick = 0;

    if (!selection()->isNone()) {
        loopInTick = selectionRange()->startTick().ticks();
        loopOutTick = selectionRange()->endTick().ticks();
    }

    if (loopInTick <= 0) {
        loopInTick = INotationPlayback::FirstScoreTick;
    }

    if (loopOutTick <= 0) {
        loopOutTick = INotationPlayback::LastScoreTick;
    }

    addLoopBoundaryToTick(LoopBoundaryType::LoopIn, loopInTick);
    addLoopBoundaryToTick(LoopBoundaryType::LoopOut, loopOutTick);
}

void PlaybackController::toggleHearPlaybackWhenEditing()
{
    bool wasPlayNotesWhenEditing = configuration()->playNotesWhenEditing();
    configuration()->setPlayNotesWhenEditing(!wasPlayNotesWhenEditing);
}

void PlaybackController::reloadPlaybackCache()
{
    INotationPlaybackPtr nPlayback = notationPlayback();
    if (nPlayback) {
        nPlayback->reload();
    }
}

void PlaybackController::openPlaybackSetupDialog()
{
    interactive()->open("musescore://playback/soundprofilesdialog");
}

void PlaybackController::addLoopBoundary(LoopBoundaryType type)
{
    if (isPlaying()) {
        addLoopBoundaryToTick(type, m_currentTick);
    } else {
        addLoopBoundaryToTick(type, INotationPlayback::SelectedNoteTick);
    }
}

void PlaybackController::addLoopBoundaryToTick(LoopBoundaryType type, int tick)
{
    if (notationPlayback()) {
        notationPlayback()->addLoopBoundary(type, tick);
        enableLoop();
    }
}

void PlaybackController::updateLoop()
{
    if (!notationPlayback() || !currentPlayer()) {
        return;
    }

    const LoopBoundaries& boundaries = notationPlayback()->loopBoundaries();

    if (!boundaries.enabled) {
        disableLoop();
        return;
    }

    // Convert from raw ticks (visual tick != playback tick due to repeats etc)
    RetVal<tick_t> playbackTickFrom = notationPlayback()->playPositionTickByRawTick(boundaries.loopInTick.ticks());
    RetVal<tick_t> playbackTickTo = notationPlayback()->playPositionTickByRawTick(boundaries.loopOutTick.ticks());
    if (!playbackTickFrom.ret || !playbackTickTo.ret) {
        return;
    }

    secs_t fromSecs = playedTickToSecs(playbackTickFrom.val);
    secs_t toSecs = playedTickToSecs(playbackTickTo.val);
    currentPlayer()->setLoop(secsToMilisecs(fromSecs), secsToMilisecs(toSecs));

    enableLoop();

    notifyActionCheckedChanged(LOOP_CODE);
}

void PlaybackController::enableLoop()
{
    if (notationPlayback()) {
        notationPlayback()->setLoopBoundariesEnabled(true);
    }
}

void PlaybackController::disableLoop()
{
    IF_ASSERT_FAILED(notationPlayback() && currentPlayer()) {
        return;
    }

    currentPlayer()->resetLoop();
    notationPlayback()->setLoopBoundariesEnabled(false);

    notifyActionCheckedChanged(LOOP_CODE);
}

void PlaybackController::notifyActionCheckedChanged(const ActionCode& actionCode)
{
    m_actionCheckedChanged.send(actionCode);
}

mu::project::IProjectAudioSettingsPtr PlaybackController::audioSettings() const
{
    IF_ASSERT_FAILED(globalContext()->currentProject()) {
        return nullptr;
    }

    return globalContext()->currentProject()->audioSettings();
}

void PlaybackController::resetCurrentSequence()
{
    if (currentPlayer()) {
        currentPlayer()->playbackPositionChanged().disconnect(this);
        currentPlayer()->playbackStatusChanged().disconnect(this);
    }

    playback()->clearSources();
    playback()->inputParamsChanged().disconnect(this);

    playback()->clearAllFx();
    playback()->outputParamsChanged().disconnect(this);
    playback()->masterOutputParamsChanged().disconnect(this);
    playback()->clearMasterOutputParams();

    m_seqAsyncReceiver.async_disconnectAll();

    m_currentTick = 0;

    playback()->removeSequence(m_currentSequenceId);

    m_instrumentTrackIdMap.clear();
    m_auxTrackIdMap.clear();

    m_isRangeSelection = false;

    m_currentSequenceId = -1;
    m_currentSequenceIdChanged.notify();

    m_player = nullptr;
    globalContext()->setCurrentPlayer(nullptr);

    m_onlineSoundsController->resetCurrentSequence();
}

void PlaybackController::addTrack(const InstrumentTrackId& instrumentTrackId, bool projectHadNoAudioSettings,
                                  const TrackAddFinished& onFinished)
{
    if (notationPlayback()->metronomeTrackId() == instrumentTrackId) {
        doAddTrack(instrumentTrackId, muse::trc("playback", "Metronome"), projectHadNoAudioSettings, onFinished);
        return;
    }

    const Part* part = masterNotationParts()->part(instrumentTrackId.partId);
    if (!part) {
        return;
    }

    if (notationPlayback()->isChordSymbolsTrack(instrumentTrackId)) {
        const std::string trackName = muse::trc("playback", "Chords") + "." + part->partName().toStdString();
        doAddTrack(instrumentTrackId, trackName, projectHadNoAudioSettings, onFinished);
        return;
    }

    const muse::String primaryInstrId = part->instrument()->id();
    if (instrumentTrackId.instrumentId == primaryInstrId) {
        const std::string trackName = part->partName().toStdString();
        doAddTrack(instrumentTrackId, trackName, projectHadNoAudioSettings, onFinished);
        return;
    }

    const Instrument* instrument = part->instrumentById(instrumentTrackId.instrumentId);
    if (instrument != nullptr) {
        std::string trackName = "(" + instrument->trackName().toStdString() + ")";
        doAddTrack(instrumentTrackId, trackName, projectHadNoAudioSettings, onFinished);
    }
}

void PlaybackController::doAddTrack(const InstrumentTrackId& instrumentTrackId, const std::string& title, bool projectHadNoAudioSettings,
                                    const TrackAddFinished& onFinished)
{
    IF_ASSERT_FAILED(notationPlayback() && playback()) {
        return;
    }

    if (!instrumentTrackId.isValid()) {
        return;
    }

    mpe::PlaybackData playbackData = notationPlayback()->trackPlaybackData(instrumentTrackId);
    if (!playbackData.isValid()) {
        return;
    }

    AudioInputParams inParams = audioSettings()->trackInputParams(instrumentTrackId);
    AudioOutputParams outParams = trackOutputParams(instrumentTrackId);
    AudioResourceMeta originMeta = inParams.resourceMeta;

    bool isMetronome = notationPlayback()->metronomeTrackId() == instrumentTrackId;

    if (!inParams.isValid()) {
        if (isMetronome) {
            const SoundProfile& profile = profilesRepo()->profile(configuration()->basicSoundProfileName());
            inParams = { profile.findResource(playbackData.setupData), {} };
        } else {
            const SoundProfile& profile = profilesRepo()->profile(audioSettings()->activeSoundProfile());
            inParams = { profile.findResource(playbackData.setupData), {} };
        }
    }

    if (!isMetronome && outParams.auxSends.empty()) {
        const muse::String& instrumentSoundId = inParams.resourceMeta.attributeVal(PLAYBACK_SETUP_DATA_ATTRIBUTE);
        AudioSourceType sourceType = inParams.isValid() ? inParams.type() : AudioSourceType::Fluid;

        for (aux_channel_idx_t idx = 0; idx < AUX_CHANNEL_NUM; ++idx) {
            gain_t signalAmount = configuration()->defaultAuxSendValue(idx, sourceType, instrumentSoundId);
            outParams.auxSends.emplace_back(AuxSendParams { signalAmount, true });
        }
    }

    uint64_t playbackKey = notationPlaybackKey();

    playback()->addTrack(m_currentSequenceId, title, std::move(playbackData), { std::move(inParams), std::move(outParams) })
    .onResolve(this, [=](const TrackId trackId, const AudioParams& appliedParams) {
        //! NOTE It may be that while we were adding a track, the notation was already closed (or opened another)
        //! This situation can be if the notation was opened and immediately closed.
        if (notationPlaybackKey() != playbackKey) {
            return;
        }

        m_instrumentTrackIdMap.insert({ instrumentTrackId, trackId });

        const bool trackNewlyAdded = projectHadNoAudioSettings || !audioSettings()->trackHasExistingOutputParams(instrumentTrackId);
        audioSettings()->setTrackInputParams(instrumentTrackId, appliedParams.in, !projectHadNoAudioSettings);
        audioSettings()->setTrackOutputParams(instrumentTrackId, appliedParams.out, !projectHadNoAudioSettings);

        updateSoloMuteStates();

        onFinished();

        m_trackAdded.send(trackId);

        if (trackNewlyAdded) {
            onTrackNewlyAdded(instrumentTrackId);
        }

        if (shouldLoadDrumset(instrumentTrackId, originMeta, appliedParams.in.resourceMeta)) {
            m_drumsetLoader.loadDrumset(m_notation, instrumentTrackId, appliedParams.in.resourceMeta);
        }

        if (muse::audio::isOnlineAudioResource(appliedParams.in.resourceMeta)) {
            m_onlineSoundsController->addOnlineTrack(trackId, appliedParams.in.resourceMeta);

            if (notationPlayback()) {
                notationPlayback()->setSendEventsOnScoreChange(instrumentTrackId, true);
            }
        }
    })
    .onReject(this, [instrumentTrackId, onFinished](int code, const std::string& msg) {
        LOGE() << "can't add a new track, code: [" << code << "] " << msg;

        onFinished();
    });

    m_loadingTrackCount++;
}

void PlaybackController::addAuxTrack(aux_channel_idx_t index, bool projectHadNoAudioSettings, const TrackAddFinished& onFinished)
{
    IF_ASSERT_FAILED(notationPlayback() && playback()) {
        return;
    }

    AudioOutputParams outParams;

    if (audioSettings()->containsAuxOutputParams(index)) {
        outParams = audioSettings()->auxOutputParams(index);
    } else if (index == REVERB_CHANNEL_IDX) {
        outParams = makeReverbOutputParams();
    }

    std::string title = resolveAuxTrackTitle(index, outParams, false);
    uint64_t playbackKey = notationPlaybackKey();

    playback()->addAuxTrack(m_currentSequenceId, title, outParams)
    .onResolve(this, [=](const TrackId trackId, const AudioOutputParams& appliedParams) {
        //! NOTE It may be that while we were adding a track, the notation was already closed (or opened another)
        //! This situation can be if the notation was opened and immediately closed.
        if (notationPlaybackKey() != playbackKey) {
            return;
        }

        m_auxTrackIdMap.insert({ index, trackId });

        audioSettings()->setAuxOutputParams(index, appliedParams, !projectHadNoAudioSettings);

        updateSoloMuteStates();
        onFinished();

        m_trackAdded.send(trackId);
    })
    .onReject(this, [onFinished](int code, const std::string& msg) {
        LOGE() << "can't add a new aux track, code: [" << code << "] " << msg;

        onFinished();
    });

    m_loadingTrackCount++;
}

void PlaybackController::setTrackActivity(const engraving::InstrumentTrackId& instrumentTrackId, const bool isActive)
{
    IF_ASSERT_FAILED(audioSettings() && playback()) {
        return;
    }

    AudioOutputParams outParams = audioSettings()->trackOutputParams(instrumentTrackId);

    outParams.muted = !isActive;

    audio::TrackId trackId = m_instrumentTrackIdMap[instrumentTrackId];
    playback()->setOutputParams(m_currentSequenceId, trackId, std::move(outParams));
}

AudioOutputParams PlaybackController::trackOutputParams(const InstrumentTrackId& instrumentTrackId) const
{
    IF_ASSERT_FAILED(audioSettings() && notationConfiguration() && notationPlayback()) {
        return {};
    }

    AudioOutputParams result = audioSettings()->trackOutputParams(instrumentTrackId);

    if (instrumentTrackId == notationPlayback()->metronomeTrackId()) {
        result.muted = !notationConfiguration()->isMetronomeEnabled() && !notationConfiguration()->isCountInEnabled();
        return result;
    }

    if (notationPlayback()->isChordSymbolsTrack(instrumentTrackId)) {
        result.muted = !notationConfiguration()->isPlayChordSymbolsEnabled();
    }

    return result;
}

void PlaybackController::removeTrack(const InstrumentTrackId& instrumentTrackId)
{
    IF_ASSERT_FAILED(notationPlayback() && playback()) {
        return;
    }

    auto search = m_instrumentTrackIdMap.find(instrumentTrackId);

    if (search == m_instrumentTrackIdMap.end()) {
        return;
    }

    playback()->removeTrack(m_currentSequenceId, search->second);
    audioSettings()->removeTrackParams(instrumentTrackId);

    m_masterNotation->notation()->soloMuteState()->removeTrackSoloMuteState(instrumentTrackId);
    for (const IExcerptNotationPtr& excerpt : m_masterNotation->excerpts()) {
        if (const INotationPtr& notation = excerpt->notation()) {
            notation->soloMuteState()->removeTrackSoloMuteState(instrumentTrackId);
        }
    }

    m_onlineSoundsController->removeOnlineTrack(search->second);

    m_trackRemoved.send(search->second);
    m_instrumentTrackIdMap.erase(instrumentTrackId);
}

void PlaybackController::onTrackNewlyAdded(const InstrumentTrackId& instrumentTrackId)
{
    for (const IExcerptNotationPtr& excerpt : m_masterNotation->excerpts()) {
        if (const INotationPtr& notation = excerpt->notation()) {
            if (notation == m_notation || notation->soloMuteState()->trackSoloMuteStateExists(instrumentTrackId)) {
                continue;
            }

            const Part* part = notation->parts()->part(instrumentTrackId.partId);
            const bool shouldMute = !part || !part->show();

            const INotationSoloMuteState::SoloMuteState soloMuteState = { shouldMute, /*solo*/ false };
            notation->soloMuteState()->setTrackSoloMuteState(instrumentTrackId, soloMuteState);
        }
    }
}

void PlaybackController::setupNewCurrentSequence(const TrackSequenceId sequenceId)
{
    playback()->removeAllTracks(m_currentSequenceId);

    m_currentSequenceId = sequenceId;
    m_onlineSoundsController->setCurrentSequence(sequenceId);
    m_player = playback()->player(sequenceId);
    globalContext()->setCurrentPlayer(m_player);

    if (!notationPlayback()) {
        return;
    }

    const audio::AudioOutputParams& masterOutputParams = audioSettings()->masterAudioOutputParams();
    playback()->setMasterOutputParams(masterOutputParams);

    subscribeOnAudioParamsChanges();
    setupSequenceTracks();
    setupSequencePlayer();

    m_currentSequenceIdChanged.notify();
}

void PlaybackController::subscribeOnAudioParamsChanges()
{
    playback()->masterOutputParamsChanged().onReceive(this, [this](const audio::AudioOutputParams& params) {
        audioSettings()->setMasterAudioOutputParams(params);
    });

    playback()->inputParamsChanged().onReceive(this,
                                               [this](const TrackSequenceId sequenceId,
                                                      const TrackId trackId,
                                                      const AudioInputParams& params) {
        if (sequenceId != m_currentSequenceId) {
            return;
        }

        auto search = std::find_if(m_instrumentTrackIdMap.begin(), m_instrumentTrackIdMap.end(), [trackId](const auto& pair) {
            return pair.second == trackId;
        });

        if (search != m_instrumentTrackIdMap.end()) {
            const AudioResourceMeta& oldMeta = audioSettings()->trackInputParams(search->first).resourceMeta;
            onAudioResourceChanged(trackId, search->first, oldMeta, params.resourceMeta);

            audioSettings()->setTrackInputParams(search->first, params);
        }
    });

    playback()->outputParamsChanged().onReceive(this,
                                                [this](const TrackSequenceId sequenceId,
                                                       const TrackId trackId,
                                                       const AudioOutputParams& params) {
        if (sequenceId != m_currentSequenceId) {
            return;
        }

        auto instrumentIt = std::find_if(m_instrumentTrackIdMap.begin(), m_instrumentTrackIdMap.end(), [trackId](const auto& pair) {
            return pair.second == trackId;
        });

        if (instrumentIt != m_instrumentTrackIdMap.end()) {
            audioSettings()->setTrackOutputParams(instrumentIt->first, params);
            return;
        }

        auto auxIt = std::find_if(m_auxTrackIdMap.begin(), m_auxTrackIdMap.end(), [trackId](const auto& pair) {
            return pair.second == trackId;
        });

        if (auxIt != m_auxTrackIdMap.end()) {
            aux_channel_idx_t auxIdx = auxIt->first;
            std::string oldName = resolveAuxTrackTitle(auxIdx, audioSettings()->auxOutputParams(auxIdx));
            std::string newName = resolveAuxTrackTitle(auxIdx, params);

            audioSettings()->setAuxOutputParams(auxIdx, params);

            if (oldName != newName) {
                m_auxChannelNameChanged.send(auxIdx, newName);
            }
        }
    });
}

void PlaybackController::setupSequenceTracks()
{
    m_instrumentTrackIdMap.clear();

    if (!masterNotationParts()) {
        return;
    }

    const bool projectHadNoAudioSettings = !audioSettings()->hasAnyAudioSettings();

    m_loadingTrackCount = 0;

    InstrumentTrackIdSet trackIdSet = notationPlayback()->existingTrackIdSet();
    size_t trackCount = trackIdSet.size() + AUX_CHANNEL_NUM;
    std::string title = muse::trc("playback", "Loading audio samples");

    auto onAddFinished = [this, trackCount, title]() {
        m_loadingTrackCount--;

        size_t current = trackCount - m_loadingTrackCount;
        m_loadingProgress.progress(current, trackCount, title);

        if (m_loadingTrackCount == 0) {
            m_loadingProgress.finish(muse::make_ok());
            m_isPlayAllowedChanged.notify();
        }
    };

    for (const InstrumentTrackId& trackId : trackIdSet) {
        addTrack(trackId, projectHadNoAudioSettings, onAddFinished);
    }

    for (aux_channel_idx_t idx = 0; idx < AUX_CHANNEL_NUM; ++idx) {
        addAuxTrack(idx, projectHadNoAudioSettings, onAddFinished);
    }

    m_loadingProgress.progress(0, trackCount, title);

    notationPlayback()->trackAdded().onReceive(this, [this, onAddFinished](const InstrumentTrackId& instrumentTrackId) {
        addTrack(instrumentTrackId, false /*projectHadNoAudioSettings*/, onAddFinished);
    });

    notationPlayback()->trackRemoved().onReceive(this, [this](const InstrumentTrackId& instrumentTrackId) {
        removeTrack(instrumentTrackId);
    });

    NotifyList<const Part*> partList = masterNotationParts()->partList();

    //! HACK - ideally we would use "this" (PlaybackController) instead of m_seqAsyncReceiver for the following
    //! subscription, but we've already subscribed to onItemChanged for a different reason in setNotation...
    partList.onItemChanged(&m_seqAsyncReceiver, [this](const Part*) {
        updateSoloMuteStates();
    });

    audioSettings()->auxSoloMuteStateChanged().onReceive(
        this, [this](aux_channel_idx_t, const notation::INotationSoloMuteState::SoloMuteState&) {
        updateSoloMuteStates();
    });

    m_isPlayAllowedChanged.notify();
}

void PlaybackController::setupSequencePlayer()
{
    currentPlayer()->playbackPositionChanged().onReceive(this, [this](const audio::secs_t pos) {
        if (m_isExportingAudio) {
            return;
        }

        m_currentTick = notationPlayback()->secToTick(pos);
        m_currentPlaybackPositionChanged.send(pos, m_currentTick);

        if (isMidiRecording()) {
            m_midiRecordingController->onPlaybackPositionChanged(pos);
        }

        updateCurrentTempo();

        secs_t endSecs = totalPlayTime();
        if (pos + milisecsToSecs(1) >= endSecs) {
            stop();
        }
    });

    currentPlayer()->playbackStatusChanged().onReceive(this, [this](PlaybackStatus) {
        if (m_isExportingAudio) {
            return;
        }

        m_isPlayingChanged.notify();
        onPlaybackStatusChanged();
        onMidiRecordingPlaybackStatusChanged();
    });

    currentPlayer()->setDuration(secsToMilisecs(notationPlayback()->totalPlayTime()));

    notationPlayback()->totalPlayTimeChanged().onReceive(this, [this](const audio::secs_t totalPlaybackTime) {
        currentPlayer()->setDuration(secsToMilisecs(totalPlaybackTime));
        m_totalPlayTimeChanged.notify();
    });
}

void PlaybackController::updateSoloMuteStates()
{
    if (!audioSettings() || !playback() || !m_notation) {
        return;
    }

    TRACEFUNC;

    InstrumentTrackIdSet existingTrackIdSet = notationPlayback()->existingTrackIdSet();
    bool hasSolo = false;

    for (const InstrumentTrackId& instrumentTrackId : existingTrackIdSet) {
        if (instrumentTrackId == notationPlayback()->metronomeTrackId()) {
            continue;
        }
        if (m_notation->soloMuteState()->trackSoloMuteState(instrumentTrackId).solo) {
            hasSolo = true;
            break;
        }
    }

    InstrumentTrackIdSet allowedInstrumentTrackIdSet = instrumentTrackIdSetForRangePlayback();
    bool isRangePlaybackMode = !m_isExportingAudio && selection()->isRange() && !allowedInstrumentTrackIdSet.empty();

    for (const InstrumentTrackId& instrumentTrackId : existingTrackIdSet) {
        if (!muse::contains(m_instrumentTrackIdMap, instrumentTrackId)) {
            continue;
        }

        if (instrumentTrackId == notationPlayback()->metronomeTrackId()) {
            continue;
        }

        // 1. Recall the solo-mute state for this notation
        const auto& soloMuteState = m_notation->soloMuteState()->trackSoloMuteState(instrumentTrackId);

        // 2. Evaluate "force mute" (disabling the mute button)
        bool shouldForceMute = hasSolo && !soloMuteState.solo;
        if (notationPlayback()->isChordSymbolsTrack(instrumentTrackId) && !shouldForceMute) {
            shouldForceMute = !notationConfiguration()->isPlayChordSymbolsEnabled();
        }

        if (isRangePlaybackMode && !shouldForceMute) {
            shouldForceMute = !muse::contains(allowedInstrumentTrackIdSet, instrumentTrackId);
        }

        // 3. Update params for playback / mixer
        AudioOutputParams params = trackOutputParams(instrumentTrackId);
        params.solo = soloMuteState.solo;
        params.muted = soloMuteState.mute || shouldForceMute;
        params.forceMute = shouldForceMute;

        audio::TrackId trackId = m_instrumentTrackIdMap.at(instrumentTrackId);
        playback()->setOutputParams(m_currentSequenceId, trackId, std::move(params));
    }

    updateAuxMuteStates();
}

void PlaybackController::updateAuxMuteStates()
{
    for (const auto& pair : m_auxTrackIdMap) {
        auto soloMuteState = audioSettings()->auxSoloMuteState(pair.first);

        AudioOutputParams params = audioSettings()->auxOutputParams(pair.first);
        if (params.muted == soloMuteState.mute) {
            continue;
        }

        params.muted = soloMuteState.mute;
        playback()->setOutputParams(m_currentSequenceId, pair.second, std::move(params));
    }
}

bool PlaybackController::actionChecked(const ActionCode& actionCode) const
{
    QMap<std::string, bool> isChecked {
        { LOOP_CODE, isLoopEnabled() },
        { MIDI_ON_CODE, notationConfiguration()->isMidiInputEnabled() },
        { INPUT_WRITTEN_PITCH, notationConfiguration()->midiUseWrittenPitch().val },
        { INPUT_SOUNDING_PITCH, !notationConfiguration()->midiUseWrittenPitch().val },
        { REPEAT_CODE, notationConfiguration()->isPlayRepeatsEnabled() },
        { PLAY_CHORD_SYMBOLS_CODE, notationConfiguration()->isPlayChordSymbolsEnabled() },
        { PAN_CODE, notationConfiguration()->isAutomaticallyPanEnabled() },
        { METRONOME_CODE, notationConfiguration()->isMetronomeEnabled() },
        { COUNT_IN_CODE, notationConfiguration()->isCountInEnabled() },
        { MIDI_RECORD_CODE, isMidiRecording() },
        { MIDI_RECORD_TUPLETS_CODE, notationConfiguration()->midiRecordingSearchTuplets() },
        { MIDI_RECORD_LEGATO_CODE, notationConfiguration()->midiRecordingLegato() },
        { MIDI_RECORD_ONE_STAFF_CODE, !notationConfiguration()->midiRecordingTwoStaves() },
        { MIDI_RECORD_TWO_STAVES_CODE, notationConfiguration()->midiRecordingTwoStaves() },
        { TOGGLE_HEAR_PLAYBACK_WHEN_EDITING_CODE, configuration()->playNotesWhenEditing() }
    };

    for (const auto& [code, ticks] : MIDI_RECORD_QUANTIZATION_CODES) {
        isChecked[code] = notationConfiguration()->midiRecordingMinQuantizationTicks() == ticks;
    }

    for (const auto& [code, measures] : MIDI_RECORD_COUNT_IN_CODES) {
        isChecked[code] = notationConfiguration()->midiRecordingCountInMeasures() == measures;
    }

    return isChecked[actionCode];
}

Channel<ActionCode> PlaybackController::actionCheckedChanged() const
{
    return m_actionCheckedChanged;
}

secs_t PlaybackController::totalPlayTime() const
{
    auto np = notationPlayback();
    return np ? np->totalPlayTime() : secs_t { 0.0 };
}

Notification PlaybackController::totalPlayTimeChanged() const
{
    return m_totalPlayTimeChanged;
}

const Tempo& PlaybackController::currentTempo() const
{
    return m_currentTempo;
}

Notification PlaybackController::currentTempoChanged() const
{
    return m_currentTempoChanged;
}

MeasureBeat PlaybackController::currentBeat() const
{
    return notationPlayback() ? notationPlayback()->beat(m_currentTick) : MeasureBeat();
}

secs_t PlaybackController::beatToSecs(int measureIndex, int beatIndex) const
{
    if (!notationPlayback()) {
        return 0;
    }

    muse::midi::tick_t rawTick = notationPlayback()->beatToRawTick(measureIndex, beatIndex);
    muse::midi::tick_t playedTick = notationPlayback()->playPositionTickByRawTick(rawTick).val;

    return playedTickToSecs(playedTick);
}

double PlaybackController::tempoMultiplier() const
{
    return notationPlayback() ? notationPlayback()->tempoMultiplier() : 1.0;
}

void PlaybackController::setTempoMultiplier(double multiplier)
{
    if (!notationPlayback()) {
        return;
    }

    tick_t tick = m_currentTick;
    bool playing = isPlaying();

    if (playing) {
        pause();
    }

    notationPlayback()->setTempoMultiplier(multiplier);
    seekRawTick(tick);
    updateLoop();

    if (playing) {
        resume();
    }
}

muse::Progress PlaybackController::loadingProgress() const
{
    return m_loadingProgress;
}

void PlaybackController::applyProfile(const SoundProfileName& profileName)
{
    project::IProjectAudioSettingsPtr audioSettingsPtr = audioSettings();

    IF_ASSERT_FAILED(audioSettingsPtr) {
        return;
    }

    const SoundProfile& profile = profilesRepo()->profile(profileName);
    if (!profile.isValid()) {
        return;
    }

    notationPlayback()->removeSoundFlags(notationPlayback()->existingTrackIdSet());

    const InstrumentTrackId& metronomeTrackId = notationPlayback()->metronomeTrackId();

    for (const auto& pair : m_instrumentTrackIdMap) {
        if (pair.first == metronomeTrackId) {
            continue;
        }

        const mpe::PlaybackData& playbackData = notationPlayback()->trackPlaybackData(pair.first);
        AudioInputParams newInputParams { profile.findResource(playbackData.setupData), {} };

        playback()->setInputParams(m_currentSequenceId, pair.second, std::move(newInputParams));
    }

    audioSettingsPtr->setActiveSoundProfile(profileName);
}

void PlaybackController::setNotation(notation::INotationPtr notation)
{
    if (m_notation == notation) {
        return;
    }

    if (m_notation) {
        INotationPartsPtr notationParts = m_notation->parts();
        NotifyList<const Part*> partList = notationParts->partList();
        partList.disconnect(this);

        m_notation->interaction()->selectionChanged().disconnect(this);
        m_notation->interaction()->textEditingEnded().disconnect(this);
        m_notation->soloMuteState()->trackSoloMuteStateChanged().disconnect(this);
    }

    m_notation = notation;

    m_isPlayAllowedChanged.notify();

    if (!m_notation) {
        setMasterNotation(nullptr);
        return;
    }

    setMasterNotation(m_notation->masterNotation());

    if (!m_notation->hasVisibleParts()) {
        pause();
    }

    updateSoloMuteStates();

    NotifyList<const Part*> partList = m_notation->parts()->partList();

    partList.onItemAdded(this, [this](const Part* part) {
        onPartChanged(part);
    });

    partList.onItemChanged(this, [this](const Part* part) {
        onPartChanged(part);
    });

    m_notation->interaction()->selectionChanged().onNotify(this, [this]() {
        onSelectionChanged();
    });

    m_notation->interaction()->textEditingEnded().onReceive(this, [this](engraving::TextBase* text) {
        if (text && text->isHarmony()) {
            playElements({ text });
        }
    });

    m_notation->soloMuteState()->trackSoloMuteStateChanged().onReceive(
        this, [this](const InstrumentTrackId&, const notation::INotationSoloMuteState::SoloMuteState&) {
        updateSoloMuteStates();
    });
}

void PlaybackController::setMasterNotation(notation::IMasterNotationPtr masterNotation)
{
    if (m_masterNotation == masterNotation) {
        return;
    }

    if (m_masterNotation) {
        m_masterNotation->hasPartsChanged().disconnect(this);
        m_masterNotation->playback()->loopBoundariesChanged().disconnect(this);
    }

    m_masterNotation = masterNotation;

    notifyActionCheckedChanged(LOOP_CODE);
    m_totalPlayTimeChanged.notify();

    if (!m_masterNotation) {
        return;
    }

    m_masterNotation->hasPartsChanged().onNotify(this, [this]() {
        m_isPlayAllowedChanged.notify();
    });

    m_masterNotation->playback()->loopBoundariesChanged().onNotify(this, [this]() {
        updateLoop();
    });
}

void PlaybackController::setIsExportingAudio(bool exporting)
{
    if (m_isExportingAudio == exporting) {
        return;
    }

    m_isExportingAudio = exporting;
    updateSoloMuteStates();

    if (exporting && notationPlayback()) {
        notationPlayback()->sendEventsForChangedTracks();
    }
}

bool PlaybackController::canReceiveAction(const ActionCode& code) const
{
    if (!m_masterNotation || !m_masterNotation->hasParts()) {
        return false;
    }

    if (interactive()->currentUri().val == NOTATION_REVIEW_PAGE_URI) {
        return false;
    }

    static const std::unordered_set<ActionCode> REQUIRES_MEASURES {
        PLAY_CODE,
        PAUSE_CODE,
        PAUSE_AND_SELECT_CODE,
        STOP_CODE,
        REWIND_CODE,
        LOOP_CODE,
        LOOP_IN_CODE,
        LOOP_OUT_CODE,
        MIDI_RECORD_CODE,
    };

    if (muse::contains(REQUIRES_MEASURES, code)) {
        const MasterScore* score = m_masterNotation->masterScore();
        return score && score->firstMeasure();
    }

    return true;
}

const std::map<TrackId, AudioResourceMeta>& PlaybackController::onlineSounds() const
{
    return m_onlineSoundsController->onlineSounds();
}

muse::async::Notification PlaybackController::onlineSoundsChanged() const
{
    return m_onlineSoundsController->onlineSoundsChanged();
}

muse::Progress PlaybackController::onlineSoundsProcessingProgress() const
{
    return m_onlineSoundsController->onlineSoundsProcessingProgress();
}

muse::audio::secs_t PlaybackController::playedTickToSecs(int tick) const
{
    return secs_t(notationPlayback()->playedTickToSec(tick));
}

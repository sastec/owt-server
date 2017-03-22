/*
 * Copyright 2017 Intel Corporation All Rights Reserved.
 *
 * The source code contained or described herein and all documents related to the
 * source code ("Material") are owned by Intel Corporation or its suppliers or
 * licensors. Title to the Material remains with Intel Corporation or its suppliers
 * and licensors. The Material contains trade secrets and proprietary and
 * confidential information of Intel or its suppliers and licensors. The Material
 * is protected by worldwide copyright and trade secret laws and treaty provisions.
 * No part of the Material may be used, copied, reproduced, modified, published,
 * uploaded, posted, transmitted, distributed, or disclosed in any way without
 * Intel's prior express written permission.
 *
 * No license under any patent, copyright, trade secret or other intellectual
 * property right is granted to or conferred upon you by disclosure or delivery of
 * the Materials, either expressly, by implication, inducement, estoppel or
 * otherwise. Any license under such intellectual property rights must be express
 * and approved by Intel in writing.
 */

#include "AcmmFrameMixer.h"

namespace mcu {

static inline AudioConferenceMixer::Frequency convert2Frequency(int32_t freq)
{
    switch (freq) {
        case 8000:
            return AudioConferenceMixer::kNbInHz;

        case 16000:
            return AudioConferenceMixer::kWbInHz;

        case 32000:
            return AudioConferenceMixer::kSwbInHz;

        case 48000:
            return AudioConferenceMixer::kFbInHz;

        default:
            return AudioConferenceMixer::kLowestPossible;
    }
}

DEFINE_LOGGER(AcmmFrameMixer, "mcu.media.AcmmFrameMixer");

AcmmFrameMixer::AcmmFrameMixer()
    : m_asyncHandle(NULL)
    , m_inputs(0)
    , m_outputs(0)
    , m_vadEnabled(false)
    , m_mostActiveChannel(-1)
    , m_frequency(0)
{
    m_mixerModule.reset(AudioConferenceMixer::Create(0));
    m_mixerModule->RegisterMixedStreamCallback(this);

    m_freeIds.resize(MAX_PARTICIPANTS);
    for (size_t i = 0; i < MAX_PARTICIPANTS; ++i)
        m_freeIds[i] = true;

    mutedAudioFrame.UpdateFrame(-1, 0, NULL, MAX_SAMPLE_RATE / MIXER_FREQUENCY, MAX_SAMPLE_RATE,
                            AudioFrame::kNormalSpeech, AudioFrame::kVadUnknown, 2);

    m_jobTimer.reset(new JobTimer(MIXER_FREQUENCY, this));
}

AcmmFrameMixer::~AcmmFrameMixer()
{
    boost::unique_lock<boost::shared_mutex> lock(m_mutex);

    m_jobTimer->stop();

    m_mixerModule->UnRegisterMixedStreamCallback();

    if (m_vadEnabled) {
        m_mixerModule->UnRegisterMixerStatusCallback();
        m_vadEnabled = false;
    }
}

boost::shared_ptr<AcmmParticipant> AcmmFrameMixer::getParticipant(const std::string& participant)
{
    if (m_ids.find(participant) == m_ids.end())
        return NULL;

    if (m_participants.find(m_ids[participant]) == m_participants.end())
        return NULL;

    return m_participants[m_ids[participant]];
}

int32_t AcmmFrameMixer::addParticipant(const std::string& participant)
{
    m_ids[participant] = getFreeId();
    m_participants[m_ids[participant]] = NULL;

    return m_ids[participant];
}

void AcmmFrameMixer::removeParticipant(const std::string& participant)
{
    m_participants.erase(m_ids[participant]);
    m_freeIds[m_ids[participant]] = true;
    m_ids.erase(participant);
}

int32_t AcmmFrameMixer::getFreeId()
{
    for (size_t i = 0; i < m_freeIds.size(); ++i) {
        if (m_freeIds[i]) {
            m_freeIds[i] = false;
            return i;
        }
    }

    assert(false);
}

void AcmmFrameMixer::setEventRegistry(EventRegistry* handle)
{
    boost::unique_lock<boost::shared_mutex> lock(m_mutex);
    ELOG_ERROR("setEventRegistry(%p)", handle);

    m_asyncHandle = handle;
}

void AcmmFrameMixer::enableVAD(uint32_t period)
{
    boost::unique_lock<boost::shared_mutex> lock(m_mutex);
    ELOG_ERROR("enableVAD, period(%u)", period);

    m_vadEnabled = true;
    m_mostActiveChannel = -1;
    m_mixerModule->RegisterMixerStatusCallback(this, period / 10);
}

void AcmmFrameMixer::disableVAD()
{
    boost::unique_lock<boost::shared_mutex> lock(m_mutex);
    ELOG_ERROR("disableVAD");

    m_vadEnabled = false;
    m_mixerModule->UnRegisterMixerStatusCallback();
}

void AcmmFrameMixer::resetVAD()
{
    boost::unique_lock<boost::shared_mutex> lock(m_mutex);
    ELOG_ERROR("resetVAD");
    m_mostActiveChannel = -1;
}

bool AcmmFrameMixer::addInput(const std::string& participant, const FrameFormat format, woogeen_base::FrameSource* source)
{
    boost::unique_lock<boost::shared_mutex> lock(m_mutex);
    boost::shared_ptr<AcmmParticipant> acmmParticipant = getParticipant(participant);
    int ret;

    ELOG_TRACE("setInput %s+++", participant.c_str());

    if (!acmmParticipant) {
        int32_t id = addParticipant(participant);

        acmmParticipant.reset(new AcmmParticipant(id));
        m_participants[id] = acmmParticipant;
    }

    if(!acmmParticipant->setInput(format, source)) {
        ELOG_ERROR("Fail to set participant input");
        return false;
    }

    ret = m_mixerModule->SetMixabilityStatus(acmmParticipant.get(), true);
    if (ret != 0) {
        ELOG_ERROR("Fail to SetMixabilityStatus");
        return false;
    }

    if (!acmmParticipant->hasOutput()) {
        ret = m_mixerModule->SetAnonymousMixabilityStatus(acmmParticipant.get(), true);
        if (ret != 0) {
            ELOG_ERROR("Fail to SetAnonymousMixabilityStatus");
            return false;
        }
    }

    m_inputs++;
    ELOG_TRACE("setInput %s---", participant.c_str());
    return true;
}

void AcmmFrameMixer::removeInput(const std::string& participant)
{
    boost::unique_lock<boost::shared_mutex> lock(m_mutex);
    boost::shared_ptr<AcmmParticipant> acmmParticipant = getParticipant(participant);

    ELOG_TRACE("removeInput %s+++", participant.c_str());

    if (acmmParticipant) {
        int ret;

        ret = m_mixerModule->SetMixabilityStatus(acmmParticipant.get(), false);
        if (ret != 0) {
            ELOG_ERROR("Fail to unSetMixabilityStatus");
            return;
        }

        acmmParticipant->unsetInput();
        if (!acmmParticipant->hasInput() && !acmmParticipant->hasOutput()) {
            acmmParticipant.reset();
            removeParticipant(participant);
        }
    }

    m_inputs--;
    ELOG_TRACE("removeInput %s---", participant.c_str());
    return;
}

bool AcmmFrameMixer::addOutput(const std::string& participant, const FrameFormat format, woogeen_base::FrameDestination* destination)
{
    boost::unique_lock<boost::shared_mutex> lock(m_mutex);
    boost::shared_ptr<AcmmParticipant> acmmParticipant = getParticipant(participant);
    int ret;

    ELOG_TRACE("setOutput %s, %d+++", participant.c_str(), format);

    if (!acmmParticipant) {
        int32_t id = addParticipant(participant);

        acmmParticipant.reset(new AcmmParticipant(id));
        m_participants[id] = acmmParticipant;
    }

    if(!acmmParticipant->setOutput(format, destination)) {
        ELOG_ERROR("Fail to set participant output");
        return false;
    }

    if (acmmParticipant->hasInput()) {
        ret = m_mixerModule->SetAnonymousMixabilityStatus(acmmParticipant.get(), false);
        if (ret != 0) {
            ELOG_ERROR("Fail to unSetAnonymousMixabilityStatus");
            return false;
        }
    }

    updateFrequency();

    m_outputs++;
    ELOG_TRACE("setOutput %s, %d---", participant.c_str(), format);
    return true;
}


void AcmmFrameMixer::removeOutput(const std::string& participant)
{
    boost::unique_lock<boost::shared_mutex> lock(m_mutex);
    boost::shared_ptr<AcmmParticipant> acmmParticipant = getParticipant(participant);

    ELOG_TRACE("removeOutput %s+++", participant.c_str());

    if (acmmParticipant) {
        if (acmmParticipant->hasInput()) {
            int ret;

            ret = m_mixerModule->SetAnonymousMixabilityStatus(acmmParticipant.get(), false);
            if (ret != 0) {
                ELOG_ERROR("Fail to unSetAnonymousMixabilityStatus");
                return;
            }
        }

        acmmParticipant->unsetOutput();
        if (!acmmParticipant->hasInput() && !acmmParticipant->hasOutput()) {
            acmmParticipant.reset();
            removeParticipant(participant);
        }

        updateFrequency();
    }

    m_outputs--;
    ELOG_TRACE("removeOutput %s---", participant.c_str());
    return;
}

void AcmmFrameMixer::updateFrequency()
{
    int32_t maxFreq = m_frequency;
    int32_t freq;
    int ret;

    for (auto& p : m_participants) {
        freq = p.second->NeededFrequency(0);
        if (freq > maxFreq)
            maxFreq= freq;
    }

    if (m_frequency != maxFreq) {
        ret = m_mixerModule->SetMinimumMixingFrequency(convert2Frequency(maxFreq));
        if (ret != 0) {
            ELOG_WARN("Fail to SetMinimumMixingFrequency, %d", maxFreq);
            return;
        }

        m_frequency = maxFreq;
    }

    return;
}

void AcmmFrameMixer::onTimeout()
{
    performMix();
}

void AcmmFrameMixer::performMix()
{
    boost::upgrade_lock<boost::shared_mutex> lock(m_mutex);
    if (m_inputs) {
        m_mixerModule->Process();
    } else if (m_outputs) {
        ELOG_TRACE_T("Generate muted frame");
        NewMixedAudio(0, mutedAudioFrame, NULL, 0);
    }
}

void AcmmFrameMixer::NewMixedAudio(int32_t id,
        const AudioFrame& generalAudioFrame,
        const AudioFrame** uniqueAudioFrames,
        uint32_t size)
{
    std::map<int32_t, bool> participantMap;
    for(uint32_t i = 0; i< size; i++) {
        ELOG_TRACE_T("NewMixedAudio(%d)", uniqueAudioFrames[i]->id_);

        participantMap[uniqueAudioFrames[i]->id_] = true;
        if (m_participants.find(uniqueAudioFrames[i]->id_) != m_participants.end()) {
            boost::shared_ptr<AcmmParticipant> acmmParticipant = m_participants[uniqueAudioFrames[i]->id_];
            if (acmmParticipant->hasInput() && acmmParticipant->hasOutput()) {
                acmmParticipant->NewMixedAudio(uniqueAudioFrames[i]);
            }
        }
    }

    for (auto& p : m_participants) {
        boost::shared_ptr<AcmmParticipant> acmmParticipant = p.second;
        if (acmmParticipant->hasOutput() && participantMap.find(acmmParticipant->id()) == participantMap.end()) {
            acmmParticipant->NewMixedAudio(&generalAudioFrame);
        }
    }
}

void AcmmFrameMixer::VADPositiveParticipants(const int32_t id, const ParticipantStatistics* participantStatistics, const uint32_t size)
{
    if (!m_asyncHandle || !m_vadEnabled || size < 2) {
        ELOG_TRACE("VADPositiveParticipants, asyncHandle(%p), enabled(%d), size(%d)", m_asyncHandle, m_vadEnabled, size);
        return;
    }

    const ParticipantStatistics* active = NULL;
    const ParticipantStatistics* p = participantStatistics;
    for(uint32_t i = 0; i < size; i++, p++) {
        ELOG_TRACE("%d, participant(%d), level(%u)", i, p->participant, p->level);
        if (!active || p->level > active->level) {
            active = p;
        }
    }

    if (m_mostActiveChannel != active->participant) {
        ELOG_TRACE("active participant, %d -> %d", m_mostActiveChannel, active->participant);

        m_mostActiveChannel = active->participant;
        for (auto it = m_ids.begin(); it != m_ids.end(); ++it) {
            if (it->second == m_mostActiveChannel) {
                ELOG_DEBUG("mostActiveParticipant now is :%s", it->first.c_str());
                m_asyncHandle->notifyAsyncEvent("vad", it->first.c_str());
                break;
            }
        }
    }
}

void AcmmFrameMixer::MixedParticipants(const int32_t id, const ParticipantStatistics* participantStatistics, const uint32_t size)
{
}

void AcmmFrameMixer::MixedAudioLevel(const int32_t id, const uint32_t level)
{
}

} /* namespace mcu */
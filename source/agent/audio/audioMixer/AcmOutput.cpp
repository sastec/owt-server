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

#include <rtputils.h>

#include "AudioUtilities.h"
#include "AcmOutput.h"

namespace mcu {

using namespace webrtc;
using namespace woogeen_base;

DEFINE_LOGGER(AcmOutput, "mcu.media.AcmOutput");

AcmOutput::AcmOutput(const FrameFormat format, FrameDestination *destination)
    : m_format(format)
    , m_destination(destination)
    , m_timestampOffset(0)
    , m_valid(false)
{
    AudioCodingModule::Config config;
    m_audioCodingModule.reset(AudioCodingModule::Create(config));
}

AcmOutput::~AcmOutput()
{
    int ret;

    if (!m_valid)
        return;

    this->removeAudioDestination(m_destination);
    m_destination = nullptr;
    m_format = FRAME_FORMAT_UNKNOWN;

    ret = m_audioCodingModule->RegisterTransportCallback(NULL);
    if (ret != 0) {
        ELOG_ERROR_T("Error RegisterTransportCallback");
        return;
    }
}

bool AcmOutput::init()
{
    int ret;
    struct CodecInst codec = CodecInst();

    if (!fillAudioCodec(m_format, codec)) {
        ELOG_ERROR_T("Error fillAudioCodec(%s)", getFormatStr(m_format));
        return false;
    }

    ret = m_audioCodingModule->RegisterSendCodec(codec);
    if (ret != 0) {
        ELOG_ERROR_T("Error RegisterSendCodec(%s)", getFormatStr(m_format));
        return false;
    }

    ret = m_audioCodingModule->RegisterTransportCallback(this);
    if (ret != 0) {
        ELOG_ERROR_T("Error RegisterTransportCallback");
        return false;
    }

    this->addAudioDestination(m_destination);

    m_timestampOffset = currentTimeMs();
    m_valid = true;

    return true;
}

bool AcmOutput::addAudioFrame(const AudioFrame* audioFrame)
{
    int ret;

    if (!m_valid)
        return false;

    ELOG_TRACE_T("NewMixedAudio, id(%d), sample_rate(%d), channels(%d), samples_per_channel(%d), timestamp(%d)",
            audioFrame->id_,
            audioFrame->sample_rate_hz_,
            audioFrame->num_channels_,
            audioFrame->samples_per_channel_,
            audioFrame->timestamp_
            );

    ret = m_audioCodingModule->Add10MsData(*audioFrame);
    if (ret < 0) {
        ELOG_ERROR_T("Fail to insert raw into acm");
        return false;
    }

    return true;
}

int32_t AcmOutput::SendData(FrameType frame_type,
        uint8_t payload_type,
        uint32_t timestamp,
        const uint8_t* payload_data,
        size_t payload_len_bytes,
        const RTPFragmentationHeader* fragmentation)
{
    if (!m_valid)
        return -1;

    if (!payload_data || payload_len_bytes <= 0 || !m_destination) {
        ELOG_ERROR_T("SendData, invalid param");
        return -1;
    }

    FrameFormat format = FRAME_FORMAT_UNKNOWN;
    woogeen_base::Frame frame;
    memset(&frame, 0, sizeof(frame));
    switch (payload_type) {
        case PCMU_8000_PT:
            format = FRAME_FORMAT_PCMU;
            frame.additionalInfo.audio.channels = 1;
            frame.additionalInfo.audio.sampleRate = 8000;
            break;
        case PCMA_8000_PT:
            format = FRAME_FORMAT_PCMA;
            frame.additionalInfo.audio.channels = 1;
            frame.additionalInfo.audio.sampleRate = 8000;
            break;
        case ISAC_16000_PT:
            format = FRAME_FORMAT_ISAC16;
            frame.additionalInfo.audio.channels = 1;
            frame.additionalInfo.audio.sampleRate = 16000;
            break;
        case ISAC_32000_PT:
            format = FRAME_FORMAT_ISAC32;
            frame.additionalInfo.audio.channels = 1;
            frame.additionalInfo.audio.sampleRate = 32000;
            break;
        case OPUS_48000_PT:
            format = FRAME_FORMAT_OPUS;
            frame.additionalInfo.audio.channels = 2;
            frame.additionalInfo.audio.sampleRate = 48000;
            break;
        default:
            ELOG_ERROR_T("SendData, invalid playload(%d)", payload_type);
            return -1;
    }

    frame.format = format;
    frame.payload = const_cast<uint8_t*>(payload_data);
    frame.length = payload_len_bytes;
    frame.timeStamp = (currentTimeMs() - m_timestampOffset) * frame.additionalInfo.audio.sampleRate / 1000;

    ELOG_TRACE_T("deliverFrame(%s), sampleRate(%d), channels(%d), timeStamp(%d), length(%d), %s",
            getFormatStr(frame.format),
            frame.additionalInfo.audio.sampleRate,
            frame.additionalInfo.audio.channels,
            frame.timeStamp * 1000 / frame.additionalInfo.audio.sampleRate,
            frame.length,
            frame.additionalInfo.audio.isRtpPacket ? "RtpPacket" : "NonRtpPacket"
            );

    deliverFrame(frame);

    return 0;
}

} /* namespace mcu */
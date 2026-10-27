//
// Created by Laky64 on 31/03/2024.
//

#include <wrtc/interfaces/media/channels/outgoing_audio_channel.hpp>

#include <wrtc/interfaces/native_connection.hpp>

#include <cstdlib>

namespace wrtc {
    OutgoingAudioChannel::OutgoingAudioChannel(
        webrtc::Call* call,
        ChannelManager* channelManager,
        webrtc::RtpTransport* rtpTransport,
        const MediaContent& mediaContent,
        SafeThread& workerThread,
        SafeThread& networkThread,
        webrtc::LocalAudioSinkAdapter* sink
    ): _ssrc(mediaContent.ssrc), workerThread(workerThread), networkThread(networkThread), sink(sink) {
        webrtc::AudioOptions audioOptions;
        audioOptions.echo_cancellation = false;
        audioOptions.noise_suppression = false;
        audioOptions.auto_gain_control = false;
        audioOptions.highpass_filter = false;

        channel = channelManager->CreateVoiceChannel(
            call,
            webrtc::MediaConfig(),
            std::to_string(_ssrc),
            false,
            NativeNetworkInterface::getDefaultCryptoOptions(),
            audioOptions
        );
        networkThread.BlockingCall([&] {
            channel->SetRtpTransport(rtpTransport);
        });
        std::vector<webrtc::Codec> codecs;
        for (const auto &[id, name, clockrate, channels, feedbackTypes, parameters] : mediaContent.payloadTypes) {
            if (name == "opus") {
                webrtc::Codec codec = webrtc::CreateAudioCodec(static_cast<int>(id), name, static_cast<int>(clockrate), channels);
                codec.SetParam(webrtc::kCodecParamUseInbandFec, 1);
                codec.SetParam(webrtc::kCodecParamPTime, 60);
                // [opus128 patch] Without these WebRTC picks its defaults for
                // the outgoing encoder: 1 channel, 32 kbps, VoIP application.
                // stereo + sprop-stereo + maxaveragebitrate make it 2 channels,
                // audio application at the given bitrate (see ntgcalls #83).
                // NTG_OPUS_BITRATE=<bps> picks the bitrate (default 128000);
                // NTG_OPUS_BITRATE=0 keeps the stock behaviour (no reinstall).
                {
                    int hqBitrate = 128000;
                    if (const char* env = std::getenv("NTG_OPUS_BITRATE")) {
                        hqBitrate = std::atoi(env);
                    }
                    if (hqBitrate > 0) {
                        if (hqBitrate < 6000) hqBitrate = 6000;
                        if (hqBitrate > 510000) hqBitrate = 510000;
                        codec.SetParam(webrtc::kCodecParamStereo, 1);
                        codec.SetParam(webrtc::kCodecParamSPropStereo, 1);
                        codec.SetParam(webrtc::kCodecParamMaxAverageBitrate, hqBitrate);
                    }
                }
                for (const auto &[type, subtype] : feedbackTypes) {
                    codec.AddFeedbackParam(webrtc::FeedbackParam(type, subtype));
                }
                codecs.push_back(std::move(codec));
                break;
            }
        }
        const auto outgoingDescription = std::make_unique<webrtc::AudioContentDescription>();
        for (const auto &rtpExtension : mediaContent.rtpExtensions) {
            outgoingDescription->AddRtpHeaderExtension(webrtc::RtpExtension(rtpExtension.uri, rtpExtension.id));
        }
        outgoingDescription->set_rtcp_mux(true);
        outgoingDescription->set_rtcp_reduced_size(true);
        outgoingDescription->set_direction(webrtc::RtpTransceiverDirection::kSendOnly);
        outgoingDescription->set_codecs(codecs);
        outgoingDescription->set_bandwidth(-1);
        outgoingDescription->AddStream(webrtc::StreamParams::CreateLegacy(_ssrc));
        const auto incomingDescription = std::make_unique<webrtc::AudioContentDescription>();
        for (const auto &rtpExtension : mediaContent.rtpExtensions) {
            incomingDescription->AddRtpHeaderExtension(webrtc::RtpExtension(rtpExtension.uri, rtpExtension.id));
        }
        incomingDescription->set_rtcp_mux(true);
        incomingDescription->set_rtcp_reduced_size(true);
        incomingDescription->set_direction(webrtc::RtpTransceiverDirection::kRecvOnly);
        incomingDescription->set_codecs(codecs);
        incomingDescription->set_bandwidth(-1);
        workerThread.BlockingCall([&] {
            channel->rtp_transport()->SetActivePayloadTypeDemuxing(false);
            channel->SetLocalContent(outgoingDescription.get(), webrtc::SdpType::kOffer);
            channel->SetRemoteContent(incomingDescription.get(), webrtc::SdpType::kAnswer);
        });
        set_enabled(true);
        workerThread.BlockingCall([&] {
            webrtc::RtpParameters initialParameters = channel->voice_media_send_channel()->GetRtpSendParameters(_ssrc);
            webrtc::RtpParameters updatedParameters = initialParameters;
            if (updatedParameters.encodings.empty()) {
                updatedParameters.encodings.emplace_back();
            }
            if (initialParameters != updatedParameters) {
                channel->voice_media_send_channel()->SetRtpSendParameters(_ssrc, updatedParameters);
            }
        });
    }

    void OutgoingAudioChannel::set_enabled(const bool enable) const {
        channel->Enable(enable);
        workerThread.BlockingCall([&] {
            channel->voice_media_send_channel()->SetAudioSend(_ssrc, enable, nullptr, sink);
        });
    }

    OutgoingAudioChannel::~OutgoingAudioChannel() {
        channel->Enable(false);
        networkThread.BlockingCall([&] {
            channel->SetRtpTransport(nullptr);
        });
        workerThread.BlockingCall([&] {
            channel = nullptr;
        });
        sink = nullptr;
    }

    uint32_t OutgoingAudioChannel::ssrc() const {
        return _ssrc;
    }
} // wrtc

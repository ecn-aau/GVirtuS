#pragma once

#include <string>
#include <experimental/filesystem>
#include <gvirtus/common/JSON.h>
#include "msquic.h"

namespace fs = std::experimental::filesystem;

inline QUIC_SERVER_RESUMPTION_LEVEL ParseResumptionLevel(const std::string& s) {
    if (s == "QUIC_SERVER_RESUME_AND_ZERORTT") return QUIC_SERVER_RESUME_AND_ZERORTT;
    if (s == "QUIC_SERVER_RESUME_ONLY")        return QUIC_SERVER_RESUME_ONLY;
    return QUIC_SERVER_NO_RESUME;
}

struct QuicSettingsConfig {

    QUIC_SETTINGS ToQuicSettings() const {
        QUIC_SETTINGS s = {0};

        s.IdleTimeoutMs                     = IdleTimeoutMs;
        s.IsSet.IdleTimeoutMs               = IsSet_IdleTimeoutMs;
        s.HandshakeIdleTimeoutMs            = HandshakeIdleTimeoutMs;
        s.IsSet.HandshakeIdleTimeoutMs      = IsSet_HandshakeIdleTimeoutMs;
        s.DisconnectTimeoutMs               = DisconnectTimeoutMs;
        s.IsSet.DisconnectTimeoutMs         = IsSet_DisconnectTimeoutMs;
        s.KeepAliveIntervalMs               = KeepAliveIntervalMs;
        s.IsSet.KeepAliveIntervalMs         = IsSet_KeepAliveIntervalMs;
        s.PeerBidiStreamCount               = PeerBidiStreamCount;
        s.IsSet.PeerBidiStreamCount         = IsSet_PeerBidiStreamCount;
        s.PeerUnidiStreamCount              = PeerUnidiStreamCount;
        s.IsSet.PeerUnidiStreamCount        = IsSet_PeerUnidiStreamCount;
        s.MaxBytesPerKey                    = MaxBytesPerKey;
        s.IsSet.MaxBytesPerKey              = IsSet_MaxBytesPerKey;
        s.ServerResumptionLevel             = ParseResumptionLevel(ServerResumptionLevel);
        s.IsSet.ServerResumptionLevel       = IsSet_ServerResumptionLevel;
        s.InitialWindowPackets              = InitialWindowPackets;
        s.IsSet.InitialWindowPackets        = IsSet_InitialWindowPackets;
        s.SendBufferingEnabled              = SendBufferingEnabled;
        s.IsSet.SendBufferingEnabled        = IsSet_SendBufferingEnabled;
        s.PacingEnabled                     = PacingEnabled;
        s.IsSet.PacingEnabled               = IsSet_PacingEnabled;
        s.MigrationEnabled                  = MigrationEnabled;
        s.IsSet.MigrationEnabled            = IsSet_MigrationEnabled;
        s.DatagramReceiveEnabled            = DatagramReceiveEnabled;
        s.IsSet.DatagramReceiveEnabled      = IsSet_DatagramReceiveEnabled;
        s.MaxAckDelayMs                     = MaxAckDelayMs;
        s.IsSet.MaxAckDelayMs               = IsSet_MaxAckDelayMs;
        s.StreamRecvWindowDefault           = StreamRecvWindowDefault;
        s.IsSet.StreamRecvWindowDefault     = IsSet_StreamRecvWindowDefault;
        s.StreamRecvBufferDefault           = StreamRecvBufferDefault;
        s.IsSet.StreamRecvBufferDefault     = IsSet_StreamRecvBufferDefault;
        s.ConnFlowControlWindow             = ConnFlowControlWindow;
        s.IsSet.ConnFlowControlWindow       = IsSet_ConnFlowControlWindow;
        s.MaxWorkerQueueDelayUs             = MaxWorkerQueueDelayUs;
        s.IsSet.MaxWorkerQueueDelayUs       = IsSet_MaxWorkerQueueDelayUs;
        s.MaxStatelessOperations            = MaxStatelessOperations;
        s.IsSet.MaxStatelessOperations      = IsSet_MaxStatelessOperations;
        s.InitialRttMs                      = InitialRttMs;
        s.IsSet.InitialRttMs                = IsSet_InitialRttMs;

        // not included in current version of msQuic
        //s.MaxUdpPayloadSize                 = MaxUdpPayloadSize;
        //s.IsSet.MaxUdpPayloadSize           = IsSet_MaxUdpPayloadSize;
        
        s.MaximumMtu                        = MaximumMtu;
        s.IsSet.MaximumMtu                  = IsSet_MaximumMtu;
        s.MinimumMtu                        = MinimumMtu;
        s.IsSet.MinimumMtu                  = IsSet_MinimumMtu;
        s.EcnEnabled                        = EcnEnabled;
        s.IsSet.EcnEnabled                  = IsSet_EcnEnabled;
        s.HyStartEnabled                    = HyStartEnabled;
        s.IsSet.HyStartEnabled              = IsSet_HyStartEnabled;

        return s;
    }

    uint64_t    IdleTimeoutMs               = 0;    bool IsSet_IdleTimeoutMs               = false;
    uint64_t    HandshakeIdleTimeoutMs      = 0;    bool IsSet_HandshakeIdleTimeoutMs      = false;
    uint32_t    DisconnectTimeoutMs         = 0;    bool IsSet_DisconnectTimeoutMs         = false;
    uint32_t    KeepAliveIntervalMs         = 0;    bool IsSet_KeepAliveIntervalMs         = false;
    uint16_t    PeerBidiStreamCount         = 0;    bool IsSet_PeerBidiStreamCount         = false;
    uint16_t    PeerUnidiStreamCount        = 0;    bool IsSet_PeerUnidiStreamCount        = false;
    uint64_t    MaxBytesPerKey              = 0;    bool IsSet_MaxBytesPerKey              = false;
    std::string ServerResumptionLevel       = "QUIC_SERVER_NO_RESUME";
                                                    bool IsSet_ServerResumptionLevel       = false;
    uint32_t    InitialWindowPackets        = 0;    bool IsSet_InitialWindowPackets        = false;
    uint8_t     SendBufferingEnabled        = 0;    bool IsSet_SendBufferingEnabled        = false;
    uint8_t     PacingEnabled               = 0;    bool IsSet_PacingEnabled               = false;
    uint8_t     MigrationEnabled            = 0;    bool IsSet_MigrationEnabled            = false;
    uint8_t     DatagramReceiveEnabled      = 0;    bool IsSet_DatagramReceiveEnabled      = false;
    uint32_t    MaxAckDelayMs               = 0;    bool IsSet_MaxAckDelayMs               = false;
    uint32_t    StreamRecvWindowDefault     = 0;    bool IsSet_StreamRecvWindowDefault     = false;
    uint32_t    StreamRecvBufferDefault     = 0;    bool IsSet_StreamRecvBufferDefault     = false;
    uint32_t    ConnFlowControlWindow       = 0;    bool IsSet_ConnFlowControlWindow       = false;
    uint64_t    MaxWorkerQueueDelayUs       = 0;    bool IsSet_MaxWorkerQueueDelayUs       = false;
    uint32_t    MaxStatelessOperations      = 0;    bool IsSet_MaxStatelessOperations      = false;
    uint32_t    InitialRttMs                = 0;    bool IsSet_InitialRttMs                = false;
    uint16_t    MaxUdpPayloadSize           = 0;    bool IsSet_MaxUdpPayloadSize           = false;
    uint16_t    MaximumMtu                  = 0;    bool IsSet_MaximumMtu                  = false;
    uint16_t    MinimumMtu                  = 0;    bool IsSet_MinimumMtu                  = false;
    uint8_t     EcnEnabled                  = 0;    bool IsSet_EcnEnabled                  = false;
    uint8_t     HyStartEnabled              = 0;    bool IsSet_HyStartEnabled              = false;
};

inline void from_json(const nlohmann::json& j, QuicSettingsConfig& s) {
    const auto& q = j.at("quic_settings");

    auto get = [&](const char* key, auto& field) {
        if (q.contains(key)) q.at(key).get_to(field);
    };

    get("IdleTimeoutMs",                s.IdleTimeoutMs);
    get("IsSet_IdleTimeoutMs",          s.IsSet_IdleTimeoutMs);
    get("HandshakeIdleTimeoutMs",       s.HandshakeIdleTimeoutMs);
    get("IsSet_HandshakeIdleTimeoutMs", s.IsSet_HandshakeIdleTimeoutMs);
    get("DisconnectTimeoutMs",          s.DisconnectTimeoutMs);
    get("IsSet_DisconnectTimeoutMs",    s.IsSet_DisconnectTimeoutMs);
    get("KeepAliveIntervalMs",          s.KeepAliveIntervalMs);
    get("IsSet_KeepAliveIntervalMs",    s.IsSet_KeepAliveIntervalMs);
    get("PeerBidiStreamCount",          s.PeerBidiStreamCount);
    get("IsSet_PeerBidiStreamCount",    s.IsSet_PeerBidiStreamCount);
    get("PeerUnidiStreamCount",         s.PeerUnidiStreamCount);
    get("IsSet_PeerUnidiStreamCount",   s.IsSet_PeerUnidiStreamCount);
    get("MaxBytesPerKey",               s.MaxBytesPerKey);
    get("IsSet_MaxBytesPerKey",         s.IsSet_MaxBytesPerKey);
    get("ServerResumptionLevel",        s.ServerResumptionLevel);
    get("IsSet_ServerResumptionLevel",  s.IsSet_ServerResumptionLevel);
    get("InitialWindowPackets",         s.InitialWindowPackets);
    get("IsSet_InitialWindowPackets",   s.IsSet_InitialWindowPackets);
    get("SendBufferingEnabled",         s.SendBufferingEnabled);
    get("IsSet_SendBufferingEnabled",   s.IsSet_SendBufferingEnabled);
    get("PacingEnabled",                s.PacingEnabled);
    get("IsSet_PacingEnabled",          s.IsSet_PacingEnabled);
    get("MigrationEnabled",             s.MigrationEnabled);
    get("IsSet_MigrationEnabled",       s.IsSet_MigrationEnabled);
    get("DatagramReceiveEnabled",       s.DatagramReceiveEnabled);
    get("IsSet_DatagramReceiveEnabled", s.IsSet_DatagramReceiveEnabled);
    get("MaxAckDelayMs",                s.MaxAckDelayMs);
    get("IsSet_MaxAckDelayMs",          s.IsSet_MaxAckDelayMs);
    get("StreamRecvWindowDefault",      s.StreamRecvWindowDefault);
    get("IsSet_StreamRecvWindowDefault",s.IsSet_StreamRecvWindowDefault);
    get("StreamRecvBufferDefault",      s.StreamRecvBufferDefault);
    get("IsSet_StreamRecvBufferDefault",s.IsSet_StreamRecvBufferDefault);
    get("ConnFlowControlWindow",        s.ConnFlowControlWindow);
    get("IsSet_ConnFlowControlWindow",  s.IsSet_ConnFlowControlWindow);
    get("MaxWorkerQueueDelayUs",        s.MaxWorkerQueueDelayUs);
    get("IsSet_MaxWorkerQueueDelayUs",  s.IsSet_MaxWorkerQueueDelayUs);
    get("MaxStatelessOperations",       s.MaxStatelessOperations);
    get("IsSet_MaxStatelessOperations", s.IsSet_MaxStatelessOperations);
    get("InitialRttMs",                 s.InitialRttMs);
    get("IsSet_InitialRttMs",           s.IsSet_InitialRttMs);
    get("MaxUdpPayloadSize",            s.MaxUdpPayloadSize);
    get("IsSet_MaxUdpPayloadSize",      s.IsSet_MaxUdpPayloadSize);
    get("MaximumMtu",                   s.MaximumMtu);
    get("IsSet_MaximumMtu",             s.IsSet_MaximumMtu);
    get("MinimumMtu",                   s.MinimumMtu);
    get("IsSet_MinimumMtu",             s.IsSet_MinimumMtu);
    get("EcnEnabled",                   s.EcnEnabled);
    get("IsSet_EcnEnabled",             s.IsSet_EcnEnabled);
    get("HyStartEnabled",               s.HyStartEnabled);
    get("IsSet_HyStartEnabled",         s.IsSet_HyStartEnabled);
}
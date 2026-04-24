/*
 * gVirtuS -- A GPGPU transparent virtualization component.
 *
 * Copyright (C) 2009-2010  The University of Napoli Parthenope at Naples.
 *
 * This file is part of gVirtuS.
 *
 * gVirtuS is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * gVirtuS is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with gVirtuS; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Written by: Giuseppe Coviello <giuseppe.coviello@uniparthenope.it>,
 *             Department of Applied Science
 */

#pragma once

#ifdef _WIN32
#include <fstream>
#else
#include <ext/stdio_filebuf.h>
#endif

#include "gvirtus/communicators/Communicator.h"
#include "msquic.h"

#include <atomic>
#include <cstring>
#include <immintrin.h>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <queue>
#include <stdexcept>
#include <thread>

const uint32_t SendBufferLength = 4 * 1024 * 1024;

typedef struct QUIC_CREDENTIAL_CONFIG_HELPER {
    QUIC_CREDENTIAL_CONFIG CredConfig;
    union {
        QUIC_CERTIFICATE_HASH CertHash;
        QUIC_CERTIFICATE_HASH_STORE CertHashStore;
        QUIC_CERTIFICATE_FILE CertFile;
        QUIC_CERTIFICATE_FILE_PROTECTED CertFileProtected;
    };
} QUIC_CREDENTIAL_CONFIG_HELPER;

bool
GetFlag(
    _In_ int argc,
    _In_reads_(argc) _Null_terminated_ const char* argv[],
    _In_z_ const char* name
    )
{
    const size_t nameLen = strlen(name);
    for (int i = 0; i < argc; i++) {
        if (_strnicmp(argv[i] + 1, name, nameLen) == 0
            && strlen(argv[i]) == nameLen + 1) {
            return TRUE;
        }
    }
    return FALSE;
}

_Ret_maybenull_ _Null_terminated_ const char*
GetValue(
    _In_ int argc,
    _In_reads_(argc) _Null_terminated_ const char* argv[],
    _In_z_ const char* name
    )
{
    const size_t nameLen = strlen(name);
    for (int i = 0; i < argc; i++) {
        if (_strnicmp(argv[i] + 1, name, nameLen) == 0
            && strlen(argv[i]) > 1 + nameLen + 1
            && *(argv[i] + 1 + nameLen) == ':') {
            return argv[i] + 1 + nameLen + 1;
        }
    }
    return NULL;
}

uint8_t
DecodeHexChar(
    _In_ char c
    )
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    return 0;
}

uint32_t
DecodeHexBuffer(
    _In_z_ const char* HexBuffer,
    _In_ uint32_t OutBufferLen,
    _Out_writes_to_(OutBufferLen, return)
        uint8_t* OutBuffer
    )
{
    uint32_t HexBufferLen = (uint32_t)strlen(HexBuffer) / 2;
    if (HexBufferLen > OutBufferLen) {
        return 0;
    }
    for (uint32_t i = 0; i < HexBufferLen; i++) {
        OutBuffer[i] =
            (DecodeHexChar(HexBuffer[i * 2]) << 4) |
            DecodeHexChar(HexBuffer[i * 2 + 1]);
    }
    return HexBufferLen;
}

#ifndef UNREFERENCED_PARAMETER
#define UNREFERENCED_PARAMETER(P) (void)(P)
#endif

const QUIC_BUFFER Alpn = { sizeof("sample") - 1, (uint8_t*)"sample" };

// ---------------------------------------------------------------------------
// SPSCRingBuffer — lock-free, single-producer / single-consumer ring.
// cap_ must be a power of two.  head_ and tail_ live on separate cache lines
// so producer and consumer never share a cache line (no false sharing).
// ---------------------------------------------------------------------------

class alignas(64) SPSCRingBuffer {
public:
    static constexpr size_t DEFAULT_CAPACITY = 1u << 23; // 8 MB

    explicit SPSCRingBuffer(size_t capacity = DEFAULT_CAPACITY)
        : cap_(capacity)
        , buf_(new uint8_t[capacity])
        , head_(0)
        , tail_(0)
    {
        if ((capacity & (capacity - 1)) != 0)
            throw std::invalid_argument("SPSCRingBuffer: capacity must be a power of 2");
    }

    ~SPSCRingBuffer() { delete[] buf_; }

    // Producer — called from MsQuic callback thread.
    // Spins until space is available, then writes atomically.
    void write(const uint8_t* src, size_t len) {
        while (true) {
            size_t head  = head_.load(std::memory_order_relaxed);
            size_t tail  = tail_.load(std::memory_order_acquire);
            size_t avail = cap_ - (head - tail);
            if (avail >= len) {
                size_t offset = head & (cap_ - 1);
                size_t first  = std::min(len, cap_ - offset);
                std::memcpy(buf_ + offset, src, first);
                if (first < len)
                    std::memcpy(buf_, src + first, len - first);
                head_.store(head + len, std::memory_order_release);
                return;
            }
            _mm_pause();
        }
    }

    // Consumer — called from Read().
    // Returns how many bytes were copied (may be < len if ring is not full enough).
    size_t read(uint8_t* dst, size_t len) {
        size_t tail  = tail_.load(std::memory_order_relaxed);
        size_t head  = head_.load(std::memory_order_acquire);
        size_t avail = head - tail;
        if (avail == 0) return 0;

        size_t n      = std::min(len, avail);
        size_t offset = tail & (cap_ - 1);
        size_t first  = std::min(n, cap_ - offset);
        std::memcpy(dst, buf_ + offset, first);
        if (first < n)
            std::memcpy(dst + first, buf_, n - first);
        tail_.store(tail + n, std::memory_order_release);
        return n;
    }

    size_t available() const {
        return head_.load(std::memory_order_acquire)
             - tail_.load(std::memory_order_relaxed);
    }

private:
    const size_t cap_;
    uint8_t*     buf_;

    alignas(64) std::atomic<size_t> head_;
    alignas(64) std::atomic<size_t> tail_;

    SPSCRingBuffer(const SPSCRingBuffer&)            = delete;
    SPSCRingBuffer& operator=(const SPSCRingBuffer&) = delete;
};

namespace gvirtus::communicators {

class QuicCommunicator : public Communicator {
 public:
    QuicCommunicator();
    QuicCommunicator(const std::string& communicator);
    QuicCommunicator(const char* hostname, short port);
    QuicCommunicator(const QuicCommunicator&);
    virtual ~QuicCommunicator();

    void InitializeQuic();
    void InitializePipes();   // allocates a fresh SPSCRingBuffer into recvRing_

    const QUIC_REGISTRATION_CONFIG RegConfig = {
        "quicsample", QUIC_EXECUTION_PROFILE_LOW_LATENCY };

    bool ServerLoadConfiguration(
        _In_ int argc,
        _In_reads_(argc) _Null_terminated_ const char* argv[]);
    bool ClientLoadConfiguration(BOOLEAN Unsecure);

    // Per-connection queue of newly accepted per-stream communicators.
    mutable std::queue<QuicCommunicator*> NewQuicCommunicatorQueue;

    // -----------------------------------------------------------------------
    // Callback wrappers (static — passed as function pointers to MsQuic)
    // -----------------------------------------------------------------------
    static unsigned int ServerStreamCallbackWrapper(
        HQUIC Stream, void* Context, QUIC_STREAM_EVENT* Event);
    static unsigned int ServerListenerCallbackWrapper(
        HQUIC Listener, void* Context, QUIC_LISTENER_EVENT* Event);
    static unsigned int ClientConnectionCallbackWrapper(
        HQUIC Connection, void* Context, QUIC_CONNECTION_EVENT* Event);
    static unsigned int ClientStreamCallbackWrapper(
        HQUIC Stream, void* Context, QUIC_STREAM_EVENT* Event);
    static unsigned int ServerConnectionCallbackWrapper(
        HQUIC Connection, void* Context, QUIC_CONNECTION_EVENT* Event);

    // -----------------------------------------------------------------------
    // Callback implementations (non-static member functions)
    // -----------------------------------------------------------------------
    _IRQL_requires_max_(PASSIVE_LEVEL)
    _Function_class_(QUIC_LISTENER_CALLBACK)
    QUIC_STATUS QUIC_API ServerListenerCallback(
        _In_ HQUIC Listener,
        _In_opt_ void* Context,
        _Inout_ QUIC_LISTENER_EVENT* Event);

    _IRQL_requires_max_(DISPATCH_LEVEL)
    _Function_class_(QUIC_STREAM_CALLBACK)
    QUIC_STATUS QUIC_API ClientStreamCallback(
        _In_ HQUIC Stream,
        _In_opt_ void* Context,
        _Inout_ QUIC_STREAM_EVENT* Event);

    _IRQL_requires_max_(DISPATCH_LEVEL)
    _Function_class_(QUIC_CONNECTION_CALLBACK)
    QUIC_STATUS QUIC_API ClientConnectionCallback(
        _In_ HQUIC Connection,
        _In_opt_ void* Context,
        _Inout_ QUIC_CONNECTION_EVENT* Event);

    _IRQL_requires_max_(DISPATCH_LEVEL)
    _Function_class_(QUIC_CONNECTION_CALLBACK)
    QUIC_STATUS QUIC_API ServerConnectionCallback(
        _In_ HQUIC Connection,
        _In_opt_ void* Context,
        _Inout_ QUIC_CONNECTION_EVENT* Event);

    _IRQL_requires_max_(DISPATCH_LEVEL)
    _Function_class_(QUIC_STREAM_CALLBACK)
    QUIC_STATUS QUIC_API ServerStreamCallback(
        _In_ HQUIC Stream,
        _In_opt_ void* Context,
        _Inout_ QUIC_STREAM_EVENT* Event);

    // -----------------------------------------------------------------------
    // Communicator interface
    // -----------------------------------------------------------------------
    void Serve();
    const Communicator* const Accept() const;
    void Connect();
    size_t Read(char* buffer, size_t size);
    size_t Write(const char* buffer, size_t size);
    void Sync();
    void Close();

    std::string to_string() override { return "quiccommunicator"; }

    // -----------------------------------------------------------------------
    // Global stream-id → ring map.
    // Accessed by MsQuic callbacks (multiple threads) — protected by
    // ringmapMutex (shared_lock for reads, unique_lock for writes).
    // -----------------------------------------------------------------------
    static std::map<QUIC_UINT62, SPSCRingBuffer*> ringmap;
    static std::shared_mutex                      ringmapMutex;

 private:
    // Per-stream receive buffer — replaces ReadPipeFds[2].
    // Owned by this communicator; allocated in InitializePipes() / Connect().
    SPSCRingBuffer* recvRing_ = nullptr;

    QUIC_UINT62 sid = 0;

    mutable std::mutex ListenerStartMutex;
    mutable bool NewConnectionEventOccurred { false };
    mutable std::condition_variable ListenerStartCv;

    std::mutex ConnectMutex;
    bool ConnectEventOccurred { false };
    std::condition_variable ConnectionStartCv;

    mutable std::mutex StreamMutex;
    mutable bool StreamEventOccurred { false };
    mutable std::condition_variable StreamStartCv;

    // MsQuic handles
    HQUIC Connection  = nullptr;
    HQUIC Stream      = nullptr;
    HQUIC Registration;
    HQUIC Configuration;
    HQUIC Listener;

    static inline const QUIC_API_TABLE* MsQuic = nullptr;

    mutable bool listener_started = false;

    std::string mHostname;
    char*       mInAddr    = nullptr;
    int         mInAddrSize = 0;
    short       mPort       = 0;
};

}  // namespace gvirtus::communicators
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

#define DEBUG

#include "QuicCommunicatorRing.h"

#ifndef _WIN32

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <stdint.h>
#include <limits.h>

#else
#include <WinSock2.h>
static bool initialized = false;
#endif

#include <gvirtus/communicators/Endpoint.h>
#include <gvirtus/communicators/Endpoint_Quic.h>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <atomic>
#include <immintrin.h>  // _mm_pause

// ---------------------------------------------------------------------------
// Debug macro
// ---------------------------------------------------------------------------

#ifdef QUICCOM_DEBUG_LEVEL
    #define DEBUG_PRINTF(fmt, ...) \
        do { printf("[tid %lu] [%s:%d] " fmt "\n", \
             syscall(SYS_gettid), __PRETTY_FUNCTION__, __LINE__, ##__VA_ARGS__); } while(0)
#else
    #define DEBUG_PRINTF(fmt, ...) do {} while(0)
#endif

using namespace std;
using gvirtus::communicators::QuicCommunicator;

// ---------------------------------------------------------------------------
// pipemap — maps stream-id → SPSCRingBuffer*
// Protected by a shared_mutex so reads (callback lookup) are concurrent
// while writes (insert/erase at stream open/close) are exclusive.
// ---------------------------------------------------------------------------

std::map<QUIC_UINT62, SPSCRingBuffer*> QuicCommunicator::ringmap;
std::shared_mutex                      QuicCommunicator::ringmapMutex;


// ---------------------------------------------------------------------------
// QuicCommunicator constructors / destructor
// ---------------------------------------------------------------------------

QuicCommunicator::QuicCommunicator(const QuicCommunicator& other)
{
    DEBUG_PRINTF("copy-ctor called");
    recvRing_       = nullptr;
    Stream          = nullptr;
    Listener        = nullptr;
    // Copy fields needed by the new per-stream communicator.
    MsQuic          = other.MsQuic;
    Registration    = other.Registration;
    Configuration   = other.Configuration;
    mHostname       = other.mHostname;
    mPort           = other.mPort;
    mInAddrSize     = other.mInAddrSize;
    mInAddr         = new char[mInAddrSize];
    std::memcpy(mInAddr, other.mInAddr, mInAddrSize);
    listener_started = false;
}

QuicCommunicator::QuicCommunicator(const std::string& communicator)
{
    DEBUG_PRINTF("string ctor called");
    recvRing_       = nullptr;
    Stream          = nullptr;
    Listener        = nullptr;

#ifdef _WIN32
    if (!initialized) {
        WSADATA data;
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
            throw "Cannot initialize WinSock.";
        initialized = true;
    }
#endif

    const char* valueptr = strstr(communicator.c_str(), "://") + 3;
    const char* portptr  = strchr(valueptr, ':');
    if (portptr == nullptr)
        throw "Port not specified.";
    mPort = (short)strtol(portptr + 1, nullptr, 10);

#ifdef _WIN32
    char* hostname = _strdup(valueptr);
#else
    char* hostname = strdup(valueptr);
#endif

    hostname[portptr - valueptr] = 0;
    mHostname = string(hostname);
    struct hostent* ent = gethostbyname(hostname);
    free(hostname);
    if (ent == nullptr)
        throw "QuicCommunicator: Can't resolve hostname '" + mHostname + "'.";
    mInAddrSize = ent->h_length;
    mInAddr     = new char[mInAddrSize];
    memcpy(mInAddr, *ent->h_addr_list, mInAddrSize);
}

QuicCommunicator::QuicCommunicator(const char* hostname, short port)
{
    DEBUG_PRINTF("hostname/port ctor called");
    MsQuic          = nullptr;
    Connection      = nullptr;
    Stream          = nullptr;
    Listener        = nullptr;
    recvRing_       = nullptr;
    listener_started = false;

    mHostname = string(hostname);
    struct hostent* ent = gethostbyname(hostname);
    if (ent == nullptr)
        throw "QuicCommunicator: Can't resolve hostname '" + mHostname + "'.";
    mInAddrSize = ent->h_length;
    mInAddr     = new char[mInAddrSize];
    memcpy(mInAddr, *ent->h_addr_list, mInAddrSize);
    mPort = port;
}

QuicCommunicator::QuicCommunicator()
{
    DEBUG_PRINTF("default ctor called");
    recvRing_        = nullptr;
    Stream           = nullptr;
    Listener         = nullptr;
    listener_started = false;
}

QuicCommunicator::~QuicCommunicator()
{
    DEBUG_PRINTF("dtor called");
    delete[] mInAddr;
    delete recvRing_;   // replaces close(ReadPipeFds[])

    if (MsQuic != nullptr) {
        if (Configuration != nullptr)
            MsQuic->ConfigurationClose(Configuration);
        if (Registration != nullptr)
            MsQuic->RegistrationClose(Registration); // blocks until children closed
        MsQuicClose(MsQuic);
    }
}

// ---------------------------------------------------------------------------
// InitializeQuic / InitializeRing
// ---------------------------------------------------------------------------

void QuicCommunicator::InitializeQuic(void)
{
    DEBUG_PRINTF("called");
    recvRing_        = nullptr;
    listener_started = false;
    Stream           = nullptr;

    QUIC_STATUS Status;
    if (QUIC_FAILED(Status = MsQuicOpen2(&MsQuic)))
        throw "MsQuicOpen2 failed.";

    if (QUIC_FAILED(Status = MsQuic->RegistrationOpen(&RegConfig, &Registration)))
        throw "RegistrationOpen failed.";

    Stream   = nullptr;
    Listener = nullptr;
}

void QuicCommunicator::InitializePipes()
{
    // "Pipes" are now ring buffers — name kept for minimal header changes.
    delete recvRing_;
    recvRing_ = new SPSCRingBuffer();
}

// ---------------------------------------------------------------------------
// Configuration helpers (unchanged logic, kept verbatim)
// ---------------------------------------------------------------------------

bool QuicCommunicator::ClientLoadConfiguration(BOOLEAN Unsecure)
{
    QUIC_SETTINGS Settings = {0};
 
    // Never drop the connection due to inactivity.
    Settings.IdleTimeoutMs        = 0;
    Settings.IsSet.IdleTimeoutMs  = TRUE;
 
    // Let MsQuic buffer sends internally so it can coalesce UDP datagrams.
    // Disabling this (FALSE) caused one StreamSend → one UDP packet, which
    // is terrible for throughput.  Let MsQuic batch them.
    Settings.SendBufferingEnabled        = TRUE;
    Settings.IsSet.SendBufferingEnabled  = TRUE;
 
    // Largest UDP payload MsQuic will attempt.  1452 fits inside a standard
    // 1500-byte Ethernet MTU with IPv4+UDP headers.  If your network supports
    // jumbo frames (9000 MTU) raise this to e.g. 8940.
    Settings.MaximumMtu        = 1452;
    Settings.IsSet.MaximumMtu  = TRUE;
 
    // How many streams the peer is allowed to open toward us.
    Settings.PeerBidiStreamCount        = 65535;
    Settings.IsSet.PeerBidiStreamCount  = TRUE;
 
    // Initial flow-control window for the whole connection (bytes).
    // Default is 16 KB which is tiny.  Set to 64 MB so the sender is never
    // stalled waiting for the receiver to grant more credits.
    Settings.ConnFlowControlWindow        = 64 * 1024 * 1024;
    Settings.IsSet.ConnFlowControlWindow  = TRUE;
 
    // Initial flow-control window per stream (bytes).  Same reasoning.
    Settings.StreamRecvWindowDefault        = 64 * 1024 * 1024;
    Settings.IsSet.StreamRecvWindowDefault  = TRUE;
 
    // How many worker threads MsQuic spins up per processor.
    // 0 = one thread per logical CPU (default, usually best).
    // Leave at 0 unless you want to pin to fewer cores.
    Settings.MaxWorkerQueueDelayUs        = 250;   // 250 µs max queue delay
    Settings.IsSet.MaxWorkerQueueDelayUs  = TRUE;
 
    // 0-RTT: allow the client to send data before the handshake completes
    // on resumed connections (saves one round-trip on reconnect).
    //Settings.ResumeEarlyDataEnabled        = TRUE;
    //Settings.IsSet.ResumeEarlyDataEnabled  = TRUE;
 
    QUIC_CREDENTIAL_CONFIG CredConfig;
    memset(&CredConfig, 0, sizeof(CredConfig));
    CredConfig.Type  = QUIC_CREDENTIAL_TYPE_NONE;
    CredConfig.Flags = QUIC_CREDENTIAL_FLAG_CLIENT;
    if (Unsecure)
        CredConfig.Flags |= QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;
 
    QUIC_STATUS Status;
    if (QUIC_FAILED(Status = MsQuic->ConfigurationOpen(
            Registration, &Alpn, 1, &Settings, sizeof(Settings), nullptr, &Configuration))) {
        printf("ConfigurationOpen failed, 0x%x!\n", Status);
        return FALSE;
    }
    if (QUIC_FAILED(Status = MsQuic->ConfigurationLoadCredential(Configuration, &CredConfig))) {
        printf("ConfigurationLoadCredential failed, 0x%x!\n", Status);
        return FALSE;
    }
    return TRUE;
}

bool QuicCommunicator::ServerLoadConfiguration(
    _In_ int argc,
    _In_reads_(argc) _Null_terminated_ const char* argv[])
{
    QUIC_SETTINGS Settings = {0};
 
    Settings.IdleTimeoutMs        = 0;
    Settings.IsSet.IdleTimeoutMs  = TRUE;
 
    // Allow 0-RTT resumption — client can send data immediately on reconnect.
    Settings.ServerResumptionLevel        = QUIC_SERVER_RESUME_AND_ZERORTT;
    Settings.IsSet.ServerResumptionLevel  = TRUE;
 
    // Maximum number of client-initiated bidirectional streams.
    Settings.PeerBidiStreamCount        = 65535;
    Settings.IsSet.PeerBidiStreamCount  = TRUE;
 
    // Buffer sends so MsQuic can coalesce multiple StreamSend calls into
    // fewer UDP packets — critical for throughput.
    Settings.SendBufferingEnabled        = TRUE;
    Settings.IsSet.SendBufferingEnabled  = TRUE;
 
    // Match the client MTU.
    Settings.MaximumMtu        = 1452;
    Settings.IsSet.MaximumMtu  = TRUE;
 
    // 64 MB connection-level flow control window.
    Settings.ConnFlowControlWindow        = 64 * 1024 * 1024;
    Settings.IsSet.ConnFlowControlWindow  = TRUE;
 
    // 64 MB per-stream receive window.
    Settings.StreamRecvWindowDefault        = 64 * 1024 * 1024;
    Settings.IsSet.StreamRecvWindowDefault  = TRUE;
 
    // Cap the MsQuic worker queue delay.
    Settings.MaxWorkerQueueDelayUs        = 250;
    Settings.IsSet.MaxWorkerQueueDelayUs  = TRUE;
 
    // Credential setup (unchanged logic).
    QUIC_CREDENTIAL_CONFIG_HELPER Config;
    memset(&Config, 0, sizeof(Config));
    Config.CredConfig.Flags = QUIC_CREDENTIAL_FLAG_NONE;

    const char* Cert;
    const char* KeyFile;
    if ((Cert = GetValue(argc, argv, "cert_hash")) != nullptr) {
        uint32_t CertHashLen = DecodeHexBuffer(
            Cert, sizeof(Config.CertHash.ShaHash), Config.CertHash.ShaHash);
        if (CertHashLen != sizeof(Config.CertHash.ShaHash))
            return FALSE;
        Config.CredConfig.Type            = QUIC_CREDENTIAL_TYPE_CERTIFICATE_HASH;
        Config.CredConfig.CertificateHash = &Config.CertHash;
    } else if ((Cert    = GetValue(argc, argv, "cert_file")) != nullptr &&
               (KeyFile = GetValue(argc, argv, "key_file"))  != nullptr) {
        const char* Password = GetValue(argc, argv, "password");
        if (Password != nullptr) {
            Config.CertFileProtected.CertificateFile    = (char*)Cert;
            Config.CertFileProtected.PrivateKeyFile     = (char*)KeyFile;
            Config.CertFileProtected.PrivateKeyPassword = (char*)Password;
            Config.CredConfig.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE_PROTECTED;
            Config.CredConfig.CertificateFileProtected = &Config.CertFileProtected;
        } else {
            Config.CertFile.CertificateFile = (char*)Cert;
            Config.CertFile.PrivateKeyFile  = (char*)KeyFile;
            Config.CredConfig.Type          = QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE;
            Config.CredConfig.CertificateFile = &Config.CertFile;
        }
    } else {
        printf("Must specify ['-cert_hash'] or ['cert_file' and 'key_file']!\n");
        return FALSE;
    }

    QUIC_STATUS Status;
    if (QUIC_FAILED(Status = MsQuic->ConfigurationOpen(
            Registration, &Alpn, 1, &Settings, sizeof(Settings), nullptr, &Configuration))) {
        printf("ConfigurationOpen failed, 0x%x!\n", Status);
        return FALSE;
    }
    if (QUIC_FAILED(Status = MsQuic->ConfigurationLoadCredential(
            Configuration, &Config.CredConfig))) {
        printf("ConfigurationLoadCredential failed, 0x%x!\n", Status);
        return FALSE;
    }
    return TRUE;
}

// ---------------------------------------------------------------------------
// Stream callbacks
// ---------------------------------------------------------------------------

// Helper: look up the ring for a stream id under a shared (read) lock.
static SPSCRingBuffer* getRing(QUIC_UINT62 streamId)
{
    std::shared_lock<std::shared_mutex> lk(QuicCommunicator::ringmapMutex);
    auto it = QuicCommunicator::ringmap.find(streamId);
    if (it == QuicCommunicator::ringmap.end()) return nullptr;
    return it->second;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
_Function_class_(QUIC_STREAM_CALLBACK)
QUIC_STATUS QUIC_API
QuicCommunicator::ServerStreamCallback(
    _In_ HQUIC Stream,
    _In_opt_ void* Context,
    _Inout_ QUIC_STREAM_EVENT* Event)
{
    auto communicator = static_cast<QuicCommunicator*>(Context);
    DEBUG_PRINTF("[sid %lu] event %d", sid, Event->Type);

    switch (Event->Type) {

    case QUIC_STREAM_EVENT_SEND_COMPLETE:
        // MsQuic is returning ownership of the buffer we passed to StreamSend.
        free(Event->SEND_COMPLETE.ClientContext);
        break;

    case QUIC_STREAM_EVENT_RECEIVE:
    {
        // Write directly into this communicator's ring — no pipemap lookup needed
        // because the Context IS the per-stream QuicCommunicator.
        SPSCRingBuffer* ring = communicator->recvRing_;
        if (ring == nullptr) {
            DEBUG_PRINTF("[sid %lu] no ring, dropping %u bytes", sid,
                         Event->RECEIVE.TotalBufferLength);
            break;
        }
        for (uint32_t i = 0; i < Event->RECEIVE.BufferCount; ++i) {
            const QUIC_BUFFER* b = &Event->RECEIVE.Buffers[i];
            DEBUG_PRINTF("[sid %lu] received %u bytes", sid, b->Length);
            ring->write(b->Buffer, b->Length);
        }
        break;
    }

    case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN:
        DEBUG_PRINTF("[sid %lu] peer shut down send", sid);
        break;

    case QUIC_STREAM_EVENT_PEER_SEND_ABORTED:
        DEBUG_PRINTF("[sid %lu] peer aborted", sid);
        MsQuic->StreamShutdown(Stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
        break;

    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
        DEBUG_PRINTF("[sid %lu] shutdown complete", sid);
        {
            // Remove from the map under an exclusive lock.
            std::unique_lock<std::shared_mutex> lk(QuicCommunicator::ringmapMutex);
            QuicCommunicator::ringmap.erase(sid);
        }
        MsQuic->StreamClose(Stream);
        break;

    default:
        break;
    }
    return QUIC_STATUS_SUCCESS;
}

unsigned int QuicCommunicator::ServerStreamCallbackWrapper(
    HQUIC Stream, void* Context, QUIC_STREAM_EVENT* Event)
{
    return static_cast<QuicCommunicator*>(Context)
               ->ServerStreamCallback(Stream, Context, Event);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
_Function_class_(QUIC_STREAM_CALLBACK)
QUIC_STATUS QUIC_API
QuicCommunicator::ClientStreamCallback(
    _In_ HQUIC Stream,
    _In_opt_ void* Context,
    _Inout_ QUIC_STREAM_EVENT* Event)
{
    auto communicator = static_cast<QuicCommunicator*>(Context);
    UNREFERENCED_PARAMETER(Stream);

    switch (Event->Type) {

    case QUIC_STREAM_EVENT_SEND_COMPLETE:
        free(Event->SEND_COMPLETE.ClientContext);
        break;

    case QUIC_STREAM_EVENT_RECEIVE:
    {
        SPSCRingBuffer* ring = communicator->recvRing_;
        if (ring == nullptr) break;
        for (uint32_t i = 0; i < Event->RECEIVE.BufferCount; ++i) {
            const QUIC_BUFFER* b = &Event->RECEIVE.Buffers[i];
            DEBUG_PRINTF("[sid %lu] received %u bytes", communicator->sid, b->Length);
            ring->write(b->Buffer, b->Length);
        }
        break;
    }

    case QUIC_STREAM_EVENT_PEER_SEND_ABORTED:
        DEBUG_PRINTF("peer aborted");
        break;

    case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN:
        DEBUG_PRINTF("peer shut down");
        break;

    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
        DEBUG_PRINTF("shutdown complete");
        if (!Event->SHUTDOWN_COMPLETE.AppCloseInProgress)
            MsQuic->StreamClose(Stream);
        break;

    default:
        break;
    }
    return QUIC_STATUS_SUCCESS;
}

unsigned int QuicCommunicator::ClientStreamCallbackWrapper(
    HQUIC Stream, void* Context, QUIC_STREAM_EVENT* Event)
{
    return static_cast<QuicCommunicator*>(Context)
               ->ClientStreamCallback(Stream, Context, Event);
}

// ---------------------------------------------------------------------------
// Connection callbacks
// ---------------------------------------------------------------------------

_IRQL_requires_max_(DISPATCH_LEVEL)
_Function_class_(QUIC_CONNECTION_CALLBACK)
QUIC_STATUS QUIC_API
QuicCommunicator::ClientConnectionCallback(
    _In_ HQUIC Connection,
    _In_opt_ void* Context,
    _Inout_ QUIC_CONNECTION_EVENT* Event)
{
    UNREFERENCED_PARAMETER(Context);
    std::unique_lock<std::mutex> lock(ConnectMutex);

    switch (Event->Type) {

    case QUIC_CONNECTION_EVENT_CONNECTED:
        DEBUG_PRINTF("[conn %p] connected", Connection);
        QuicCommunicator::Connection = Connection;
        break;

    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
        if (Event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status == QUIC_STATUS_CONNECTION_IDLE)
            printf("[conn][%p] Shut down on idle.\n", Connection);
        else
            printf("[conn][%p] Shut down by transport, 0x%x\n", Connection,
                   Event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status);
        break;

    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
        printf("[conn][%p] Shut down by peer, 0x%llu\n", Connection,
               (unsigned long long)Event->SHUTDOWN_INITIATED_BY_PEER.ErrorCode);
        break;

    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
        printf("[conn][%p] All done\n", Connection);
        if (!Event->SHUTDOWN_COMPLETE.AppCloseInProgress)
            MsQuic->ConnectionClose(Connection);
        break;

    case QUIC_CONNECTION_EVENT_RESUMPTION_TICKET_RECEIVED:
        printf("[conn][%p] Resumption ticket received (%u bytes)\n", Connection,
               Event->RESUMPTION_TICKET_RECEIVED.ResumptionTicketLength);
        break;

    default:
        break;
    }

    ConnectEventOccurred = true;
    ConnectionStartCv.notify_one();
    return QUIC_STATUS_SUCCESS;
}

unsigned int QuicCommunicator::ClientConnectionCallbackWrapper(
    HQUIC Connection, void* Context, QUIC_CONNECTION_EVENT* Event)
{
    return static_cast<QuicCommunicator*>(Context)
               ->ClientConnectionCallback(Connection, Context, Event);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
_Function_class_(QUIC_CONNECTION_CALLBACK)
QUIC_STATUS QUIC_API
QuicCommunicator::ServerConnectionCallback(
    _In_ HQUIC Connection,
    _In_opt_ void* Context,
    _Inout_ QUIC_CONNECTION_EVENT* Event)
{
    DEBUG_PRINTF("[conn %p] event %d", Connection, Event->Type);
    UNREFERENCED_PARAMETER(Context);
    uint32_t sidSize = sizeof(sid);

    switch (Event->Type) {

    case QUIC_CONNECTION_EVENT_CONNECTED:
        printf("[conn][%p] Connected\n", Connection);
        MsQuic->ConnectionSendResumptionTicket(
            Connection, QUIC_SEND_RESUMPTION_FLAG_NONE, 0, nullptr);
        break;

    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
        if (Event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status == QUIC_STATUS_CONNECTION_IDLE)
            printf("[conn][%p] Shut down on idle.\n", Connection);
        else
            printf("[conn][%p] Shut down by transport, 0x%x\n", Connection,
                   Event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status);
        break;

    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
        printf("[conn][%p] Shut down by peer, 0x%llu\n", Connection,
               (unsigned long long)Event->SHUTDOWN_INITIATED_BY_PEER.ErrorCode);
        break;

    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
        printf("[conn][%p] All done\n", Connection);
        MsQuic->ConnectionClose(Connection);
        break;

    case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED:
    {
        QUIC_UINT62 tmpsid;
        MsQuic->GetParam(Event->PEER_STREAM_STARTED.Stream,
                         QUIC_PARAM_STREAM_ID, &sidSize, &tmpsid);
        DEBUG_PRINTF("[strm %p] peer started, sid=%lu",
                     Event->PEER_STREAM_STARTED.Stream, tmpsid);

        // Allocate a new per-stream communicator.
        QuicCommunicator* NewComm = new QuicCommunicator(*this);
        NewComm->InitializePipes();   // creates a fresh SPSCRingBuffer
        NewComm->Stream     = Event->PEER_STREAM_STARTED.Stream;
        NewComm->Connection = Connection;
        NewComm->sid        = tmpsid;

        // Register in the ringmap under an exclusive lock.
        {
            std::unique_lock<std::shared_mutex> lk(QuicCommunicator::ringmapMutex);
            QuicCommunicator::ringmap.emplace(tmpsid, NewComm->recvRing_);
        }

        MsQuic->SetCallbackHandler(Event->PEER_STREAM_STARTED.Stream,
                                   (void*)ServerStreamCallbackWrapper, NewComm);

        NewQuicCommunicatorQueue.push(NewComm);
        StreamEventOccurred = true;
        StreamStartCv.notify_one();
        break;
    }

    case QUIC_CONNECTION_EVENT_RESUMED:
        printf("[conn][%p] Connection resumed!\n", Connection);
        break;

    default:
        printf("Unknown connection event %d\n", Event->Type);
        break;
    }
    return QUIC_STATUS_SUCCESS;
}

unsigned int QuicCommunicator::ServerConnectionCallbackWrapper(
    HQUIC Connection, void* Context, QUIC_CONNECTION_EVENT* Event)
{
    DEBUG_PRINTF("called");
    return static_cast<QuicCommunicator*>(Context)
               ->ServerConnectionCallback(Connection, Context, Event);
}

// ---------------------------------------------------------------------------
// Listener callback
// ---------------------------------------------------------------------------

_IRQL_requires_max_(PASSIVE_LEVEL)
_Function_class_(QUIC_LISTENER_CALLBACK)
QUIC_STATUS QUIC_API
QuicCommunicator::ServerListenerCallback(
    _In_ HQUIC Listener,
    _In_opt_ void* Context,
    _Inout_ QUIC_LISTENER_EVENT* Event)
{
    DEBUG_PRINTF("called");
    UNREFERENCED_PARAMETER(Listener);

    std::unique_lock<std::mutex> lock(ListenerStartMutex);
    int stream_count = 65535;

    QUIC_STATUS Status = QUIC_STATUS_NOT_SUPPORTED;

    switch (Event->Type) {

    case QUIC_LISTENER_EVENT_NEW_CONNECTION:
    {
        printf("QUIC_LISTENER_EVENT_NEW_CONNECTION\n");
        HQUIC newConn = Event->NEW_CONNECTION.Connection;  // NOT the class member
        MsQuic->SetCallbackHandler(newConn, (void*)ServerConnectionCallbackWrapper, Context);
        Status = MsQuic->ConnectionSetConfiguration(newConn, Configuration);

        // FIX: use newConn, not the stale class-member 'Connection' (which may be null here).
        MsQuic->SetParam(newConn, QUIC_PARAM_CONN_LOCAL_BIDI_STREAM_COUNT,
                         sizeof(stream_count), &stream_count);
        MsQuic->SetParam(newConn, QUIC_PARAM_CONN_LOCAL_UNIDI_STREAM_COUNT,
                         sizeof(stream_count), &stream_count);

        NewConnectionEventOccurred = true;
        ListenerStartCv.notify_one();
        break;
    }

    default:
        break;
    }

    return Status;
}

unsigned int QuicCommunicator::ServerListenerCallbackWrapper(
    HQUIC Listener, void* Context, QUIC_LISTENER_EVENT* Event)
{
    DEBUG_PRINTF("called");
    return static_cast<QuicCommunicator*>(Context)
               ->ServerListenerCallback(Listener, Context, Event);
}

// ---------------------------------------------------------------------------
// Serve / Accept / Connect / Close
// ---------------------------------------------------------------------------

void QuicCommunicator::Serve()
{
    printf("QuicCommunicator::Serve() called\n");

    InitializeQuic();

    int argc = 2;
    const char* argv[2];
    argv[0] = (char*)"-cert_file:" GVIRTUS_HOME "/etc/server.cert";
    argv[1] = (char*)"-key_file:"  GVIRTUS_HOME "/etc/server.key";

    printf("Load Server Config\n");
    if (!ServerLoadConfiguration(argc, argv))
        return;

    QUIC_STATUS Status;
    if (QUIC_FAILED(Status = MsQuic->ListenerOpen(
            Registration, ServerListenerCallbackWrapper, this, &Listener)))
        throw "ListenerOpen failed";

    sleep(1);
    printf("QuicCommunicator::Serve() returned\n");
}

const gvirtus::communicators::Communicator* const QuicCommunicator::Accept() const
{
    printf("QuicCommunicator::Accept() called\n");

    QUIC_STATUS Status;
    QUIC_ADDR Address = {0};
    QuicAddrSetFamily(&Address, QUIC_ADDRESS_FAMILY_UNSPEC);
    QuicAddrSetPort(&Address, htons(mPort));

    if (!listener_started) {
        std::unique_lock<std::mutex> lock(ListenerStartMutex);
        printf("MsQuic->ListenerStart called %p\n", Listener);
        if (QUIC_FAILED(Status = MsQuic->ListenerStart(Listener, &Alpn, 1, &Address)))
            throw "ListenerStart failed";
        listener_started = true;

        printf("MsQuic->ListenerStart wait\n");
        ListenerStartCv.wait(lock, [this] { return NewConnectionEventOccurred; });
        NewConnectionEventOccurred = false;
        printf("QuicCommunicator::Accept() connection ready\n");
    }

    std::unique_lock<std::mutex> slock(StreamMutex);
    DEBUG_PRINTF("waiting for stream");
    StreamStartCv.wait(slock, [this] {
        return !NewQuicCommunicatorQueue.empty() || StreamEventOccurred;
    });

    QuicCommunicator* NewComm = nullptr;
    if (!NewQuicCommunicatorQueue.empty()) {
        StreamEventOccurred = false;
        NewComm = NewQuicCommunicatorQueue.front();
        NewQuicCommunicatorQueue.pop();
        DEBUG_PRINTF("returning new stream communicator");
    } else {
        DEBUG_PRINTF("queue empty after notify");
    }
    return NewComm;
}

void QuicCommunicator::Connect()
{
    printf("QuicCommunicator::Connect() called\n");
 
    int argc = 1;
    const char* argv[] = { "-unsecure" };
    QUIC_STATUS Status;
 
    if (QuicCommunicator::Connection == nullptr) {
        std::unique_lock<std::mutex> lock(ConnectMutex);
 
        InitializeQuic();
 
        if (!ClientLoadConfiguration(GetFlag(argc, argv, "unsecure")))
            return;
 
        if (QUIC_FAILED(Status = MsQuic->ConnectionOpen(
                Registration, ClientConnectionCallbackWrapper, this, &Connection))) {
            printf("ConnectionOpen failed, 0x%x!\n", Status);
            throw "ConnectionOpen failed";
        }
 
        // --------------------------------------------------------
        // Per-connection throughput knobs applied before Start().
        // --------------------------------------------------------
 
        // How many bidirectional streams we will open toward the server.
        uint16_t streamCount = 65535;
        MsQuic->SetParam(Connection, QUIC_PARAM_CONN_LOCAL_BIDI_STREAM_COUNT,
                         sizeof(streamCount), &streamCount);
 
        // Disable the pacing algorithm — pacing spreads sends over time to
        // reduce burst loss on WAN.  On LAN/localhost it just adds latency.
        BOOLEAN pacing = FALSE;
        MsQuic->SetParam(Connection, QUIC_PARAM_CONN_SEND_PACING,
                         sizeof(pacing), &pacing);
 
        printf("[conn][%p] Connecting...\n", Connection);
 
        if (QUIC_FAILED(Status = MsQuic->ConnectionStart(
                Connection, Configuration, QUIC_ADDRESS_FAMILY_UNSPEC,
                mHostname.data(), htons(mPort)))) {
            printf("ConnectionStart failed, 0x%x!\n", Status);
            throw "ConnectionStart failed";
        }
 
        ConnectionStartCv.wait(lock, [this] { return ConnectEventOccurred; });
    } else {
        printf("Connection already open\n");
    }
 
    // Open and start a bidirectional stream.
    if (QUIC_FAILED(Status = MsQuic->StreamOpen(
            Connection, QUIC_STREAM_OPEN_FLAG_NONE,
            ClientStreamCallbackWrapper, this, &Stream))) {
        printf("StreamOpen failed, 0x%x!\n", Status);
        throw "StreamOpen failed";
    }
 
    if (QUIC_FAILED(Status = MsQuic->StreamStart(Stream, QUIC_STREAM_START_FLAG_NONE))) {
        printf("StreamStart failed, 0x%x!\n", Status);
        MsQuic->StreamClose(Stream);
        throw "StreamStart failed";
    }
 
    uint32_t sidSize = sizeof(sid);
    MsQuic->GetParam(Stream, QUIC_PARAM_STREAM_ID, &sidSize, &sid);
 
    delete recvRing_;
    recvRing_ = new SPSCRingBuffer();
 
    {
        std::unique_lock<std::shared_mutex> lk(QuicCommunicator::ringmapMutex);
        QuicCommunicator::ringmap.emplace(sid, recvRing_);
    }
 
    printf("QuicCommunicator::Connect() returned\n");
}

void QuicCommunicator::Close()
{
    printf("QuicCommunicator::Close\n");
    if (Stream     != nullptr) MsQuic->StreamClose(Stream);
    if (Connection != nullptr) MsQuic->ConnectionClose(Connection);
}

// ---------------------------------------------------------------------------
// Read — consumes from the SPSC ring; no syscall, no kernel involvement.
// ---------------------------------------------------------------------------

size_t QuicCommunicator::Read(char* buffer, size_t size)
{
    DEBUG_PRINTF("[sid %lu] Read(%zu)", sid, size);

    size_t done = 0;
    while (done < size) {
        size_t n = recvRing_->read(
            reinterpret_cast<uint8_t*>(buffer) + done, size - done);
        if (n == 0) {
            // Ring is empty.  Spin with a PAUSE hint — avoids memory-order
            // thrashing and burns ~10 ns instead of a full yield.
            _mm_pause();
            continue;
        }
        done += n;
    }

    DEBUG_PRINTF("[sid %lu] Read returned %zu", sid, done);
    return done;
}

// ---------------------------------------------------------------------------
// Write — async zero-copy StreamSend.
// The QUIC_BUFFER is freed in SEND_COMPLETE via free(ClientContext).
// QUIC_STATUS_PENDING (0x1) is a success code — do NOT treat it as an error.
// ---------------------------------------------------------------------------

size_t QuicCommunicator::Write(const char* buffer, size_t size)
{
    DEBUG_PRINTF("[sid %lu] Write(%zu)", sid, size);

    const size_t CHUNK = 1u << 20; // 1 MB — tune to measured cwnd
    size_t sent = 0;

    while (sent < size) {
        size_t chunk = std::min(CHUNK, size - sent);

        // Single allocation: QUIC_BUFFER header immediately followed by payload.
        // MsQuic owns this memory until SEND_COMPLETE, where we free it.
        uint8_t* raw = (uint8_t*)malloc(sizeof(QUIC_BUFFER) + chunk);
        if (raw == nullptr) {
            printf("Write: malloc failed\n");
            return sent;
        }

        QUIC_BUFFER* qb = (QUIC_BUFFER*)raw;
        qb->Buffer = raw + sizeof(QUIC_BUFFER);
        qb->Length = (uint32_t)chunk;
        std::memcpy(qb->Buffer, buffer + sent, chunk);

        QUIC_STATUS st = MsQuic->StreamSend(
            Stream, qb, 1, QUIC_SEND_FLAG_NONE, raw /*ClientContext*/);

        // PENDING means "queued OK, will call SEND_COMPLETE later" — not an error.
        if (QUIC_FAILED(st) && st != QUIC_STATUS_PENDING) {
            printf("StreamSend failed, 0x%x!\n", st);
            free(raw);
            return sent;
        }

        sent += chunk;
    }

    DEBUG_PRINTF("[sid %lu] Write sent %zu bytes", sid, sent);
    return size;
}

void QuicCommunicator::Sync()
{
    // Nothing to flush — sends are async, receives are ring-buffered.
}

// ---------------------------------------------------------------------------
// Factory function (extern "C" for dynamic loading)
// ---------------------------------------------------------------------------

extern "C" std::shared_ptr<QuicCommunicator> create_communicator(
    std::shared_ptr<gvirtus::communicators::Endpoint> end)
{
    std::string arg =
        "quic://" +
        std::dynamic_pointer_cast<gvirtus::communicators::Endpoint_Quic>(end)->address() +
        ":" +
        std::to_string(
            std::dynamic_pointer_cast<gvirtus::communicators::Endpoint_Quic>(end)->port());
    return std::make_shared<QuicCommunicator>(arg);
}
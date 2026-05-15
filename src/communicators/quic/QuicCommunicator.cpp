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

/**
 * @file   QuicCommunicator.cpp
 * @author Giuseppe Coviello <giuseppe.coviello@uniparthenope.it>
 * @date   Thu Oct 8 12:08:33 2009
 *
 * @brief
 *
 *
 */
#define DEBUG

#include "QuicCommunicator.h"

#ifndef _WIN32

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <fcntl.h>

#include <unistd.h>
#include <poll.h>

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
#include <condition_variable>

//#define QUICCOM_DEBUG_LEVEL 0
//import quic settings
#include "gvirtus/common/JSON.h"
#include "QuicSettings.h"

using namespace std;
using gvirtus::communicators::QuicCommunicator;

std::map<QUIC_UINT62, int> QuicCommunicator::pipemap;


#ifdef QUICCOM_DEBUG_LEVEL
    #define DEBUG_PRINTF(fmt, ...) do { printf("[tid %lu] [%s:%d] " fmt "\n", syscall(SYS_gettid),__PRETTY_FUNCTION__, __LINE__, ##__VA_ARGS__); } while(0)
#else
    #define DEBUG_PRINTF(fmt, ...) do {} while(0)
#endif


static inline long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

// Set the socket to non-blocking
int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}



// Does a non-blocking write to a pipe, currently not used
// Return 0 on success (all bytes written), -1 on error.
// If timeout_ms >= 0, the function will time out and set errno=ETIMEDOUT.
int QuicCommunicator::write_all_nonblocking(int fd, const void *buf, ssize_t len, int timeout_ms) {
    
    printf("[TRACE] write_all_nonblocking called: fd=%d len=%zd timeout_ms=%d\n", fd, len, timeout_ms);
    DEBUG_PRINTF("called");

    const uint8_t *p = (const uint8_t *)buf;
    ssize_t rem = len;
    const long deadline = (timeout_ms >= 0) ? now_ms() + timeout_ms : -1;
    set_nonblocking(fd);
    while (rem > 0) {
        ssize_t chunk = rem > (size_t)SSIZE_MAX ? (size_t)SSIZE_MAX : rem;
        ssize_t n = write(fd, p, chunk);
        if (n > 0) {
            printf("[TRACE] write_all_nonblocking: wrote %zd bytes, %zd remaining\n", n, rem - n);
            DEBUG_PRINTF("bytes written \n");
            p += (size_t)n;
            rem -= (size_t)n;
            continue;
        }
        if (n == -1) {
            printf("[TRACE] write_all_nonblocking: write error errno=%d (%s)\n", errno, strerror(errno));
            DEBUG_PRINTF("error %d \n", errno);
            if (errno == EINTR) continue;

            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                int wait_ms = -1;
                printf("[TRACE] write_all_nonblocking: EAGAIN/EWOULDBLOCK, polling...\n");
                DEBUG_PRINTF("wait");
                if (deadline != -1) {
                    long left = deadline - now_ms();
                    if (left <= 0) { 
                            errno = ETIMEDOUT;
                            printf("[TRACE] write_all_nonblocking: ETIMEDOUT (deadline exceeded before poll)\n");
                            printf("ETIMEDOUT1\n");
                        }
                    wait_ms = (left > INT_MAX) ? INT_MAX : (int)left;
                }

                struct pollfd pfd = { .fd = fd, .events = POLLOUT, .revents = 0 };
                int r;
                do { r = poll(&pfd, 1, wait_ms); } while (r < 0 && errno == EINTR);
                if (r == 0) { 
                    errno = ETIMEDOUT; 
                    printf("[TRACE] write_all_nonblocking: ETIMEDOUT (poll timed out)\n");
                    DEBUG_PRINTF("ETIMEDOUT2");
                    }
                if (r < 0) {
                    printf("[TRACE] write_all_nonblocking: poll error errno=%d (%s)\n", errno, strerror(errno));
                    return -1;
                }

                // If the read end was closed, writes will fail with EPIPE.
                if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                    errno = EPIPE;
                    printf("[TRACE] write_all_nonblocking: EPIPE detected (revents=0x%x)\n", pfd.revents);
                    DEBUG_PRINTF("EPIPE");
                }
                // Ready to try write() again.
                continue;
            }

            // Other hard errors (e.g., EPIPE if no reader)
        }
    }
    printf("[TRACE] write_all_nonblocking: completed successfully\n");
    return 0;
}


QuicCommunicator::QuicCommunicator(const QuicCommunicator& other)
{
    printf("[TRACE] QuicCommunicator copy constructor called: Connection=%p\n", QuicCommunicator::Connection);
    DEBUG_PRINTF("called with connection %p", QuicCommunicator::Connection);
    Stream = NULL;
    Listener = NULL;
}

QuicCommunicator::QuicCommunicator(const std::string &communicator) {
    printf("[TRACE] QuicCommunicator string constructor called: communicator='%s'\n", communicator.c_str());
    DEBUG_PRINTF("called with connection %p", QuicCommunicator::Connection);
    Stream = NULL;
    Listener = NULL;

#ifdef _WIN32
    if (!initialized) {
      WSADATA data;
      if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
        throw "Cannot initialized WinSock.";
      initialized = true;
    }
#endif

    const char *valueptr = strstr(communicator.c_str(), "://") + 3;
    const char *portptr = strchr(valueptr, ':');
    if (portptr == NULL)
        throw "Port not specified.";
    mPort = (short) strtol(portptr + 1, NULL, 10);

#ifdef _WIN32
    char *hostname = _strdup(valueptr);
#else
    char *hostname = strdup(valueptr);
#endif

    hostname[portptr - valueptr] = 0;
    mHostname = string(hostname);
    struct hostent *ent = gethostbyname(hostname);
    free(hostname);
    if (ent == NULL)
        throw "QuicCommunicator: Can't resolve hostname '" + mHostname + "'.";
    mInAddrSize = ent->h_length;
    mInAddr = new char[mInAddrSize];
    memcpy(mInAddr, *ent->h_addr_list, mInAddrSize);

    printf("[TRACE] QuicCommunicator string constructor: hostname='%s' port=%d resolved OK\n", mHostname.c_str(), mPort);
}

//
// Helper function to load a client configuration.
//
bool QuicCommunicator::ClientLoadConfiguration(
    BOOLEAN Unsecure
    )
{
    printf("[TRACE] ClientLoadConfiguration called: Unsecure=%d\n", Unsecure);
    DEBUG_PRINTF("called");

    //
    // Load QUIC settings from JSON file.
    //
    QUIC_SETTINGS Settings = {0};
    try {
        const char* gvirtus_home = getenv("GVIRTUS_HOME");
        if (gvirtus_home == nullptr) {
            printf("[TRACE] ClientLoadConfiguration: GVIRTUS_HOME not set\n");
            printf("GVIRTUS_HOME environment variable not set\n");
            return FALSE;
        }
        printf("[TRACE] ClientLoadConfiguration: GVIRTUS_HOME='%s'\n", gvirtus_home);
        fs::path settingsPath = fs::path(gvirtus_home) / "etc" / "quic_settings.json";
        printf("[TRACE] ClientLoadConfiguration: loading settings from '%s'\n", settingsPath.c_str());
        gvirtus::common::JSON<QuicSettingsConfig> jsonLoader(settingsPath);
        Settings = jsonLoader.parser().ToQuicSettings();
        printf("[TRACE] ClientLoadConfiguration: settings loaded OK\n");
        DEBUG_PRINTF("Loaded settings from %s", settingsPath.c_str());
    } catch (const std::ifstream::failure& e) {
        printf("[TRACE] ClientLoadConfiguration: ifstream failure: %s\n", e.what());
        printf("Failed to load quic_settings.json: %s\n", e.what());
        return FALSE;
    } catch (const nlohmann::json::exception& e) {
        printf("[TRACE] ClientLoadConfiguration: JSON parse failure: %s\n", e.what());
        printf("Failed to parse quic_settings.json: %s\n", e.what());
        return FALSE;
    }

    //
    // Configures a default client configuration, optionally disabling
    // server certificate validation.
    //
    QUIC_CREDENTIAL_CONFIG CredConfig;
    memset(&CredConfig, 0, sizeof(CredConfig));
    CredConfig.Type = QUIC_CREDENTIAL_TYPE_NONE;
    CredConfig.Flags = QUIC_CREDENTIAL_FLAG_CLIENT;
    if (Unsecure) {
        CredConfig.Flags |= QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;
        printf("[TRACE] ClientLoadConfiguration: certificate validation disabled\n");
    }

    //
    // Allocate/initialize the configuration object, with the configured ALPN
    // and settings.
    //
    QUIC_STATUS Status = QUIC_STATUS_SUCCESS;
    printf("[TRACE] ClientLoadConfiguration: calling ConfigurationOpen\n");
    if (QUIC_FAILED(Status = MsQuic->ConfigurationOpen(Registration, &Alpn, 1, &Settings, sizeof(Settings), NULL, &Configuration))) {
        printf("[TRACE] ClientLoadConfiguration: ConfigurationOpen FAILED status=0x%x\n", Status);
        printf("ConfigurationOpen failed, 0x%x!\n", Status);
        return FALSE;
    }
    printf("[TRACE] ClientLoadConfiguration: ConfigurationOpen OK, calling ConfigurationLoadCredential\n");

    //
    // Loads the TLS credential part of the configuration. This is required even
    // on client side, to indicate if a certificate is required or not.
    //
    if (QUIC_FAILED(Status = MsQuic->ConfigurationLoadCredential(Configuration, &CredConfig))) {
        printf("[TRACE] ClientLoadConfiguration: ConfigurationLoadCredential FAILED status=0x%x\n", Status);
        printf("ConfigurationLoadCredential failed, 0x%x!\n", Status);
        return FALSE;
    }

    printf("[TRACE] ClientLoadConfiguration: completed successfully\n");
    return TRUE;
}


//
// Helper function to load a server configuration. Uses the command line
// arguments to load the credential part of the configuration.
//
bool 
QuicCommunicator::ServerLoadConfiguration(
    _In_ int argc,
    _In_reads_(argc) _Null_terminated_ const char* argv[]
    )
{
    printf("[TRACE] ServerLoadConfiguration called: argc=%d\n", argc);
    for (int i = 0; i < argc; i++) {
        printf("[TRACE] ServerLoadConfiguration: argv[%d]='%s'\n", i, argv[i]);
    }

    QUIC_SETTINGS Settings = {0};
    //
    // Configures the server's idle timeout.
    //
    Settings.IdleTimeoutMs = 0;
    Settings.IsSet.IdleTimeoutMs = TRUE;
    //
    // Configures the server's resumption level to allow for resumption and
    // 0-RTT.
    //
    Settings.ServerResumptionLevel = QUIC_SERVER_RESUME_AND_ZERORTT;
    Settings.IsSet.ServerResumptionLevel = TRUE;
    //
    // Configures the server's settings to allow for the peer to open a single
    // bidirectional stream. By default connections are not configured to allow
    // any streams from the peer.
    //
    Settings.PeerBidiStreamCount = 65535;
    Settings.IsSet.PeerBidiStreamCount = TRUE;

    QUIC_CREDENTIAL_CONFIG_HELPER Config;
    memset(&Config, 0, sizeof(Config));
    Config.CredConfig.Flags = QUIC_CREDENTIAL_FLAG_NONE;

    const char* Cert;
    const char* KeyFile;
    if ((Cert = GetValue(argc, argv, "cert_hash")) != NULL) {
        printf("[TRACE] ServerLoadConfiguration: using cert_hash mode\n");
        uint32_t CertHashLen =
            DecodeHexBuffer(
                Cert,
                sizeof(Config.CertHash.ShaHash),
                Config.CertHash.ShaHash);
        if (CertHashLen != sizeof(Config.CertHash.ShaHash)) {
            printf("[TRACE] ServerLoadConfiguration: cert_hash decode failed (len=%u expected=%zu)\n",
                   CertHashLen, sizeof(Config.CertHash.ShaHash));
            return FALSE;
        }
        Config.CredConfig.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_HASH;
        Config.CredConfig.CertificateHash = &Config.CertHash;

    } else if ((Cert = GetValue(argc, argv, "cert_file")) != NULL &&
               (KeyFile = GetValue(argc, argv, "key_file")) != NULL) {
        printf("[TRACE] ServerLoadConfiguration: using cert_file='%s' key_file='%s'\n", Cert, KeyFile);
        const char* Password = GetValue(argc, argv, "password");
        if (Password != NULL) {
            printf("[TRACE] ServerLoadConfiguration: password-protected key detected\n");
            Config.CertFileProtected.CertificateFile = (char*)Cert;
            Config.CertFileProtected.PrivateKeyFile = (char*)KeyFile;
            Config.CertFileProtected.PrivateKeyPassword = (char*)Password;
            Config.CredConfig.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE_PROTECTED;
            Config.CredConfig.CertificateFileProtected = &Config.CertFileProtected;
        } else {
            Config.CertFile.CertificateFile = (char*)Cert;
            Config.CertFile.PrivateKeyFile = (char*)KeyFile;
            Config.CredConfig.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE;
            Config.CredConfig.CertificateFile = &Config.CertFile;
        }

    } else {
        printf("[TRACE] ServerLoadConfiguration: no valid cert arguments found\n");
        printf("Must specify ['-cert_hash'] or ['cert_file' and 'key_file' (and optionally 'password')]!\n");
        return FALSE;
    }

    QUIC_STATUS Status = QUIC_STATUS_SUCCESS;
    printf("[TRACE] ServerLoadConfiguration: calling ConfigurationOpen\n");
    if (QUIC_FAILED(Status = MsQuic->ConfigurationOpen(Registration, &Alpn, 1, &Settings, sizeof(Settings), NULL, &Configuration))) {
        printf("[TRACE] ServerLoadConfiguration: ConfigurationOpen FAILED status=0x%x\n", Status);
        printf("ConfigurationOpen failed, 0x%x!\n", Status);
        return FALSE;
    }
    printf("[TRACE] ServerLoadConfiguration: ConfigurationOpen OK\n");

    printf("[TRACE] ServerLoadConfiguration: calling ConfigurationLoadCredential\n");
    if (QUIC_FAILED(Status = MsQuic->ConfigurationLoadCredential(Configuration, &Config.CredConfig))) {
        printf("[TRACE] ServerLoadConfiguration: ConfigurationLoadCredential FAILED status=0x%x\n", Status);
        printf("ConfigurationLoadCredential failed, 0x%x!\n", Status);
        return FALSE;
    }

    printf("[TRACE] ServerLoadConfiguration: completed successfully\n");
    return TRUE;
}

QuicCommunicator::QuicCommunicator(const char *hostname, short port) {
    pid_t tid = syscall(SYS_gettid);
    printf("[TRACE] QuicCommunicator(hostname, port) called: hostname='%s' port=%d tid=%d\n", hostname, port, tid);
    DEBUG_PRINTF("called with connection %p", QuicCommunicator::Connection);
    MsQuic = NULL;
    Connection = NULL;
    Stream = NULL;
    Listener = NULL;
    listener_started=false;

    mHostname = string(hostname);
    struct hostent *ent = gethostbyname(hostname);
    if (ent == NULL)
        throw "QuicCommunicator: Can't resolve hostname '" + mHostname + "'.";
    mInAddrSize = ent->h_length;
    mInAddr = new char[mInAddrSize];
    memcpy(mInAddr, *ent->h_addr_list, mInAddrSize);
    mPort = port;

    printf("[TRACE] QuicCommunicator(hostname, port): hostname resolved OK\n");

    QUIC_STATUS Status = QUIC_STATUS_SUCCESS;
}

QuicCommunicator::QuicCommunicator() {
    pid_t tid = syscall(SYS_gettid);
    printf("[TRACE] QuicCommunicator() default constructor called: tid=%d Connection=%p\n", tid, QuicCommunicator::Connection);
    DEBUG_PRINTF("called with connection %p", QuicCommunicator::Connection);
    Stream = NULL;
    Listener = NULL;
    listener_started=false;
}


void QuicCommunicator::InitializeQuic(void) {
    printf("[TRACE] InitializeQuic called\n");
    DEBUG_PRINTF("called");
    Stream = NULL;
    listener_started=false;

    QUIC_STATUS Status = QUIC_STATUS_SUCCESS;

    printf("[TRACE] InitializeQuic: calling MsQuicOpen2\n");
    if (QUIC_FAILED(Status = MsQuicOpen2(&MsQuic))) {
        printf("[TRACE] InitializeQuic: MsQuicOpen2 FAILED status=0x%x\n", Status);
        throw "MsQuicOpen2 failed.";
    }
    printf("[TRACE] InitializeQuic: MsQuicOpen2 OK MsQuic=%p\n", MsQuic);

    printf("[TRACE] InitializeQuic: calling RegistrationOpen\n");
    if (QUIC_FAILED(Status = MsQuic->RegistrationOpen(&RegConfig, &Registration))) {
        printf("[TRACE] InitializeQuic: RegistrationOpen FAILED status=0x%x\n", Status);
        throw "RegistrationOpen failed.";
    }
    printf("[TRACE] InitializeQuic: RegistrationOpen OK Registration=%p\n", Registration);

    Stream = NULL;
    Listener = NULL;
}

void gvirtus::communicators::QuicCommunicator::InitializePipes()
    {
        printf("[TRACE] InitializePipes called\n");
        if (pipe(ReadPipeFds) == -1) {
            printf("[TRACE] InitializePipes: pipe() FAILED errno=%d (%s)\n", errno, strerror(errno));
            printf("Failed to create pipe\n");
            throw "Failed to create pipe";
        }
        int pipe_size = 4096 * 4096 * 4; // 1MB buffer or adjust as needed
        int r0 = fcntl(ReadPipeFds[0], F_SETPIPE_SZ, pipe_size);
        int r1 = fcntl(ReadPipeFds[1], F_SETPIPE_SZ, pipe_size);
        printf("[TRACE] InitializePipes: created pipe read_fd=%d write_fd=%d actual_sizes=%d/%d\n",
               ReadPipeFds[0], ReadPipeFds[1], r0, r1);
    }


QuicCommunicator::~QuicCommunicator() {
    printf("[TRACE] ~QuicCommunicator called: MsQuic=%p Connection=%p Stream=%p\n", MsQuic, Connection, Stream);
    DEBUG_PRINTF("called");
    delete[] mInAddr;
    printf("[TRACE] ~QuicCommunicator: closing pipes read_fd=%d write_fd=%d\n", ReadPipeFds[0], ReadPipeFds[1]);
    close(ReadPipeFds[0]);
    close(ReadPipeFds[1]);
    
    if (MsQuic != NULL) {
        if (Configuration != NULL) {
            printf("[TRACE] ~QuicCommunicator: closing Configuration=%p\n", Configuration);
            MsQuic->ConfigurationClose(Configuration);
        }
        if (Registration != NULL) {
            printf("[TRACE] ~QuicCommunicator: closing Registration=%p (will block until children closed)\n", Registration);
            MsQuic->RegistrationClose(Registration);
        }
        printf("[TRACE] ~QuicCommunicator: calling MsQuicClose\n");
        MsQuicClose(MsQuic);
    }
    printf("[TRACE] ~QuicCommunicator: done\n");
}

_IRQL_requires_max_(DISPATCH_LEVEL)
_Function_class_(QUIC_STREAM_CALLBACK)
QUIC_STATUS
QUIC_API
QuicCommunicator::ServerStreamCallback(
    _In_ HQUIC Stream,
    _In_opt_ void* Context,
    _Inout_ QUIC_STREAM_EVENT* Event
    )
{
    auto communicator = static_cast<QuicCommunicator*>(Context);
    QUIC_BUFFER* qb=NULL;
    printf("[TRACE] ServerStreamCallback: Stream=%p Event->Type=%d sid=%lu\n", Stream, Event->Type, sid);
    DEBUG_PRINTF("[sid %lu] called", sid);
    int wp = -1;
    if (QuicCommunicator::pipemap.find(sid) != QuicCommunicator::pipemap.end()) {
        wp = QuicCommunicator::pipemap[sid];
        printf("[TRACE] ServerStreamCallback: found pipe wp=%d for sid=%lu\n", wp, sid);
        DEBUG_PRINTF("[sid %lu] Get pipe %d %p %d\n",sid, wp,Stream,Event->Type);
    }
    else {
        printf("[TRACE] ServerStreamCallback: pipe NOT found for sid=%lu, returning early\n", sid);
        DEBUG_PRINTF("[sid %lu] Pipe not found pipe %d %p %d\n",sid, wp,Stream,Event->Type);
        return QUIC_STATUS_SUCCESS;
    }
    switch (Event->Type) {
    case QUIC_STREAM_EVENT_SEND_COMPLETE:
       qb = (QUIC_BUFFER*)Event->SEND_COMPLETE.ClientContext;
        printf("[TRACE] ServerStreamCallback: SEND_COMPLETE Stream=%p len=%u\n", Stream, qb ? qb->Length : 0);
        DEBUG_PRINTF("[strm][%p] Data sent %d\n", Stream, qb->Length);
        free(Event->SEND_COMPLETE.ClientContext);
        break;
    case QUIC_STREAM_EVENT_RECEIVE:
        printf("[TRACE] ServerStreamCallback: RECEIVE Stream=%p BufferCount=%u\n", Stream, Event->RECEIVE.BufferCount);
        for (uint32_t i = 0; i < Event->RECEIVE.BufferCount; ++i) {
            const QUIC_BUFFER* b = &Event->RECEIVE.Buffers[i];
            printf("[TRACE] ServerStreamCallback: RECEIVE buffer[%u] len=%u flags=%d pipe_fd=%d\n",
                   i, b->Length, Event->RECEIVE.Flags, wp);
            DEBUG_PRINTF("[sid %lu] [strm %p] [pipe %d] Data received %u, flags %d\n", sid, Stream, wp, b->Length, Event->RECEIVE.Flags);
            if (write(wp, b->Buffer, b->Length) == -1) {
                printf("[TRACE] ServerStreamCallback: write to pipe FAILED errno=%d (%s)\n", errno, strerror(errno));
                printf("Failed to write to pipe\n");
                throw "Failed to write to pipe";
            }
            printf("[TRACE] ServerStreamCallback: wrote %u bytes to pipe fd=%d\n", b->Length, wp);
            DEBUG_PRINTF("[sid %lu] [strm %p] [pipe %d] Data written %u, flags %d\n", sid, Stream, wp, b->Length, Event->RECEIVE.Flags);
        }
        break;
    case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN:
        printf("[TRACE] ServerStreamCallback: PEER_SEND_SHUTDOWN Stream=%p\n", Stream);
        DEBUG_PRINTF("[strm][%p] Peer shut down\n", Stream);
        break;
    case QUIC_STREAM_EVENT_PEER_SEND_ABORTED:
        printf("[TRACE] ServerStreamCallback: PEER_SEND_ABORTED Stream=%p\n", Stream);
        DEBUG_PRINTF("[strm][%p] Peer aborted\n", Stream);
        MsQuic->StreamShutdown(Stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
        break;
    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
        printf("[TRACE] ServerStreamCallback: SHUTDOWN_COMPLETE Stream=%p sid=%lu, erasing from pipemap\n", Stream, sid);
        DEBUG_PRINTF("[strm][%p] All done\n", Stream);
        QuicCommunicator::pipemap.erase(sid);
        MsQuic->StreamClose(Stream);
        break;
    default:
        printf("[TRACE] ServerStreamCallback: unhandled event type=%d Stream=%p\n", Event->Type, Stream);
        break;
    }
    return QUIC_STATUS_SUCCESS;
}

unsigned int QuicCommunicator::ClientStreamCallbackWrapper(HQUIC Stream, void* Context, QUIC_STREAM_EVENT* Event) {
        printf("[TRACE] ClientStreamCallbackWrapper: Stream=%p Event->Type=%d\n", Stream, Event->Type);
        auto communicator = static_cast<QuicCommunicator*>(Context);
        return communicator->ClientStreamCallback(Stream, Context, Event);
    }

_IRQL_requires_max_(DISPATCH_LEVEL)
_Function_class_(QUIC_STREAM_CALLBACK)
QUIC_STATUS
QUIC_API
QuicCommunicator::ClientStreamCallback(
    _In_ HQUIC Stream,
    _In_opt_ void* Context,
    _Inout_ QUIC_STREAM_EVENT* Event
    )
{
    UNREFERENCED_PARAMETER(Context);
    printf("[TRACE] ClientStreamCallback: Stream=%p Event->Type=%d sid=%lu\n", Stream, Event->Type, sid);
    int wp = -1;
    if (QuicCommunicator::pipemap.find(sid) != QuicCommunicator::pipemap.end()) {
        wp = QuicCommunicator::pipemap[sid];
        printf("[TRACE] ClientStreamCallback: found pipe wp=%d for sid=%lu\n", wp, sid);
        DEBUG_PRINTF("Get pipe %lu %d %p %d\n",sid, wp,Stream,Event->Type);
    }
    else {
        printf("[TRACE] ClientStreamCallback: pipe NOT found for sid=%lu, returning early\n", sid);
        DEBUG_PRINTF("Pipe not found pipe %lu %d %p %d\n",sid, wp,Stream,Event->Type);
        return QUIC_STATUS_SUCCESS;
    }

    QUIC_BUFFER* qb=NULL;

    switch (Event->Type) {
    case QUIC_STREAM_EVENT_SEND_COMPLETE:
        qb = (QUIC_BUFFER*)Event->SEND_COMPLETE.ClientContext;
        printf("[TRACE] ClientStreamCallback: SEND_COMPLETE Stream=%p len=%u\n", Stream, qb ? qb->Length : 0);
        DEBUG_PRINTF("[strm][%p] Data sent %d\n", Stream, qb->Length) ;
        free(Event->SEND_COMPLETE.ClientContext);
        break;
    case QUIC_STREAM_EVENT_RECEIVE:
        printf("[TRACE] ClientStreamCallback: RECEIVE Stream=%p BufferCount=%u\n", Stream, Event->RECEIVE.BufferCount);
        for (uint32_t i = 0; i < Event->RECEIVE.BufferCount; ++i) {
            const QUIC_BUFFER* b = &Event->RECEIVE.Buffers[i];
            printf("[TRACE] ClientStreamCallback: RECEIVE buffer[%u] len=%u flags=%d pipe_fd=%d\n",
                   i, b->Length, Event->RECEIVE.Flags, wp);
            DEBUG_PRINTF("[strm][%p][sid %lu][pipe %d] Data received %u, flags %d\n", Stream, sid, wp, b->Length, Event->RECEIVE.Flags);
            if (write(wp, b->Buffer, b->Length) == -1) {
                printf("[TRACE] ClientStreamCallback: write to pipe FAILED errno=%d (%s)\n", errno, strerror(errno));
                printf("Failed to write to pipe\n");
                throw "Failed to write to pipe";
            }
            printf("[TRACE] ClientStreamCallback: wrote %u bytes to pipe fd=%d\n", b->Length, wp);
        }
        DEBUG_PRINTF("[strm][%p] Data received\n", Stream);
        break;
    case QUIC_STREAM_EVENT_PEER_SEND_ABORTED:
        printf("[TRACE] ClientStreamCallback: PEER_SEND_ABORTED Stream=%p\n", Stream);
        DEBUG_PRINTF("[strm][%p] Peer aborted\n", Stream);
        break;
    case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN:
        printf("[TRACE] ClientStreamCallback: PEER_SEND_SHUTDOWN Stream=%p\n", Stream);
        DEBUG_PRINTF("[strm][%p] Peer shut down\n", Stream);
        break;
    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
        printf("[TRACE] ClientStreamCallback: SHUTDOWN_COMPLETE Stream=%p AppCloseInProgress=%d\n",
               Stream, Event->SHUTDOWN_COMPLETE.AppCloseInProgress);
        DEBUG_PRINTF("[strm][%p] All done\n", Stream);
        if (!Event->SHUTDOWN_COMPLETE.AppCloseInProgress) {
            MsQuic->StreamClose(Stream);
        }
        break;
    default:
        printf("[TRACE] ClientStreamCallback: unhandled event type=%d Stream=%p\n", Event->Type, Stream);
        break;
    }
    return QUIC_STATUS_SUCCESS;
}


unsigned int QuicCommunicator::ClientConnectionCallbackWrapper(HQUIC Stream, void* Context, QUIC_CONNECTION_EVENT* Event) {
        printf("[TRACE] ClientConnectionCallbackWrapper: Connection=%p Event->Type=%d\n", Stream, Event->Type);
        auto communicator = static_cast<QuicCommunicator*>(Context);
        return communicator->ClientConnectionCallback(Stream, Context, Event);
    }

//
// The clients's callback for connection events from MsQuic.
//
_IRQL_requires_max_(DISPATCH_LEVEL)
_Function_class_(QUIC_CONNECTION_CALLBACK)
QUIC_STATUS
QUIC_API
QuicCommunicator::ClientConnectionCallback(
    _In_ HQUIC Connection,
    _In_opt_ void* Context,
    _Inout_ QUIC_CONNECTION_EVENT* Event
    )
{
    UNREFERENCED_PARAMETER(Context);
    printf("[TRACE] ClientConnectionCallback: Connection=%p Event->Type=%d\n", Connection, Event->Type);
    std::unique_lock<std::mutex> lock(ConnectMutex);
    switch (Event->Type) {
    case QUIC_CONNECTION_EVENT_CONNECTED:
        printf("[TRACE] ClientConnectionCallback: CONNECTED Connection=%p\n", Connection);
        DEBUG_PRINTF("[conn][%p] Connected\n", Connection);
        QuicCommunicator::Connection=Connection;
        break;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
        if (Event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status == QUIC_STATUS_CONNECTION_IDLE) {
            printf("[TRACE] ClientConnectionCallback: SHUTDOWN_BY_TRANSPORT (idle) Connection=%p\n", Connection);
            printf("[conn][%p] Successfully shut down on idle.\n", Connection);
        } else {
            printf("[TRACE] ClientConnectionCallback: SHUTDOWN_BY_TRANSPORT status=0x%x Connection=%p\n",
                   Event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status, Connection);
            printf("[conn][%p] Shut down by transport, 0x%x\n", Connection, Event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status);
        }
        break;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
        printf("[TRACE] ClientConnectionCallback: SHUTDOWN_BY_PEER ErrorCode=%llu Connection=%p\n",
               (unsigned long long)Event->SHUTDOWN_INITIATED_BY_PEER.ErrorCode, Connection);
        printf("[conn][%p] Shut down by peer, 0x%llu\n", Connection, (unsigned long long)Event->SHUTDOWN_INITIATED_BY_PEER.ErrorCode);
        break;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
        printf("[TRACE] ClientConnectionCallback: SHUTDOWN_COMPLETE Connection=%p AppCloseInProgress=%d\n",
               Connection, Event->SHUTDOWN_COMPLETE.AppCloseInProgress);
        printf("[conn][%p] All done\n", Connection);
        if (!Event->SHUTDOWN_COMPLETE.AppCloseInProgress) {
            MsQuic->ConnectionClose(Connection);
        }
        break;
    case QUIC_CONNECTION_EVENT_RESUMPTION_TICKET_RECEIVED:
        printf("[TRACE] ClientConnectionCallback: RESUMPTION_TICKET_RECEIVED len=%u Connection=%p\n",
               Event->RESUMPTION_TICKET_RECEIVED.ResumptionTicketLength, Connection);
        printf("[conn][%p] Resumption ticket received (%u bytes):\n", Connection, Event->RESUMPTION_TICKET_RECEIVED.ResumptionTicketLength);
        for (uint32_t i = 0; i < Event->RESUMPTION_TICKET_RECEIVED.ResumptionTicketLength; i++) {
            printf("%.2X", (uint8_t)Event->RESUMPTION_TICKET_RECEIVED.ResumptionTicket[i]);
        }
        printf("\n");
        break;
    default:
        printf("[TRACE] ClientConnectionCallback: unhandled event type=%d Connection=%p\n", Event->Type, Connection);
        break;
    }
    ConnectEventOccurred = true;
    ConnectionStartCv.notify_one();
    printf("[TRACE] ClientConnectionCallback: notified ConnectionStartCv\n");
    return QUIC_STATUS_SUCCESS;
}


unsigned int QuicCommunicator::ServerStreamCallbackWrapper(HQUIC Stream, void* Context, QUIC_STREAM_EVENT* Event) {
        printf("[TRACE] ServerStreamCallbackWrapper: Stream=%p Event->Type=%d\n", Stream, Event->Type);
        auto communicator = static_cast<QuicCommunicator*>(Context);
        return communicator->ServerStreamCallback(Stream, Context, Event);
    }

_IRQL_requires_max_(DISPATCH_LEVEL)
_Function_class_(QUIC_CONNECTION_CALLBACK)
QUIC_STATUS
QUIC_API
QuicCommunicator::ServerConnectionCallback(
    _In_ HQUIC Connection,
    _In_opt_ void* Context,
    _Inout_ QUIC_CONNECTION_EVENT* Event
    )
{
    printf("[TRACE] ServerConnectionCallback: Connection=%p Event->Type=%d MsQuic=%p\n", Connection, Event->Type, MsQuic);
    DEBUG_PRINTF("[conn %p] [msquic %p] %s event type %d\n", Connection, MsQuic, __PRETTY_FUNCTION__, Event->Type);
    UNREFERENCED_PARAMETER(Context);
    uint32_t sidSize = sizeof(sid);

    switch (Event->Type) {
    case QUIC_CONNECTION_EVENT_CONNECTED:
        printf("[TRACE] ServerConnectionCallback: CONNECTED Connection=%p\n", Connection);
        printf("[conn][%p] Connected\n", Connection);
        MsQuic->ConnectionSendResumptionTicket(Connection, QUIC_SEND_RESUMPTION_FLAG_NONE, 0, NULL);
        break;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
        if (Event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status == QUIC_STATUS_CONNECTION_IDLE) {
            printf("[TRACE] ServerConnectionCallback: SHUTDOWN_BY_TRANSPORT (idle) Connection=%p\n", Connection);
            printf("[conn][%p] Successfully shut down on idle.\n", Connection);
        } else {
            printf("[TRACE] ServerConnectionCallback: SHUTDOWN_BY_TRANSPORT status=0x%x Connection=%p\n",
                   Event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status, Connection);
            printf("[conn][%p] Shut down by transport, 0x%x\n", Connection, Event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status);
        }
        break;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
        printf("[TRACE] ServerConnectionCallback: SHUTDOWN_BY_PEER ErrorCode=%llu Connection=%p\n",
               (unsigned long long)Event->SHUTDOWN_INITIATED_BY_PEER.ErrorCode, Connection);
        printf("[conn][%p] Shut down by peer, 0x%llu\n", Connection, (unsigned long long)Event->SHUTDOWN_INITIATED_BY_PEER.ErrorCode);
        break;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
        printf("[TRACE] ServerConnectionCallback: SHUTDOWN_COMPLETE Connection=%p\n", Connection);
        printf("[conn][%p] All done\n", Connection);
        MsQuic->ConnectionClose(Connection);
        break;
    case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED:
        {
            QUIC_UINT62 tmpsid;
            MsQuic->GetParam(Event->PEER_STREAM_STARTED.Stream, QUIC_PARAM_STREAM_ID, &sidSize, &tmpsid);
            printf("[TRACE] ServerConnectionCallback: PEER_STREAM_STARTED Stream=%p sid=%lu Connection=%p\n",
                   Event->PEER_STREAM_STARTED.Stream, tmpsid, Connection);
            DEBUG_PRINTF("[strm][%p] Peer started witd id %lu\n", Event->PEER_STREAM_STARTED.Stream, tmpsid);

            QuicCommunicator * NewStreamQuicCommunicator = new QuicCommunicator(*this);
            printf("[TRACE] ServerConnectionCallback: created NewStreamQuicCommunicator=%p\n", NewStreamQuicCommunicator);
            NewStreamQuicCommunicator->InitializePipes();

            QuicCommunicator::pipemap.insert(std::pair(tmpsid, NewStreamQuicCommunicator->ReadPipeFds[1]));
            printf("[TRACE] ServerConnectionCallback: inserted pipemap sid=%lu write_fd=%d read_fd=%d\n",
                   tmpsid, NewStreamQuicCommunicator->ReadPipeFds[1], NewStreamQuicCommunicator->ReadPipeFds[0]);
            DEBUG_PRINTF("[strm %p] pipes inserted %d %d \n", Event->PEER_STREAM_STARTED.Stream, NewStreamQuicCommunicator->ReadPipeFds[0], NewStreamQuicCommunicator->ReadPipeFds[1]);

            NewStreamQuicCommunicator->Stream = Event->PEER_STREAM_STARTED.Stream;
            NewStreamQuicCommunicator->Connection = Connection;
            NewStreamQuicCommunicator->sid = tmpsid;
            MsQuic->SetCallbackHandler(Event->PEER_STREAM_STARTED.Stream, (void *) ServerStreamCallbackWrapper, NewStreamQuicCommunicator);
            printf("[TRACE] ServerConnectionCallback: SetCallbackHandler done for Stream=%p\n", Event->PEER_STREAM_STARTED.Stream);

            NewQuicCommunicatorQueue.push(NewStreamQuicCommunicator);
            StreamEventOccurred = true;
            StreamStartCv.notify_one();
            printf("[TRACE] ServerConnectionCallback: queued new communicator, notified StreamStartCv\n");
            break;
        }

    case QUIC_CONNECTION_EVENT_RESUMED:
        printf("[TRACE] ServerConnectionCallback: RESUMED Connection=%p\n", Connection);
        printf("[conn][%p] Connection resumed!\n", Connection);
        break;
    default:
        printf("[TRACE] ServerConnectionCallback: unhandled event type=%d Connection=%p\n", Event->Type, Connection);
        printf("Unkown Connection event %i", Event->Type);
        break;
    }
    return QUIC_STATUS_SUCCESS;
}


unsigned int QuicCommunicator::ServerConnectionCallbackWrapper(HQUIC Connection, void* Context, QUIC_CONNECTION_EVENT* Event) {
        printf("[TRACE] ServerConnectionCallbackWrapper: Connection=%p Event->Type=%d\n", Connection, Event->Type);
        DEBUG_PRINTF("called");
        auto communicator = static_cast<QuicCommunicator*>(Context);
        return communicator->ServerConnectionCallback(Connection, Context, Event);
    }
  

//
// The server's callback for listener events from MsQuic.
//
_IRQL_requires_max_(PASSIVE_LEVEL)
_Function_class_(QUIC_LISTENER_CALLBACK)
QUIC_STATUS
QUIC_API
QuicCommunicator::ServerListenerCallback(
    _In_ HQUIC Listener,
    _In_opt_ void* Context,
    _Inout_ QUIC_LISTENER_EVENT* Event
    )
{
    printf("[TRACE] ServerListenerCallback: Listener=%p Event->Type=%d\n", Listener, Event->Type);
    DEBUG_PRINTF("called");
    int stream_count = 65535;
    std::unique_lock<std::mutex> lock(ListenerStartMutex);
    UNREFERENCED_PARAMETER(Listener);
    int c = 1024;
    QUIC_STATUS Status = QUIC_STATUS_NOT_SUPPORTED;
    switch (Event->Type) {
    case QUIC_LISTENER_EVENT_NEW_CONNECTION:
        printf("[TRACE] ServerListenerCallback: NEW_CONNECTION Connection=%p\n", Event->NEW_CONNECTION.Connection);
        printf("QUIC_LISTENER_EVENT_NEW_CONNECTION\n");
        MsQuic->SetCallbackHandler(Event->NEW_CONNECTION.Connection, (void*)ServerConnectionCallbackWrapper, Context);
        Status = MsQuic->ConnectionSetConfiguration(Event->NEW_CONNECTION.Connection, Configuration);
        printf("[TRACE] ServerListenerCallback: ConnectionSetConfiguration status=0x%x\n", Status);
        MsQuic->SetParam(Connection, QUIC_PARAM_CONN_LOCAL_BIDI_STREAM_COUNT, sizeof(stream_count), &stream_count);
        MsQuic->SetParam(Connection, QUIC_PARAM_CONN_LOCAL_UNIDI_STREAM_COUNT, sizeof(stream_count), &stream_count);

        NewConnectionEventOccurred = true;
        ListenerStartCv.notify_one();
        printf("[TRACE] ServerListenerCallback: notified ListenerStartCv\n");
        break;
    default:
        printf("[TRACE] ServerListenerCallback: unhandled event type=%d\n", Event->Type);
        break;
    }
    
    return Status;
}

unsigned int QuicCommunicator::ServerListenerCallbackWrapper(HQUIC Listener, void* Context, QUIC_LISTENER_EVENT* Event) {
        printf("[TRACE] ServerListenerCallbackWrapper: Listener=%p Event->Type=%d\n", Listener, Event->Type);
        DEBUG_PRINTF("called");
        auto communicator = static_cast<QuicCommunicator*>(Context);
        return communicator->ServerListenerCallback(Listener, Context, Event);
    }


void QuicCommunicator::Serve() {
    printf("[TRACE] Serve called\n");
#ifdef DEBUG
    printf("QuicCommunicator::Serve() called\n");
#endif

    QUIC_STATUS Status;

    InitializeQuic(); 
    
    int argc = 2;
    const char * argv[2];
    
    argv[0] = (char *)"-cert_file:" GVIRTUS_HOME "/etc/server.cert";
    argv[1] = (char *)"-key_file:" GVIRTUS_HOME "/etc/server.key";

    printf("Load Server Config\n");
    printf("[TRACE] Serve: cert_file arg='%s'\n", argv[0]);
    printf("[TRACE] Serve: key_file arg='%s'\n", argv[1]);
    if (!ServerLoadConfiguration(argc, argv)) {
        printf("[TRACE] Serve: ServerLoadConfiguration FAILED, returning\n");
        return;
    }

    printf("[TRACE] Serve: calling ListenerOpen\n");
    if (QUIC_FAILED(Status = MsQuic->ListenerOpen(Registration, ServerListenerCallbackWrapper, this, &Listener))) {
        printf("[TRACE] Serve: ListenerOpen FAILED status=0x%x\n", Status);
        throw "ListenerOpen failed";
    }
    printf("[TRACE] Serve: ListenerOpen OK Listener=%p, sleeping 1s\n", Listener);
    sleep(1);
#ifdef DEBUG
    printf("QuicCommunicator::Serve() returned\n");
#endif
    printf("[TRACE] Serve: completed\n");
}

const gvirtus::communicators::Communicator *const QuicCommunicator::Accept() const {
    printf("[TRACE] Accept called\n");
    printf("QuicCommunicator::Accept() called\n");
    QUIC_STATUS Status;

    QUIC_ADDR Address = {0};
    QuicAddrSetFamily(&Address, QUIC_ADDRESS_FAMILY_UNSPEC);
    QuicAddrSetPort(&Address, htons(mPort));
    printf("[TRACE] Accept: listening on port=%d\n", mPort);

    if (listener_started==false){
        std::unique_lock<std::mutex> lock(ListenerStartMutex);
        printf("[TRACE] Accept: calling ListenerStart Listener=%p\n", Listener);
        printf("MsQuic->ListenerStart called %p\n", Listener);
        if (QUIC_FAILED(Status = MsQuic->ListenerStart(Listener, &Alpn, 1, &Address))) {
            printf("[TRACE] Accept: ListenerStart FAILED status=0x%x\n", Status);
            throw "ListenerStart failed";
        }
        listener_started=true;
        printf("[TRACE] Accept: ListenerStart OK, waiting for first connection\n");

        printf("MsQuic->ListenerStart wait\n");
        ListenerStartCv.wait(lock, [this] { return NewConnectionEventOccurred; });
        NewConnectionEventOccurred = false;
        printf("[TRACE] Accept: first connection received\n");
        printf("QuicCommunicator::Accept() returned\n");
    }

    std::unique_lock<std::mutex> slock(StreamMutex);    
    printf("[TRACE] Accept: waiting for new stream\n");
    DEBUG_PRINTF("Wait for Stream\n");
    StreamStartCv.wait(slock, [this] { return (!NewQuicCommunicatorQueue.empty() || StreamEventOccurred); });
    printf("[TRACE] Accept: stream event received, queue size=%zu StreamEventOccurred=%d\n",
           NewQuicCommunicatorQueue.size(), StreamEventOccurred);
    DEBUG_PRINTF("New Stream\n");
    QuicCommunicator * NewStreamQuicCommunicator = NULL;
    if (!NewQuicCommunicatorQueue.empty()) {
        StreamEventOccurred = false;
        NewStreamQuicCommunicator = NewQuicCommunicatorQueue.front();
        NewQuicCommunicatorQueue.pop();
        printf("[TRACE] Accept: dequeued NewStreamQuicCommunicator=%p\n", NewStreamQuicCommunicator);
        DEBUG_PRINTF("QuicCommunicator::Streamstart() returned\n");
    } else {
        printf("[TRACE] Accept: queue was empty despite event signal\n");
        DEBUG_PRINTF("No QuicCommunicator in queue\n");
    }
    printf("[TRACE] Accept: returning communicator=%p\n", NewStreamQuicCommunicator);
    return NewStreamQuicCommunicator;
}

void QuicCommunicator::Connect() {
    printf("[TRACE] Connect called\n");
#ifdef DEBUG
    printf("QuicCommunicator::Connect() called\n");
#endif
    DEBUG_PRINTF("QuicCommunicator::Connect() called\n");
    
    int argc = 1;
    const char * argv[1];
    argv[0] = (char *)"-unsecure";
    int stream_count = 65535;
    QUIC_STATUS Status;
    const char* ResumptionTicketString = NULL;
    pid_t tid = syscall(SYS_gettid);

    if (QuicCommunicator::Connection == NULL) {
        printf("[TRACE] Connect: Connection is NULL, establishing new connection\n");
        printf("Connection is null\n");
        std::unique_lock<std::mutex> lock(ConnectMutex);

        InitializeQuic(); 

        printf("[TRACE] Connect: calling ClientLoadConfiguration unsecure=%d\n", GetFlag(argc, argv, "unsecure"));
        if (!ClientLoadConfiguration(GetFlag(argc, argv, "unsecure"))) {
            printf("[TRACE] Connect: ClientLoadConfiguration FAILED, returning\n");
            return;
        }

        printf("[TRACE] Connect: calling ConnectionOpen\n");
        if (QUIC_FAILED(Status = MsQuic->ConnectionOpen(Registration, ClientConnectionCallbackWrapper, this, &Connection))) {
            printf("[TRACE] Connect: ConnectionOpen FAILED status=0x%x\n", Status);
            printf("ConnectionOpen failed, 0x%x!\n", Status);
            throw "ConnectionOpen failed";
        }
        printf("[TRACE] Connect: ConnectionOpen OK Connection=%p\n", Connection);

        MsQuic->SetParam(Connection, QUIC_PARAM_CONN_LOCAL_BIDI_STREAM_COUNT, sizeof(stream_count), &stream_count);
        MsQuic->SetParam(Connection, QUIC_PARAM_CONN_LOCAL_UNIDI_STREAM_COUNT, sizeof(stream_count), &stream_count);
        printf("[TRACE] Connect: stream count params set to %d\n", stream_count);

        printf("[TRACE] Connect: calling ConnectionStart hostname='%s' port=%d\n", mHostname.data(), mPort);
        printf("[conn][%p] Connecting...\n", QuicCommunicator::Connection);

        if (QUIC_FAILED(Status = MsQuic->ConnectionStart(QuicCommunicator::Connection, Configuration, QUIC_ADDRESS_FAMILY_UNSPEC, mHostname.data(), htons(mPort)))) {
            printf("[TRACE] Connect: ConnectionStart FAILED status=0x%x\n", Status);
            printf("ConnectionStart failed, 0x%x!\n", Status);
            throw "ConnectionStart failed";
        }
        printf("[TRACE] Connect: ConnectionStart OK, waiting for ConnectEventOccurred\n");

        ConnectionStartCv.wait(lock, [this] {return ConnectEventOccurred; });
        printf("[TRACE] Connect: connection established\n");
    } 
    else {
        printf("[TRACE] Connect: Connection already open Connection=%p\n", QuicCommunicator::Connection);
        printf("Connection is open\n");
    }

    printf("[TRACE] Connect: calling StreamOpen Connection=%p\n", Connection);
    DEBUG_PRINTF("[conn %p]  [msquic %p] %s open stream \n", Connection, MsQuic, __PRETTY_FUNCTION__);

    if (QUIC_FAILED(Status = MsQuic->StreamOpen(Connection, QUIC_STREAM_OPEN_FLAG_NONE, ClientStreamCallbackWrapper, this, &Stream))) {
        printf("[TRACE] Connect: StreamOpen FAILED status=0x%x\n", Status);
        printf("StreamOpen failed, 0x%x!\n", Status);
        throw "StreamOpen failed";
    }
    printf("[TRACE] Connect: StreamOpen OK Stream=%p\n", Stream);

    DEBUG_PRINTF("[sid %lu] [conn %p] %s start stream \n", sid, Connection, __PRETTY_FUNCTION__);

    printf("[TRACE] Connect: calling StreamStart Stream=%p\n", Stream);
    if (QUIC_FAILED(Status = MsQuic->StreamStart(Stream, QUIC_STREAM_START_FLAG_NONE))) {
        printf("[TRACE] Connect: StreamStart FAILED status=0x%x\n", Status);
        printf("StreamStart failed, 0x%x!\n", Status);
        MsQuic->StreamClose(Stream);
        throw "StreamStart failed";
    }
    printf("[TRACE] Connect: StreamStart OK\n");

    uint32_t sidSize = sizeof(sid);
    MsQuic->GetParam(Stream, QUIC_PARAM_STREAM_ID, &sidSize, &sid);
    printf("[TRACE] Connect: stream ID obtained sid=%lu\n", sid);

    printf("[TRACE] Connect: creating pipe\n");
    if (pipe(ReadPipeFds) == -1) {
        printf("[TRACE] Connect: pipe() FAILED errno=%d (%s)\n", errno, strerror(errno));
        printf("Failed to create pipe\n");
        throw "Failed to create pipe";
    }

    int pipe_size = 4096 * 4096 * 4;
    int r0 = fcntl(ReadPipeFds[0], F_SETPIPE_SZ, pipe_size);
    int r1 = fcntl(ReadPipeFds[1], F_SETPIPE_SZ, pipe_size);
    printf("[TRACE] Connect: pipe created read_fd=%d write_fd=%d actual_sizes=%d/%d\n",
           ReadPipeFds[0], ReadPipeFds[1], r0, r1);

    DEBUG_PRINTF("Pipe created %lu %d %d %p\n", sid, ReadPipeFds[0], ReadPipeFds[1],Stream);
    DEBUG_PRINTF("Insert pipe %lu %d %p\n", sid, ReadPipeFds[1],Stream);

    QuicCommunicator::pipemap.insert(std::pair(sid, ReadPipeFds[1]));
    printf("[TRACE] Connect: inserted pipemap sid=%lu write_fd=%d pipemap.size=%zu\n",
           sid, ReadPipeFds[1], QuicCommunicator::pipemap.size());

    DEBUG_PRINTF("[strm][%p][%u][%lu] Starting...\n", Stream, tid, sid);

#ifdef DEBUG
    printf("QuicCommunicator::Connect() returned\n");
#endif
    printf("[TRACE] Connect: completed\n");
}

void QuicCommunicator::Close() {
    printf("[TRACE] Close called: Stream=%p Connection=%p\n", Stream, Connection);
    printf("QuicCommunicator::Close\n");
    if (Stream!=NULL) {
        printf("[TRACE] Close: closing Stream=%p\n", Stream);
        MsQuic->StreamClose(Stream);
    }
    if (Connection != NULL) {
        printf("[TRACE] Close: closing Connection=%p\n", Connection);
        MsQuic->ConnectionClose(Connection);
    }
    printf("[TRACE] Close: completed\n");
}

size_t QuicCommunicator::Read(char *buffer, size_t size) {

    printf("[TRACE] Read called: sid=%lu size=%zu pipe_read_fd=%d\n", sid, size, ReadPipeFds[0]);
    DEBUG_PRINTF("[sid %lu] %s called with size %lu to pipe %d\n", sid, __PRETTY_FUNCTION__, size, ReadPipeFds[0]);

    ssize_t ret_value=0;
    ssize_t size_left=size;

    while(size_left>0) {
        DEBUG_PRINTF("[sid %lu] QuicCommunicator::Read() Block on read() %ld %lu %lu\n", sid, ret_value,size,size_left);
        ssize_t r = read(ReadPipeFds[0], buffer+ret_value, size_left);
        if (r < 0) {
            printf("[TRACE] Read: read() error errno=%d (%s) sid=%lu\n", errno, strerror(errno), sid);
            DEBUG_PRINTF("errno %d\n",errno);
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            continue;
        }
        
        if (r < 0 || r==0){
            printf("[TRACE] Read: read() returned %zd (zero/negative), resetting ret_value sid=%lu\n", r, sid);
            ret_value = 0;
            continue;
        }
        else {
            ret_value += r;
            size_left=size_left-r;
            printf("[TRACE] Read: read() returned %zd bytes, total=%zd remaining=%zd sid=%lu\n",
                   r, ret_value, size_left, sid);
            if (size_left == 0)
                break;
        }
        DEBUG_PRINTF("[sid %lu] Read return value: %ld %ld %lu %lu\n",sid ,r,ret_value,size,size_left);
    }

    printf("[TRACE] Read: completed ret_value=%zd size=%zu sid=%lu\n", ret_value, size, sid);
    DEBUG_PRINTF("returned %zu\n", ret_value);

    return ret_value;
}

size_t QuicCommunicator::Write(const char *buffer, size_t size) {
    printf("[TRACE] Write called: sid=%lu size=%zu Stream=%p\n", sid, size, Stream);
#ifdef DEBUG
    printf("QuicCommunicator::Write() called\n");
#endif

    DEBUG_PRINTF("[sid %lu] %s called with size %lu", sid, __PRETTY_FUNCTION__, size);

    QUIC_STATUS Status;

    size_t MAX_BUF_SIZE = 4096*4096*4;

    size_t size_left = size;
    size_t send_size = 0;
    size_t send_size_cum = 0;

    printf("[TRACE] Write: MAX_BUF_SIZE=%zu total_size=%zu Stream=%p\n", MAX_BUF_SIZE, size, Stream);
    DEBUG_PRINTF("[strm][%p] Sending data total... %ld", Stream, size);

    while (size_left>0)
    {
        uint8_t* SendBufferRaw;
        QUIC_BUFFER* SendBuffer;
        
        if (size_left > MAX_BUF_SIZE)
            send_size = MAX_BUF_SIZE;
        else
            send_size = size_left;
        
        printf("[TRACE] Write: allocating chunk send_size=%zu size_left=%zu send_size_cum=%zu\n",
               send_size, size_left, send_size_cum);
        SendBufferRaw = (uint8_t*)malloc(sizeof(QUIC_BUFFER) + send_size);
        if (SendBufferRaw == NULL) {
            printf("[TRACE] Write: malloc FAILED for send_size=%zu\n", send_size);
            printf("SendBuffer allocation failed!\n");
            Status = QUIC_STATUS_OUT_OF_MEMORY;
            goto Error;
        }
        printf("memcpy\n");
        memcpy(SendBufferRaw+sizeof(QUIC_BUFFER), buffer+send_size_cum, send_size);
        SendBuffer = (QUIC_BUFFER*)SendBufferRaw;
        SendBuffer->Buffer = SendBufferRaw + sizeof(QUIC_BUFFER);
        SendBuffer->Length = send_size;

        printf("[TRACE] Write: calling StreamSend Stream=%p send_size=%zu\n", Stream, send_size);
        DEBUG_PRINTF("[strm %p] Sending data... %ld %ld\n", Stream, send_size, sizeof(QUIC_BUFFER));
        send_size_cum += send_size;
        size_left -= send_size;

        if (QUIC_FAILED(Status = MsQuic->StreamSend(Stream, SendBuffer, 1, QUIC_SEND_FLAG_NONE, SendBuffer))) {
            printf("[TRACE] Write: StreamSend FAILED status=0x%x Stream=%p\n", Status, Stream);
            printf("StreamSend failed, 0x%x!\n", Status);
            free(SendBufferRaw);
            goto Error;
        }
        printf("[TRACE] Write: StreamSend OK send_size_cum=%zu size_left=%zu\n", send_size_cum, size_left);
    }
    
Error:
#ifdef DEBUG
    printf("QuicCommunicator::Read() returned %zu\n", size);
#endif
    printf("[TRACE] Write: completed total_sent=%zu\n", size);

    return size;
}

void QuicCommunicator::Sync() {
    printf("[TRACE] Sync called (no-op)\n");
}

extern "C" std::shared_ptr <QuicCommunicator> create_communicator(
        std::shared_ptr <gvirtus::communicators::Endpoint> end) {
    printf("[TRACE] create_communicator called\n");
    std::string arg =
            "quic://" +
            std::dynamic_pointer_cast<gvirtus::communicators::Endpoint_Quic>(end)->address() +
            ":" +
            std::to_string(std::dynamic_pointer_cast<gvirtus::communicators::Endpoint_Quic>(end)->port());
    printf("[TRACE] create_communicator: arg='%s'\n", arg.c_str());
    return std::make_shared<QuicCommunicator>(arg);
}
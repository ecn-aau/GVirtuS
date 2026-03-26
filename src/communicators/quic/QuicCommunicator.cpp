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
    
    DEBUG_PRINTF("called");

    const uint8_t *p = (const uint8_t *)buf;
    ssize_t rem = len;
    const long deadline = (timeout_ms >= 0) ? now_ms() + timeout_ms : -1;
    set_nonblocking(fd);
    while (rem > 0) {
        ssize_t chunk = rem > (size_t)SSIZE_MAX ? (size_t)SSIZE_MAX : rem;
        //StreamRecvMutex.lock();
        ssize_t n = write(fd, p, chunk);
        //StreamRecvMutex.unlock();
        if (n > 0) {
            DEBUG_PRINTF("bytes written \n");
            p += (size_t)n;
            rem -= (size_t)n;
            continue;
        }
        if (n == -1) {
            DEBUG_PRINTF("error %d \n", errno);
            if (errno == EINTR) continue;

            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                int wait_ms = -1;
                DEBUG_PRINTF("wait");
                if (deadline != -1) {
                    long left = deadline - now_ms();
                    if (left <= 0) { 
                            errno = ETIMEDOUT;
                            printf("ETIMEDOUT1\n");
                            //return -1; 
                        }
                    wait_ms = (left > INT_MAX) ? INT_MAX : (int)left;
                }

                struct pollfd pfd = { .fd = fd, .events = POLLOUT, .revents = 0 };
                int r;
                do { r = poll(&pfd, 1, wait_ms); } while (r < 0 && errno == EINTR);
                if (r == 0) { 
                    errno = ETIMEDOUT; 
                    DEBUG_PRINTF("ETIMEDOUT2");
                    //return -1; 
                    }
                if (r < 0) return -1;

                // If the read end was closed, writes will fail with EPIPE.
                if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                    errno = EPIPE;
                    DEBUG_PRINTF("EPIPE");
                    //return -1;
                }
                // Ready to try write() again.
                continue;
            }

            // Other hard errors (e.g., EPIPE if no reader)
            //return -1;
        }
    }
    return 0;
}


QuicCommunicator::QuicCommunicator(const QuicCommunicator& other)
{
    DEBUG_PRINTF("called with connection %p", QuicCommunicator::Connection);
    Stream = NULL;
    Listener = NULL;
}

QuicCommunicator::QuicCommunicator(const std::string &communicator) {
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

}

//
// Helper function to load a client configuration.
//
bool QuicCommunicator::ClientLoadConfiguration(
    BOOLEAN Unsecure
    )
{
    DEBUG_PRINTF("called");
    
    QUIC_SETTINGS Settings = {0};

    //
    // Configures the client's idle timeout.
    //
    Settings.IdleTimeoutMs = 0;
    Settings.IsSet.IdleTimeoutMs = TRUE;

    Settings.SendBufferingEnabled = FALSE;
    Settings.IsSet.SendBufferingEnabled = TRUE;

    Settings.MaximumMtu = 1200;
    Settings.IsSet.MaximumMtu = TRUE;

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
    }

    //
    // Allocate/initialize the configuration object, with the configured ALPN
    // and settings.
    //
    QUIC_STATUS Status = QUIC_STATUS_SUCCESS;
    if (QUIC_FAILED(Status = MsQuic->ConfigurationOpen(Registration, &Alpn, 1, &Settings, sizeof(Settings), NULL, &Configuration))) {
        printf("ConfigurationOpen failed, 0x%x!\n", Status);
        return FALSE;
    }

    //
    // Loads the TLS credential part of the configuration. This is required even
    // on client side, to indicate if a certificate is required or not.
    //
    if (QUIC_FAILED(Status = MsQuic->ConfigurationLoadCredential(Configuration, &CredConfig))) {
        printf("ConfigurationLoadCredential failed, 0x%x!\n", Status);
        return FALSE;
    }

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
        //
        // Load the server's certificate from the default certificate store,
        // using the provided certificate hash.
        //
        uint32_t CertHashLen =
            DecodeHexBuffer(
                Cert,
                sizeof(Config.CertHash.ShaHash),
                Config.CertHash.ShaHash);
        if (CertHashLen != sizeof(Config.CertHash.ShaHash)) {
            return FALSE;
        }
        Config.CredConfig.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_HASH;
        Config.CredConfig.CertificateHash = &Config.CertHash;

    } else if ((Cert = GetValue(argc, argv, "cert_file")) != NULL &&
               (KeyFile = GetValue(argc, argv, "key_file")) != NULL) {
        //
        // Loads the server's certificate from the file.
        //
        const char* Password = GetValue(argc, argv, "password");
        if (Password != NULL) {
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
        printf("Must specify ['-cert_hash'] or ['cert_file' and 'key_file' (and optionally 'password')]!\n");
        return FALSE;
    }

    //
    // Allocate/initialize the configuration object, with the configured ALPN
    // and settings.
    //
    QUIC_STATUS Status = QUIC_STATUS_SUCCESS;
    if (QUIC_FAILED(Status = MsQuic->ConfigurationOpen(Registration, &Alpn, 1, &Settings, sizeof(Settings), NULL, &Configuration))) {
        printf("ConfigurationOpen failed, 0x%x!\n", Status);
        return FALSE;
    }

    //
    // Loads the TLS credential part of the configuration.
    //
    if (QUIC_FAILED(Status = MsQuic->ConfigurationLoadCredential(Configuration, &Config.CredConfig))) {
        printf("ConfigurationLoadCredential failed, 0x%x!\n", Status);
        return FALSE;
    }

    return TRUE;
}

QuicCommunicator::QuicCommunicator(const char *hostname, short port) {
    pid_t tid = syscall(SYS_gettid);
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

    QUIC_STATUS Status = QUIC_STATUS_SUCCESS;

    //
    // Open a handle to the library and get the API function table.
    //
    // InitializeQuic();

}

QuicCommunicator::QuicCommunicator() {
    pid_t tid = syscall(SYS_gettid);
    DEBUG_PRINTF("called with connection %p", QuicCommunicator::Connection);
    Stream = NULL;
    Listener = NULL;
    listener_started=false;
}


void QuicCommunicator::InitializeQuic(void) {
    DEBUG_PRINTF("called");
    Stream = NULL;
    listener_started=false;

    QUIC_STATUS Status = QUIC_STATUS_SUCCESS;

    if (QUIC_FAILED(Status = MsQuicOpen2(&MsQuic))) {
                throw "MsQuicOpen2 failed.";
    }

    if (QUIC_FAILED(Status = MsQuic->RegistrationOpen(&RegConfig, &Registration))) {
        //printf("RegistrationOpen failed, 0x%x!\n", Status);
        throw "RegistrationOpen failed.";
    }

    //QuicCommunicator::Connection = NULL;
    Stream = NULL;
    Listener = NULL;
    //ToDo: close pipes
    //ToDo: handle exceptions and close connection
}

void gvirtus::communicators::QuicCommunicator::InitializePipes()
    {
        if (pipe(ReadPipeFds) == -1) {
            printf("Failed to create pipe\n");
            throw "Failed to create pipe";
        }
        int pipe_size = 4096 * 4096 * 4; // 1MB buffer or adjust as needed
        fcntl(ReadPipeFds[0], F_SETPIPE_SZ, pipe_size);
        fcntl(ReadPipeFds[1], F_SETPIPE_SZ, pipe_size);
    }


QuicCommunicator::~QuicCommunicator() {
    //    close(mSocketFd);
    //ToDo: only close the connection at the end of the programm
    DEBUG_PRINTF("called");
    delete[] mInAddr;
    close(ReadPipeFds[0]);
    close(ReadPipeFds[1]);
    
    if (MsQuic != NULL) {
        if (Configuration != NULL) {
            MsQuic->ConfigurationClose(Configuration);
        }
        if (Registration != NULL) {
            //
            // This will block until all outstanding child objects have been
            // closed.
            //
            MsQuic->RegistrationClose(Registration);
        }
        MsQuicClose(MsQuic);
    }

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
    DEBUG_PRINTF("[sid %lu] called", sid);
    int wp = -1;
    if (QuicCommunicator::pipemap.find(sid) != QuicCommunicator::pipemap.end()) {
        wp = QuicCommunicator::pipemap[sid];
        DEBUG_PRINTF("[sid %lu] Get pipe %d %p %d\n",sid, wp,Stream,Event->Type);
    }
    else {
        DEBUG_PRINTF("[sid %lu] Pipe not found pipe %d %p %d\n",sid, wp,Stream,Event->Type);
        return QUIC_STATUS_SUCCESS;
    }
    switch (Event->Type) {
    case QUIC_STREAM_EVENT_SEND_COMPLETE:
        //
        // A previous StreamSend call has completed, and the context is being
        // returned back to the app.
        //
       qb = (QUIC_BUFFER*)Event->SEND_COMPLETE.ClientContext;

        
        DEBUG_PRINTF("[strm][%p] Data sent %d\n", Stream, qb->Length);
        free(Event->SEND_COMPLETE.ClientContext);
        break;
    case QUIC_STREAM_EVENT_RECEIVE:
        //
        // Data was received from the peer on the stream.
        //
        // Write received data to the pipe


        for (uint32_t i = 0; i < Event->RECEIVE.BufferCount; ++i) {
            const QUIC_BUFFER* b = &Event->RECEIVE.Buffers[i];
            DEBUG_PRINTF("[sid %lu] [strm %p] [pipe %d] Data received %u, flags %d\n", sid, Stream, wp, b->Length, Event->RECEIVE.Flags);
            /*if (write_all_nonblocking(wp, b->Buffer, b->Length, 10000) == -1) {
                printf("Failed to write to pipe\n");
                throw "Failed to write to pipe";
            }*/
            if (write(wp, b->Buffer, b->Length) == -1) {
                printf("Failed to write to pipe\n");
                throw "Failed to write to pipe";
            }
            DEBUG_PRINTF("[sid %lu] [strm %p] [pipe %d] Data written %u, flags %d\n", sid, Stream, wp, b->Length, Event->RECEIVE.Flags);
        }
        break;
    case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN:
        //
        // The peer gracefully shut down its send direction of the stream.
        //
        DEBUG_PRINTF("[strm][%p] Peer shut down\n", Stream);
        //ServerSend(Stream);
        break;
    case QUIC_STREAM_EVENT_PEER_SEND_ABORTED:
        //
        // The peer aborted its send direction of the stream.
        //
        DEBUG_PRINTF("[strm][%p] Peer aborted\n", Stream);
        MsQuic->StreamShutdown(Stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
        break;
    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
        //
        // Both directions of the stream have been shut down and MsQuic is done
        // with the stream. It can now be safely cleaned up.
        //
        DEBUG_PRINTF("[strm][%p] All done\n", Stream);
        QuicCommunicator::pipemap.erase(sid);
        MsQuic->StreamClose(Stream);
        break;
    default:
        break;
    }
    return QUIC_STATUS_SUCCESS;
}

unsigned int QuicCommunicator::ClientStreamCallbackWrapper(HQUIC Stream, void* Context, QUIC_STREAM_EVENT* Event) {
        // Cast the context back to the class instance and call the member function
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
    //std::unique_lock<std::mutex> slock(StreamMutex);
    int wp = -1;
    if (QuicCommunicator::pipemap.find(sid) != QuicCommunicator::pipemap.end()) {
        wp = QuicCommunicator::pipemap[sid];
        DEBUG_PRINTF("Get pipe %lu %d %p %d\n",sid, wp,Stream,Event->Type);
    }
    else {
        DEBUG_PRINTF("Pipe not found pipe %lu %d %p %d\n",sid, wp,Stream,Event->Type);
        //Todo: stream should be deleted
        return QUIC_STATUS_SUCCESS;
    }

    QUIC_BUFFER* qb=NULL;

    switch (Event->Type) {
    case QUIC_STREAM_EVENT_SEND_COMPLETE:
        //
        // A previous StreamSend call has completed, and the context is being
        // returned back to the app.
        //
        
        qb = (QUIC_BUFFER*)Event->SEND_COMPLETE.ClientContext;

        DEBUG_PRINTF("[strm][%p] Data sent %d\n", Stream, qb->Length) ;
        free(Event->SEND_COMPLETE.ClientContext);
        break;
    case QUIC_STREAM_EVENT_RECEIVE:
        //
        // Data was received from the peer on the stream.
        //
        // Write received data to the pipe
        for (uint32_t i = 0; i < Event->RECEIVE.BufferCount; ++i) {
            const QUIC_BUFFER* b = &Event->RECEIVE.Buffers[i];
            DEBUG_PRINTF("[strm][%p][sid %lu][pipe %d] Data received %u, flags %d\n", Stream, sid, wp, b->Length, Event->RECEIVE.Flags);
            /*if (write_all_nonblocking(wp, b->Buffer, b->Length, 100000) == -1) {
                printf("Failed to write to pipe\n");
                throw "Failed to write to pipe";
            }*/
            if (write(wp, b->Buffer, b->Length) == -1) {
                printf("Failed to write to pipe\n");
                throw "Failed to write to pipe";
            }
        }

            //Todo: Improve QUIC API use async. 

        DEBUG_PRINTF("[strm][%p] Data received\n", Stream);


        break;
    case QUIC_STREAM_EVENT_PEER_SEND_ABORTED:
        //
        // The peer gracefully shut down its send direction of the stream.
        //
        DEBUG_PRINTF("[strm][%p] Peer aborted\n", Stream);
        break;
    case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN:
        //
        // The peer aborted its send direction of the stream.
        //
        DEBUG_PRINTF("[strm][%p] Peer shut down\n", Stream);
        break;
    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
        //
        // Both directions of the stream have been shut down and MsQuic is done
        // with the stream. It can now be safely cleaned up.
        //
        DEBUG_PRINTF("[strm][%p] All done\n", Stream);
        if (!Event->SHUTDOWN_COMPLETE.AppCloseInProgress) {
            MsQuic->StreamClose(Stream);
        }
        break;
    default:
        break;
    }
    //StreamEventOccurred = true;
    //StreamStartCv.notify_one();
    return QUIC_STATUS_SUCCESS;
}


unsigned int QuicCommunicator::ClientConnectionCallbackWrapper(HQUIC Stream, void* Context, QUIC_CONNECTION_EVENT* Event) {
        // Cast the context back to the class instance and call the member function
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
    std::unique_lock<std::mutex> lock(ConnectMutex);
    switch (Event->Type) {
    case QUIC_CONNECTION_EVENT_CONNECTED:
        //
        // The handshake has completed for the connection.
        //
        DEBUG_PRINTF("[conn][%p] Connected\n", Connection);
        QuicCommunicator::Connection=Connection;
        //ClientSend(Connection);
        break;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
        //
        // The connection has been shut down by the transport. Generally, this
        // is the expected way for the connection to shut down with this
        // protocol, since we let idle timeout kill the connection.
        //
        if (Event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status == QUIC_STATUS_CONNECTION_IDLE) {
            printf("[conn][%p] Successfully shut down on idle.\n", Connection);
        } else {
            printf("[conn][%p] Shut down by transport, 0x%x\n", Connection, Event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status);
        }
        break;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
        //
        // The connection was explicitly shut down by the peer.
        //
        printf("[conn][%p] Shut down by peer, 0x%llu\n", Connection, (unsigned long long)Event->SHUTDOWN_INITIATED_BY_PEER.ErrorCode);
        break;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
        //
        // The connection has completed the shutdown process and is ready to be
        // safely cleaned up.
        //
        printf("[conn][%p] All done\n", Connection);
        if (!Event->SHUTDOWN_COMPLETE.AppCloseInProgress) {
            MsQuic->ConnectionClose(Connection);
        }
        break;
    case QUIC_CONNECTION_EVENT_RESUMPTION_TICKET_RECEIVED:
        //
        // A resumption ticket (also called New Session Ticket or NST) was
        // received from the server.
        //
        printf("[conn][%p] Resumption ticket received (%u bytes):\n", Connection, Event->RESUMPTION_TICKET_RECEIVED.ResumptionTicketLength);
        for (uint32_t i = 0; i < Event->RESUMPTION_TICKET_RECEIVED.ResumptionTicketLength; i++) {
            printf("%.2X", (uint8_t)Event->RESUMPTION_TICKET_RECEIVED.ResumptionTicket[i]);
        }
        printf("\n");
        break;
    default:
        break;
    }
    ConnectEventOccurred = true;
    ConnectionStartCv.notify_one();
    return QUIC_STATUS_SUCCESS;
}


unsigned int QuicCommunicator::ServerStreamCallbackWrapper(HQUIC Stream, void* Context, QUIC_STREAM_EVENT* Event) {
        // Cast the context back to the class instance and call the member function
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

    DEBUG_PRINTF("[conn %p] [msquic %p] %s event type %d\n", Connection, MsQuic, __PRETTY_FUNCTION__, Event->Type);
    UNREFERENCED_PARAMETER(Context);
    uint32_t sidSize = sizeof(sid);

    switch (Event->Type) {
    case QUIC_CONNECTION_EVENT_CONNECTED:
        //
        // The handshake has completed for the connection.
        //
        printf("[conn][%p] Connected\n", Connection);
        MsQuic->ConnectionSendResumptionTicket(Connection, QUIC_SEND_RESUMPTION_FLAG_NONE, 0, NULL);
        break;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
        //
        // The connection has been shut down by the transport. Generally, this
        // is the expected way for the connection to shut down with this
        // protocol, since we let idle timeout kill the connection.
        //
        if (Event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status == QUIC_STATUS_CONNECTION_IDLE) {
            printf("[conn][%p] Successfully shut down on idle.\n", Connection);
        } else {
            printf("[conn][%p] Shut down by transport, 0x%x\n", Connection, Event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status);
        }
        break;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
        //
        // The connection was explicitly shut down by the peer.
        //
        printf("[conn][%p] Shut down by peer, 0x%llu\n", Connection, (unsigned long long)Event->SHUTDOWN_INITIATED_BY_PEER.ErrorCode);
        break;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
        //
        // The connection has completed the shutdown process and is ready to be
        // safely cleaned up.
        //
        printf("[conn][%p] All done\n", Connection);
        MsQuic->ConnectionClose(Connection);
        break;
    case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED:
        //
        // The peer has started/created a new stream. The app MUST set the
        // callback handler before returning.
        //
        {
            QUIC_UINT62 tmpsid;
            MsQuic->GetParam(Event->PEER_STREAM_STARTED.Stream, QUIC_PARAM_STREAM_ID, &sidSize, &tmpsid);
            DEBUG_PRINTF("[strm][%p] Peer started witd id %lu\n", Event->PEER_STREAM_STARTED.Stream, tmpsid);

            //ToDo Delete QuicCommunicator
            QuicCommunicator * NewStreamQuicCommunicator = new QuicCommunicator(*this);
            NewStreamQuicCommunicator->InitializePipes();

            DEBUG_PRINTF("[strm][%p] Peer started witd id %lu\n", Event->PEER_STREAM_STARTED.Stream, tmpsid);
            QuicCommunicator::pipemap.insert(std::pair(tmpsid, NewStreamQuicCommunicator->ReadPipeFds[1]));
            DEBUG_PRINTF("[strm %p] pipes inserted %d %d \n", Event->PEER_STREAM_STARTED.Stream, NewStreamQuicCommunicator->ReadPipeFds[0], NewStreamQuicCommunicator->ReadPipeFds[1]);

            NewStreamQuicCommunicator->Stream = Event->PEER_STREAM_STARTED.Stream;
            NewStreamQuicCommunicator->Connection = Connection;
            NewStreamQuicCommunicator->sid = tmpsid;
            //set     HQUIC Registration;  HQUIC Configuration; HQUIC Listener;
            MsQuic->SetCallbackHandler(Event->PEER_STREAM_STARTED.Stream, (void *) ServerStreamCallbackWrapper, NewStreamQuicCommunicator);

            NewQuicCommunicatorQueue.push(NewStreamQuicCommunicator);
            StreamEventOccurred = true;
            StreamStartCv.notify_one();
            break;
        }

    case QUIC_CONNECTION_EVENT_RESUMED:
        //
        // The connection succeeded in doing a TLS resumption of a previous
        // connection's session.
        //
        printf("[conn][%p] Connection resumed!\n", Connection);
        break;
    default:
        printf("Unkown Connection event %i", Event->Type);
        break;
    }
    return QUIC_STATUS_SUCCESS;
}


unsigned int QuicCommunicator::ServerConnectionCallbackWrapper(HQUIC Connection, void* Context, QUIC_CONNECTION_EVENT* Event) {
        // Cast the context back to the class instance and call the member function
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
    DEBUG_PRINTF("called");
    int stream_count = 65535;
    std::unique_lock<std::mutex> lock(ListenerStartMutex);
    UNREFERENCED_PARAMETER(Listener);
    int c = 1024;
    //UNREFERENCED_PARAMETER(Context);
    QUIC_STATUS Status = QUIC_STATUS_NOT_SUPPORTED;
    switch (Event->Type) {
    case QUIC_LISTENER_EVENT_NEW_CONNECTION:
        //
        // A new connection is being attempted by a client. For the handshake to
        // proceed, the server must provide a configuration for QUIC to use. The
        // app MUST set the callback handler before returning.
        //
        printf("QUIC_LISTENER_EVENT_NEW_CONNECTION\n");
        MsQuic->SetCallbackHandler(Event->NEW_CONNECTION.Connection, (void*)ServerConnectionCallbackWrapper, Context);
        Status = MsQuic->ConnectionSetConfiguration(Event->NEW_CONNECTION.Connection, Configuration);
        MsQuic->SetParam(Connection, QUIC_PARAM_CONN_LOCAL_BIDI_STREAM_COUNT, sizeof(stream_count), &stream_count);
        MsQuic->SetParam(Connection, QUIC_PARAM_CONN_LOCAL_UNIDI_STREAM_COUNT, sizeof(stream_count), &stream_count);

        NewConnectionEventOccurred = true;
        ListenerStartCv.notify_one();
        break;
    default:
        break;
    }
    
    return Status;
}

unsigned int QuicCommunicator::ServerListenerCallbackWrapper(HQUIC Listener, void* Context, QUIC_LISTENER_EVENT* Event) {
        // Cast the context back to the class instance and call the member function
        DEBUG_PRINTF("called");
        auto communicator = static_cast<QuicCommunicator*>(Context);
        return communicator->ServerListenerCallback(Listener, Context, Event);
    }


void QuicCommunicator::Serve() {
#ifdef DEBUG
    printf("QuicCommunicator::Serve() called\n");
#endif

    QUIC_STATUS Status;

    InitializeQuic(); 
    
    int argc = 2;
    const char * argv[2];
    
    argv[0] = (char *)"-cert_file:/gvirtus/etc/server.cert";
    argv[1] = (char *)"-key_file:/gvirtus/etc/server.key";

    printf("Load Server Config\n");
    if (!ServerLoadConfiguration(argc, argv)) {
        return;
    }

    //
    // Create/allocate a new listener object.
    //
    if (QUIC_FAILED(Status = MsQuic->ListenerOpen(Registration, ServerListenerCallbackWrapper, this, &Listener))) {
        //printf("ListenerOpen failed, 0x%x!\n", Status);
        throw "ListenerOpen failed";
    }
    sleep(1);
    //
#ifdef DEBUG
    printf("QuicCommunicator::Serve() returned\n");
#endif
}

const gvirtus::communicators::Communicator *const QuicCommunicator::Accept() const {
    printf("QuicCommunicator::Accept() called\n");
    //std::unique_lock<std::mutex> lock(ListenerStartMutex);
    QUIC_STATUS Status;
        //
    // Configures the address used for the listener to listen on all IP
    // addresses and the given UDP port.
    //
    QUIC_ADDR Address = {0};
    QuicAddrSetFamily(&Address, QUIC_ADDRESS_FAMILY_UNSPEC);
    QuicAddrSetPort(&Address, htons(mPort));

    if (listener_started==false){
        std::unique_lock<std::mutex> lock(ListenerStartMutex);
        printf("MsQuic->ListenerStart called %p\n", Listener);
        if (QUIC_FAILED(Status = MsQuic->ListenerStart(Listener, &Alpn, 1, &Address))) {
            throw "ListenerStart failed";
        }
        listener_started=true;

        //ToDo: How to differentiate if it is a new stream but old connection or a new connection?
        printf("MsQuic->ListenerStart wait\n");
        ListenerStartCv.wait(lock, [this] { return NewConnectionEventOccurred; });
        NewConnectionEventOccurred = false;
        printf("QuicCommunicator::Accept() returned\n");

    }
    std::unique_lock<std::mutex> slock(StreamMutex);
    DEBUG_PRINTF("Wait for Stream\n");
    StreamStartCv.wait(slock, [this] { return (!NewQuicCommunicatorQueue.empty() || StreamEventOccurred); });
    DEBUG_PRINTF("New Stream\n");
    QuicCommunicator * NewStreamQuicCommunicator = NULL;
    if (!NewQuicCommunicatorQueue.empty()) {
        StreamEventOccurred = false;
        NewStreamQuicCommunicator = NewQuicCommunicatorQueue.front();
        NewQuicCommunicatorQueue.pop();
        DEBUG_PRINTF("QuicCommunicator::Streamstart() returned\n");
    } else {
        DEBUG_PRINTF("No QuicCommunicator in queue\n");
    }
    return NewStreamQuicCommunicator; //new 
}

void QuicCommunicator::Connect() {
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
        printf("Connection is null\n");
        std::unique_lock<std::mutex> lock(ConnectMutex);

        InitializeQuic(); 

        if (!ClientLoadConfiguration(GetFlag(argc, argv, "unsecure"))) {
            return;
        }
        //
        // Allocate a new connection object.
        //
        

        if (QUIC_FAILED(Status = MsQuic->ConnectionOpen(Registration, ClientConnectionCallbackWrapper, this, &Connection))) {
            printf("ConnectionOpen failed, 0x%x!\n", Status);
            throw "ConnectionOpen failed";
            //goto Error;
        }

        /*
        if ((ResumptionTicketString = GetValue(argc, argv, "ticket")) != NULL) {
            //
            // If provided at the command line, set the resumption ticket that can
            // be used to resume a previous session.
            //
            uint8_t ResumptionTicket[10240];
            uint16_t TicketLength = (uint16_t)DecodeHexBuffer(ResumptionTicketString, sizeof(ResumptionTicket), ResumptionTicket);
            if (QUIC_FAILED(Status = MsQuic->SetParam(QuicCommunicator::Connection, QUIC_PARAM_CONN_RESUMPTION_TICKET, TicketLength, ResumptionTicket))) {
                printf("SetParam(QUIC_PARAM_CONN_RESUMPTION_TICKET) failed, 0x%x!\n", Status);
                throw "SetParam(QUIC_PARAM_CONN_RESUMPTION_TICKET) failed";
            }
        }
            */

        MsQuic->SetParam(Connection, QUIC_PARAM_CONN_LOCAL_BIDI_STREAM_COUNT, sizeof(stream_count), &stream_count);
        MsQuic->SetParam(Connection, QUIC_PARAM_CONN_LOCAL_UNIDI_STREAM_COUNT, sizeof(stream_count), &stream_count);


        printf("[conn][%p] Connecting...\n", QuicCommunicator::Connection);

        //
        // Start the connection to the server.
        //
        if (QUIC_FAILED(Status = MsQuic->ConnectionStart(QuicCommunicator::Connection, Configuration, QUIC_ADDRESS_FAMILY_UNSPEC, mHostname.data(), htons(mPort)))) {
            //printf("ConnectionStart failed, 0x%x!\n", Status);
            //goto Error;
            throw "ConnectionStart failed";
        }

        ConnectionStartCv.wait(lock, [this] {return ConnectEventOccurred; });
    } 
    else {
        printf("Connection is open\n");
    }

    /*
    int c = 1024;
    MsQuic->SetParam(Connection, QUIC_PARAM_CONN_LOCAL_BIDI_STREAM_COUNT, sizeof(c), &c);
    MsQuic->SetParam(Connection, QUIC_PARAM_CONN_LOCAL_UNIDI_STREAM_COUNT, sizeof(c), &c);

    printf("[conn][%p] Connecting...\n", Connection);

    //
    // Start the connection to the server.
    //
    if (QUIC_FAILED(Status = MsQuic->ConnectionStart(Connection, Configuration, QUIC_ADDRESS_FAMILY_UNSPEC, mHostname.data(), htons(mPort)))) {
        //printf("ConnectionStart failed, 0x%x!\n", Status);
        //goto Error;
        throw "ConnectionStart failed";
    }

    //ConnectionStartCv.wait(lock, [this] {return ConnectEventOccurred; });
    */
    

    //
    // Create/allocate a new bidirectional stream. The stream is just allocated
    // and no QUIC stream identifier is assigned until it's started.
    //

    DEBUG_PRINTF("[conn %p]  [msquic %p] %s open stream \n", Connection, MsQuic, __PRETTY_FUNCTION__);

    //std::unique_lock<std::mutex> slock(StreamMutex);
    if (QUIC_FAILED(Status = MsQuic->StreamOpen(Connection, QUIC_STREAM_OPEN_FLAG_NONE, ClientStreamCallbackWrapper, this, &Stream))) {
        printf("StreamOpen failed, 0x%x!\n", Status);
        throw "StreamOpen failed";
        //goto Error;
    }

    //
    // Starts the bidirectional stream. By default, the peer is not notified of
    // the stream being started until data is sent on the stream.
    //
    
    DEBUG_PRINTF("[sid %lu] [conn %p] %s start stream \n", sid, Connection, __PRETTY_FUNCTION__);

    
    if (QUIC_FAILED(Status = MsQuic->StreamStart(Stream, QUIC_STREAM_START_FLAG_NONE))) {
        printf("StreamStart failed, 0x%x!\n", Status);
        MsQuic->StreamClose(Stream);
        throw "StreamStart failed";
        //goto Error;
    }

    uint32_t sidSize = sizeof(sid);
    MsQuic->GetParam(Stream, QUIC_PARAM_STREAM_ID, &sidSize, &sid);

    if (pipe(ReadPipeFds) == -1) {
        printf("Failed to create pipe\n");
        throw "Failed to create pipe";
    }

    int pipe_size = 4096 * 4096 * 4; // 1MB buffer or adjust as needed
    fcntl(ReadPipeFds[0], F_SETPIPE_SZ, pipe_size);
    fcntl(ReadPipeFds[1], F_SETPIPE_SZ, pipe_size);

    DEBUG_PRINTF("Pipe created %lu %d %d %p\n", sid, ReadPipeFds[0], ReadPipeFds[1],Stream);
    DEBUG_PRINTF("Insert pipe %lu %d %p\n", sid, ReadPipeFds[1],Stream);

    QuicCommunicator::pipemap.insert(std::pair(sid, ReadPipeFds[1]));

    DEBUG_PRINTF("[strm][%p][%u][%lu] Starting...\n", Stream, tid, sid);
    
    //InitializeStream();

#ifdef DEBUG
    printf("QuicCommunicator::Connect() returned\n");
#endif
}

void QuicCommunicator::Close() {
    printf("QuicCommunicator::Close\n");
    if (Stream!=NULL)
        MsQuic->StreamClose(Stream);
    if (Connection != NULL)
        MsQuic->ConnectionClose(Connection);


}

size_t QuicCommunicator::Read(char *buffer, size_t size) {

//#ifdef DEBUG

    DEBUG_PRINTF("[sid %lu] %s called with size %lu to pipe %d\n", sid, __PRETTY_FUNCTION__, size, ReadPipeFds[0]);
//#endif
    ssize_t ret_value=0;
    ssize_t size_left=size;

    //for (unsigned int i = 0; i < size; i++) printf("%d LETTO %02X\n", i, buffer[i]);
    while(size_left>0) {
        //StreamRecvMutex.lock();
        DEBUG_PRINTF("[sid %lu] QuicCommunicator::Read() Block on read() %ld %lu %lu\n", sid, ret_value,size,size_left);
        //int flags = fcntl(ReadPipeFds[0], F_GETFL);
        //fcntl(ReadPipeFds[0], F_SETFL, flags | O_NONBLOCK);
        ssize_t r = read(ReadPipeFds[0], buffer+ret_value, size_left);
        //StreamRecvMutex.unlock();
        if (r < 0) {
            DEBUG_PRINTF("errno %d\n",errno);
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                //usleep(10);
                continue;
            }
            continue;
            //perror("read error");
            //return -1;
        }
        //fcntl(ReadPipeFds[0], F_SETFL, flags);
        
        if (r < 0 || r==0){
            ret_value = 0;
            //break;
            continue;
        }
        else {
            ret_value += r;
            size_left=size_left-r;
            if (size_left == 0)
                break;
        }
        DEBUG_PRINTF("[sid %lu] Read return value: %ld %ld %lu %lu\n",sid ,r,ret_value,size,size_left);
    }


//#ifdef DEBUG
    DEBUG_PRINTF("returned %zu\n", ret_value);
//#endif

    return ret_value;
}

size_t QuicCommunicator::Write(const char *buffer, size_t size) {
#ifdef DEBUG
    printf("QuicCommunicator::Write() called\n");
#endif

    //std::unique_lock<std::mutex> lock(StreamMutex);
    DEBUG_PRINTF("[sid %lu] %s called with size %lu", sid, __PRETTY_FUNCTION__, size);

    QUIC_STATUS Status;

    //
    // Allocates and builds the buffer to send over the stream.
    //
    size_t MAX_BUF_SIZE = 4096*4096*4;

    size_t size_left = size;
    size_t send_size = 0;
    size_t send_size_cum = 0;

    DEBUG_PRINTF("[strm][%p] Sending data total... %ld", Stream, size);

    while (size_left>0)
    {
        uint8_t* SendBufferRaw;
        QUIC_BUFFER* SendBuffer;
        
        if (size_left > MAX_BUF_SIZE)
            send_size = MAX_BUF_SIZE;
        else
            send_size = size_left;
        
        SendBufferRaw = (uint8_t*)malloc(sizeof(QUIC_BUFFER) + send_size);
        if (SendBufferRaw == NULL) {
            printf("SendBuffer allocation failed!\n");
            Status = QUIC_STATUS_OUT_OF_MEMORY;
            goto Error;
        }
        memcpy(SendBufferRaw+sizeof(QUIC_BUFFER), buffer+send_size_cum, send_size);
        SendBuffer = (QUIC_BUFFER*)SendBufferRaw;
        SendBuffer->Buffer = SendBufferRaw + sizeof(QUIC_BUFFER);
        SendBuffer->Length = send_size;

        DEBUG_PRINTF("[strm %p] Sending data... %ld %ld\n", Stream, send_size, sizeof(QUIC_BUFFER));
        send_size_cum += send_size;
        size_left -= send_size;


        //s
        // Sends the buffer over the stream. Note the FIN flag is passed along with
        // the buffer. This indicates this is the last buffer on the stream and the
        // the stream is shut down (in the send direction) immediately after.
        //
        if (QUIC_FAILED(Status = MsQuic->StreamSend(Stream, SendBuffer, 1, QUIC_SEND_FLAG_NONE, SendBuffer))) {
            printf("StreamSend failed, 0x%x!\n", Status);
            free(SendBufferRaw);
            goto Error;
        }
    }
    
Error:
    /*
    if (QUIC_FAILED(Status)) {
        MsQuic->ConnectionShutdown(Connection, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
    }*/

    //mpOutput->write(buffer, size);

#ifdef DEBUG
    for (unsigned int i = 0; i < size; i++) printf("%d SCRITTO %02X \n", i, buffer[i]);
#endif

#ifdef DEBUG
    printf("QuicCommunicator::Read() returned %zu\n", size);
#endif

    return size;
}

void QuicCommunicator::Sync() {
    //mpOutput->flush();
}

/*
void QuicCommunicator::InitializeStream() {
#ifdef _WIN32
    FILE *i = _fdopen(mSocketFd, "r");
    FILE *o = _fdopen(mSocketFd, "w");
    mpInputBuf = new filebuf(i);
    mpOutputBuf = new filebuf(o);
#else
    mpInputBuf = new __gnu_cxx::stdio_filebuf<char>(mSocketFd, ios_base::in);
    mpOutputBuf = new __gnu_cxx::stdio_filebuf<char>(mSocketFd, ios_base::out);
#endif

    mpInput = new istream(mpInputBuf);
    mpOutput = new ostream(mpOutputBuf);
}*/

extern "C" std::shared_ptr <QuicCommunicator> create_communicator(
        std::shared_ptr <gvirtus::communicators::Endpoint> end) {
    std::string arg =
            "quic://" +
            std::dynamic_pointer_cast<gvirtus::communicators::Endpoint_Quic>(end)->address() +
            ":" +
            std::to_string(std::dynamic_pointer_cast<gvirtus::communicators::Endpoint_Quic>(end)->port());
    return std::make_shared<QuicCommunicator>(arg);
}

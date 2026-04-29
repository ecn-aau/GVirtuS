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
 * @file   QuicCommunicator.h
 * @author Giuseppe Coviello <giuseppe.coviello@uniparthenope.it>
 * @date   Thu Oct 8 12:08:33 2009
 *
 * @brief
 *
 *
 */

#pragma once

#ifdef _WIN32
#include <fstream>
#else
#include <ext/stdio_filebuf.h>
#endif

#include "gvirtus/communicators/Communicator.h"
#include "msquic.h"

#include <thread>
#include <mutex>
#include <map>
#include <condition_variable>
#include <queue>

const uint32_t SendBufferLength = 4*1024*1024;

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

//
// Helper function to convert a string of hex characters to a byte buffer.
//
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

namespace gvirtus::communicators {
/**
 * QuicCommunicator implements a Communicator for the TCP/IP socket.
 */
class QuicCommunicator : public Communicator {
 public:
    QuicCommunicator();
    QuicCommunicator(const std::string &communicator);
    QuicCommunicator(const char *hostname, short port);
    QuicCommunicator(int fd, const char *hostname);
    QuicCommunicator(const QuicCommunicator &);
    void InitializeQuic();
    void InitializePipes();
    const QUIC_REGISTRATION_CONFIG RegConfig = { "quicsample", QUIC_EXECUTION_PROFILE_LOW_LATENCY };
    bool ServerLoadConfiguration( _In_ int argc, _In_reads_(argc) _Null_terminated_ const char* argv[] );
    bool ClientLoadConfiguration( BOOLEAN Unsecure );  

    mutable std::queue<QuicCommunicator *> NewQuicCommunicatorQueue;

    _IRQL_requires_max_(PASSIVE_LEVEL)
      _Function_class_(QUIC_LISTENER_CALLBACK)
      QUIC_STATUS
      QUIC_API
      ServerListenerCallback(
        _In_ HQUIC Listener,
        _In_opt_ void* Context,
        _Inout_ QUIC_LISTENER_EVENT* Event
    );

    static unsigned int ServerStreamCallbackWrapper(HQUIC Stream, void* Context, QUIC_STREAM_EVENT* Event);
    static unsigned int ServerListenerCallbackWrapper(HQUIC Stream, void* Context, QUIC_LISTENER_EVENT* Event);
    static unsigned int ClientConnectionCallbackWrapper(HQUIC Stream, void* Context, QUIC_CONNECTION_EVENT* Event);
    static unsigned int ClientStreamCallbackWrapper(HQUIC Stream, void* Context, QUIC_STREAM_EVENT* Event);
    static unsigned int ServerConnectionCallbackWrapper(HQUIC Connection, void* Context, QUIC_CONNECTION_EVENT* Event);


    _IRQL_requires_max_(DISPATCH_LEVEL)
      _Function_class_(QUIC_STREAM_CALLBACK)
      QUIC_STATUS
      QUIC_API
      ClientStreamCallback(
        _In_ HQUIC Stream,
        _In_opt_ void* Context,
        _Inout_ QUIC_STREAM_EVENT* Event
    );

    void ClientSend( _In_ HQUIC Connection );

    _IRQL_requires_max_(DISPATCH_LEVEL)
      _Function_class_(QUIC_CONNECTION_CALLBACK)
      QUIC_STATUS
      QUIC_API
      ClientConnectionCallback( _In_ HQUIC Connection,
        _In_opt_ void* Context,
        _Inout_ QUIC_CONNECTION_EVENT* Event
    );

    _IRQL_requires_max_(DISPATCH_LEVEL)
      _Function_class_(QUIC_CONNECTION_CALLBACK)
      QUIC_STATUS
      QUIC_API
      ServerConnectionCallback( _In_ HQUIC Connection,
        _In_opt_ void* Context,
        _Inout_ QUIC_CONNECTION_EVENT* Event
    );


    _IRQL_requires_max_(DISPATCH_LEVEL)
      _Function_class_(QUIC_STREAM_CALLBACK)
      QUIC_STATUS
      QUIC_API
      ServerStreamCallback(
        _In_ HQUIC Stream,
        _In_opt_ void* Context,
        _Inout_ QUIC_STREAM_EVENT* Event
    );

    void ServerSend(_In_ HQUIC Stream);

    virtual ~QuicCommunicator();
    void Serve();
    const Communicator *const Accept() const;
    void Connect();
    size_t Read(char *buffer, size_t size);
    size_t Write(const char *buffer, size_t size);
    void Sync();
    void Close();

    

    std::string to_string() override { return "quiccommunicator"; }
 private:

     int write_all_nonblocking(int fd, const void *buf, ssize_t len, int timeout_ms);

    QUIC_UINT62 sid = 0;
    int ReadPipeFds[2];
    mutable std::mutex ListenerStartMutex;
    mutable bool NewConnectionEventOccurred { false };
    mutable std::condition_variable ListenerStartCv;

    std::mutex ConnectMutex;
    bool ConnectEventOccurred { false };
    std::condition_variable ConnectionStartCv;

    mutable std::mutex StreamMutex;
    mutable std::mutex StreamRecvMutex;
    mutable bool StreamEventOccurred { false };
    //mutable std::atomic<bool> StreamEventOccurred(false);
    mutable std::condition_variable StreamStartCv;

    bool LoadQuicSettingsFromJson(QUIC_SETTINGS& Settings);


    HQUIC Connection = nullptr;
    static inline const QUIC_API_TABLE* MsQuic = nullptr;
    HQUIC Registration;
    HQUIC Configuration;
    HQUIC Listener;
    static std::map<QUIC_UINT62, int> pipemap;
    HQUIC Stream;
    mutable bool listener_started;
  //void InitializeStream();
  //std::istream *mpInput;
  //std::ostream *mpOutput;
    std::string mHostname;
    
    char *mInAddr;
    int mInAddrSize;
    short mPort;
/*
  int mSocketFd;

#ifdef _WIN32
  std::filebuf *mpInputBuf;
  std::filebuf *mpOutputBuf;
#else
  __gnu_cxx::stdio_filebuf<char> *mpInputBuf;
  __gnu_cxx::stdio_filebuf<char> *mpOutputBuf;
#endif
*/
};
}  // namespace gvirtus::communicators

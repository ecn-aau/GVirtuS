#pragma once

#include "gvirtus/communicators/Communicator.h"
#include "msquic.h"

#include <thread>
#include <mutex>
#include <map>
#include <condition_variable>
#include <queue>

namespace gvirtus::communicators {

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


const QUIC_BUFFER Alpn = { sizeof("gvirtus") - 1, (uint8_t*)"gvirtus" };

struct Pipes {
    int in;
    int out;
};

class QuicCommunicator : public gvirtus::communicators::Communicator {
public:
    QuicCommunicator(const QuicCommunicator &);
    QuicCommunicator(const std::string &communicator);

    virtual ~QuicCommunicator();
    void Serve();
    const Communicator *const Accept() const;
    void Connect();
    size_t Read(char *buffer, size_t size);
    size_t Write(const char *buffer, size_t size);
    void Sync();
    void Close();

    // Callbacks - Must be public due to Wrapper calling object callbacks
    QUIC_STATUS ServerListenerCallback(HQUIC Listener, void* Context, QUIC_LISTENER_EVENT* Event);
    static QUIC_STATUS ServerListenerCallbackWrapper(HQUIC Listener, void* Context, QUIC_LISTENER_EVENT* Event);
    QUIC_STATUS ServerConnectionCallback(HQUIC Connection, void* Context, QUIC_CONNECTION_EVENT* Event);
    static QUIC_STATUS ServerConnectionCallbackWrapper(HQUIC Connection, void* Context, QUIC_CONNECTION_EVENT* Event);
    QUIC_STATUS ServerStreamCallback(HQUIC Stream, void* Context, QUIC_STREAM_EVENT* Event);
    static QUIC_STATUS ServerStreamCallbackWrapper(HQUIC Stream, void* Context, QUIC_STREAM_EVENT* Event);
    
    QUIC_STATUS ClientConnectionCallback(HQUIC Connection, void* Context, QUIC_CONNECTION_EVENT* Event);
    static QUIC_STATUS ClientConnectionCallbackWrapper(HQUIC Connection, void* Context, QUIC_CONNECTION_EVENT* Event);
    QUIC_STATUS ClientStreamCallback(HQUIC Stream, void* Context, QUIC_STREAM_EVENT* Event);
    static QUIC_STATUS ClientStreamCallbackWrapper(HQUIC Stream, void* Context, QUIC_STREAM_EVENT* Event);
protected:


private:
    void InitializeQuic();
    Pipes InitializePipes();
    bool ServerLoadConfiguration(int argc, const char* argv[] );
    bool ClientLoadConfiguration(bool unsecure);



    // !! Mutables are needed because Accept is a const function. (Should find a better solution)
    // Maybe do most changes in other functions?

    // Listener objects
    HQUIC Listener;
    mutable HQUIC receivedConnection;
    mutable std::condition_variable listernerCv;
    mutable std::mutex listenerMutex;
    mutable bool connectionEventOcurred = false;
    mutable bool listenerStarted = false;

    // Connection objects
    HQUIC Connection = nullptr;
    static inline const QUIC_API_TABLE* MsQuic = nullptr;
    HQUIC Registration;
    HQUIC Configuration;
    HQUIC DefaultStream;
    QUIC_SETTINGS Settings = {0};

    // map of streams and their corresponding pipes. The key is the stream id, which is a 62 bit unsigned integer.
    std::map<HQUIC, Pipes> multiStreams;
    std::mutex multiStreamMutex;

    std::string mHostname;
    uint16_t mPort;
    const QUIC_REGISTRATION_CONFIG RegConfig = { "GvirtuS", QUIC_EXECUTION_PROFILE_LOW_LATENCY };


};

}; // namespace gvirtus::communicators
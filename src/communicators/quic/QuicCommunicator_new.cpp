#include <stdexcept>
#include <iostream>
#include <fcntl.h>
#include <gvirtus/communicators/Endpoint_Quic.h>
#include "QuicCommunicator_new.h"

// =============================================================================
// Debug printing macro
// =============================================================================
#define DEBUG

#ifdef QUICCOM_DEBUG_LEVEL
    #define DEBUG_PRINTF(fmt, ...) do { printf("[tid %lu] [%s:%d] " fmt "\n", syscall(SYS_gettid),__PRETTY_FUNCTION__, __LINE__, ##__VA_ARGS__); } while(0)
#else
    #define DEBUG_PRINTF(fmt, ...) do {} while(0)
#endif


// =============================================================================
// Helper functions
// =============================================================================

bool GetFlag(int argc, const char* argv[], const char* name)
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

const char* GetValue(int argc, const char* argv[], const char* name)
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

uint8_t DecodeHexChar(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    return 0;
}

//
// Helper function to convert a string of hex characters to a byte buffer.
//
uint32_t DecodeHexBuffer(const char* HexBuffer, uint32_t OutBufferLen, uint8_t* OutBuffer)
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


// =============================================================================
// Public methods
// =============================================================================

namespace gvirtus::communicators {

// =========================
// SHARED SETTINGS LOADER
// =========================
bool QuicCommunicator::LoadQuicSettingsFromJson(QUIC_SETTINGS& Settings) {
    try {
        const char* gvirtus_home = getenv("GVIRTUS_HOME");
        if (gvirtus_home == nullptr) {
            printf("GVIRTUS_HOME environment variable not set\n");
            return FALSE;
        }

        fs::path settingsPath = fs::path(gvirtus_home) / "etc" / "quic_settings.json";

        DEBUG_PRINTF("Loading JSON from %s", settingsPath.c_str());

        gvirtus::common::JSON<QuicSettingsConfig> jsonLoader(settingsPath);
        QuicSettingsConfig cfg = jsonLoader.parser();

        Settings = cfg.ToQuicSettings();

        // =========================
        // VALIDATION
        // =========================
        auto fix_uint16 = [&](const char* name, uint16_t val, uint64_t isSet, uint16_t def) {
            if (isSet && val == 0) {
                printf("FIX: %s was 0 → using default %u\n", name, def);
                return def;
            }
            return val;
        };

        auto fix_uint32 = [&](const char* name, uint32_t val, uint64_t isSet, uint32_t def) {
            if (isSet && val == 0) {
                printf("FIX: %s was 0 → using default %u\n", name, def);
                return def;
            }
            return val;
        };

        auto fix_uint64 = [&](const char* name, uint64_t val, uint64_t isSet, uint64_t def) {
            if (isSet && val == 0) {
                printf("FIX: %s was 0 → using default %lu\n", name, def);
                return def;
            }
            return val;
        };

        Settings.PeerBidiStreamCount = fix_uint16(
            "PeerBidiStreamCount",
            Settings.PeerBidiStreamCount,
            Settings.IsSet.PeerBidiStreamCount,
            65535);

        Settings.PeerUnidiStreamCount = fix_uint16(
            "PeerUnidiStreamCount",
            Settings.PeerUnidiStreamCount,
            Settings.IsSet.PeerUnidiStreamCount,
            65535);

        Settings.StreamRecvWindowDefault = fix_uint32(
            "StreamRecvWindowDefault",
            Settings.StreamRecvWindowDefault,
            Settings.IsSet.StreamRecvWindowDefault,
            65536);

        Settings.ConnFlowControlWindow = fix_uint32(
            "ConnFlowControlWindow",
            Settings.ConnFlowControlWindow,
            Settings.IsSet.ConnFlowControlWindow,
            65536);

        Settings.InitialWindowPackets = fix_uint32(
            "InitialWindowPackets",
            Settings.InitialWindowPackets,
            Settings.IsSet.InitialWindowPackets,
            10);

        Settings.IdleTimeoutMs = fix_uint64(
            "IdleTimeoutMs",
            Settings.IdleTimeoutMs,
            Settings.IsSet.IdleTimeoutMs,
            30000);

        printf("=== QUIC SETTINGS LOADED ===\n");
        printf("PeerBidiStreamCount = %u\n", Settings.PeerBidiStreamCount);
        printf("PeerUnidiStreamCount = %u\n", Settings.PeerUnidiStreamCount);
        printf("StreamRecvWindowDefault = %u\n", Settings.StreamRecvWindowDefault);
        printf("ConnFlowControlWindow = %u\n", Settings.ConnFlowControlWindow);
        printf("IdleTimeoutMs = %lu\n", Settings.IdleTimeoutMs);

    } catch (const std::exception& e) {
        printf("Failed to load/parse quic_settings.json: %s\n", e.what());
        return FALSE;
    }

    return TRUE;
}


QuicCommunicator::QuicCommunicator(const QuicCommunicator& other)
    : Listener(nullptr),
      receivedConnection(nullptr),
      cv(),
      listenerMutex(),
      connectionEventOcurred(false),
      listenerStarted(false),
      Connection(nullptr),
      Registration(other.Registration),
      Configuration(other.Configuration),
      DefaultStream(nullptr),
      Settings(other.Settings),
      mHostname(other.mHostname),
      mPort(other.mPort)
{
    std::cout << "QuicCommunicator copy constructor called" << std::endl;
}

QuicCommunicator::QuicCommunicator(const std::string &communicator) {
    // DEBUG_PRINTF("called with connection %p", QuicCommunicator::Connection);
    DefaultStream = NULL;
    Listener = NULL;


    const char *valueptr = strstr(communicator.c_str(), "://") + 3;
    const char *portptr = strchr(valueptr, ':');
    if (portptr == NULL)
        throw "Port not specified.";
    mPort = (short) strtol(portptr + 1, NULL, 10);

    char *hostname = strdup(valueptr);

    hostname[portptr - valueptr] = 0;
    mHostname = std::string(hostname);
    free(hostname);
}

QuicCommunicator::~QuicCommunicator() {
    if (DefaultStream!=NULL)
        MsQuic->StreamClose(DefaultStream);
        
    for (auto& pair : cudaStreamMap) {
        MsQuic->StreamClose(pair.second);
        cudaStreamMap.erase(pair.first);
    }
    if (Connection != NULL)
        MsQuic->ConnectionClose(Connection);

    // TODO: This should probably do more stuff
}


// Server

void QuicCommunicator::Serve() {
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
    if (!ServerLoadConfiguration(argc, argv)) {
        return;
    }

    //
    // Create/allocate a new listener object.
    //
    if (QUIC_FAILED(Status = MsQuic->ListenerOpen(Registration, ServerListenerCallbackWrapper, this, &Listener))) {
        printf("ListenerOpen failed, 0x%x!\n", Status);
        throw std::runtime_error("ListenerOpen failed");
    }

    std::cout << "QuicCommunicator::Accept() called, waiting for connection..." << std::endl;

    QUIC_ADDR Address = {0};
    QuicAddrSetFamily(&Address, QUIC_ADDRESS_FAMILY_UNSPEC);
    QuicAddrSetPort(&Address, mPort);

    // if (listenerStarted == false){
    if (QUIC_FAILED(Status = MsQuic->ListenerStart(Listener, &Alpn, 1, &Address))) {
        throw std::runtime_error("ListenerStart failed");
    }
        // listenerStarted = true;
    printf("ListenerStart returned 0x%x\n", Status);
    // }
    //
#ifdef DEBUG
    printf("QuicCommunicator::Serve() returned\n");
#endif
}


const gvirtus::communicators::Communicator *const QuicCommunicator::Accept() const {

    // Wait for connection request to arrive.
    std::unique_lock<std::mutex> lock(listenerMutex);

    cv.wait(lock, [this] { return connectionEventOcurred; });
    
    std::cout<< "Connection event occurred, processing connection... "<< receivedConnection << std::endl;
    

    DEBUG_PRINTF("New Connection Received\n");

    int stream_count = 65535;
    QuicCommunicator* newQuicCommunicator = newQuicCommunicator = new QuicCommunicator(*this);
    newQuicCommunicator->Connection = receivedConnection;
    newQuicCommunicator->MsQuic->SetCallbackHandler(newQuicCommunicator->Connection, (void*) ServerConnectionCallbackWrapper, newQuicCommunicator);
    newQuicCommunicator->MsQuic->ConnectionSetConfiguration(newQuicCommunicator->Connection, Configuration);
    newQuicCommunicator->MsQuic->SetParam(newQuicCommunicator->Connection, QUIC_PARAM_CONN_LOCAL_BIDI_STREAM_COUNT, sizeof(int), &stream_count);
    newQuicCommunicator->MsQuic->SetParam(newQuicCommunicator->Connection, QUIC_PARAM_CONN_LOCAL_UNIDI_STREAM_COUNT, sizeof(int), &stream_count);
    receivedConnection = NULL;
    connectionEventOcurred = false;
    std::cout << "New connection accepted and configured, returning communicator..." << std::endl;
    // TODO: maybe this communicator shoudl be saved to a list of communicators so that in async the server can handle multiple reads
    return newQuicCommunicator; //new 
}

void QuicCommunicator::Sync() {}

void QuicCommunicator::Close() {
    printf("QuicCommunicator::Close\n");
    if (DefaultStream!=NULL)
        MsQuic->StreamClose(DefaultStream);
    if (Connection != NULL)
        MsQuic->ConnectionClose(Connection);

    // TODO: This should probably do more stuff

}

// Client


void QuicCommunicator::Connect() {
#ifdef DEBUG
    printf("QuicCommunicator::Connect() called\n");
#endif

    if (Connection != NULL) {
        printf("Connection is already open\n");
        return;
    }
    this->InitializeQuic();
    QUIC_STATUS Status;
    // TODO REDO CLIENT LOAD CONFIGURATION
    if (!ClientLoadConfiguration(false)) {
        return;
    }

    Status = MsQuic->ConnectionOpen(Registration, ClientConnectionCallbackWrapper, this, &Connection);

    int stream_count = 65535;
    MsQuic->SetParam(Connection, QUIC_PARAM_CONN_LOCAL_BIDI_STREAM_COUNT, sizeof(stream_count), &stream_count);
    MsQuic->SetParam(Connection, QUIC_PARAM_CONN_LOCAL_UNIDI_STREAM_COUNT, sizeof(stream_count), &stream_count);

    std::cout << "[conn][" << Connection << "] Connecting..." << std::endl;
    std::cout << "hostname: " << mHostname << std::endl;
    std::cout << "port: " << mPort << std::endl; 


    Status = MsQuic->ConnectionStart(Connection, Configuration, QUIC_ADDRESS_FAMILY_UNSPEC, mHostname.data(), mPort);
    std::cout << "ConnectionStart returned " << Status << std::endl;

    {
        std::cout << "Waiting for lock event..." << std::endl;
        std::unique_lock<std::mutex> lock(listenerMutex);
        std::cout << "Waiting for connection event..." << std::endl;
        cv.wait(lock, [this] { return connectionEventOcurred; });
    }



    // Open Default Stream
    if (QUIC_FAILED(Status = MsQuic->StreamOpen(Connection, QUIC_STREAM_OPEN_FLAG_NONE, ClientStreamCallbackWrapper, this, &DefaultStream))) {
        printf("StreamOpen failed, 0x%x!\n", Status);
        throw std::runtime_error("StreamOpen failed");
    }

    // Start Default Stream
    if (QUIC_FAILED(Status = MsQuic->StreamStart(DefaultStream, QUIC_STREAM_START_FLAG_NONE))) {
        printf("StreamStart failed, 0x%x!\n", Status);
        MsQuic->StreamClose(DefaultStream);
        throw "StreamStart failed";
    }


    multiStreams[DefaultStream] = InitializePipes();
    // TODO: NEED TO LOOK INTO PIPES!!
}


// =============================================================================
// Private methods
// =============================================================================

void QuicCommunicator::InitializeQuic() {
    std::cout << "QuicCommunicator::InitializeQuic() called" << std::endl;
    DefaultStream = NULL;

    QUIC_STATUS Status = QUIC_STATUS_SUCCESS;

    if (QUIC_FAILED(Status = MsQuicOpen2(&MsQuic))) {
                throw std::runtime_error("MsQuicOpen2 failed.");
    }

    if (QUIC_FAILED(Status = MsQuic->RegistrationOpen(&RegConfig, &Registration))) {
        throw std::runtime_error("RegistrationOpen failed.");
    }
    Connection = NULL;
    DefaultStream = NULL;
    Listener = NULL;
}

Pipes gvirtus::communicators::QuicCommunicator::InitializePipes() {
    int pipes[2];
    if (pipe(pipes) == -1) {
        printf("Failed to create pipe\n");
        throw std::runtime_error("Failed to create pipe");
    }
    Pipes fdPipes {pipes[0], pipes[1]};

    int pipe_size = 4096 * 4096 * 4; // 1MB buffer or adjust as needed
    fcntl(fdPipes.read, F_SETPIPE_SZ, pipe_size);
    fcntl(fdPipes.write, F_SETPIPE_SZ, pipe_size);

    return fdPipes;
}

bool QuicCommunicator::ServerLoadConfiguration(int argc, const char* argv[])
{


        QUIC_SETTINGS Settings = {0};

    // =========================
    // LOAD SHARED SETTINGS
    // =========================
    if (!LoadQuicSettingsFromJson(Settings)) {
        return FALSE;
    }

    // =========================
    // SERVER-SPECIFIC SETTINGS
    // =========================
    Settings.ServerResumptionLevel = QUIC_SERVER_RESUME_AND_ZERORTT;
    Settings.IsSet.ServerResumptionLevel = TRUE;

    // =========================
    // CERT CONFIG
    // =========================
    QUIC_CREDENTIAL_CONFIG_HELPER Config;
    memset(&Config, 0, sizeof(Config));
    Config.CredConfig.Flags = QUIC_CREDENTIAL_FLAG_NONE;

    const char* Cert;
    const char* KeyFile;

    if ((Cert = GetValue(argc, argv, "cert_hash")) != NULL) {

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

    // =========================
    // APPLY CONFIG
    // =========================
    QUIC_STATUS Status = QUIC_STATUS_SUCCESS;

    if (QUIC_FAILED(Status = MsQuic->ConfigurationOpen(
            Registration,
            &Alpn,
            1,
            &Settings,
            sizeof(Settings),
            NULL,
            &Configuration))) {

        printf("ConfigurationOpen failed, 0x%x!\n", Status);
        return FALSE;
    }

    if (QUIC_FAILED(Status = MsQuic->ConfigurationLoadCredential(
            Configuration,
            &Config.CredConfig))) {

        printf("ConfigurationLoadCredential failed, 0x%x!\n", Status);
        return FALSE;
    }

    return TRUE;
}

bool QuicCommunicator::ClientLoadConfiguration(bool secure) {

    QUIC_SETTINGS Settings = {0};

    // =========================
    // LOAD SHARED SETTINGS
    // =========================
    if (!LoadQuicSettingsFromJson(Settings)) {
        return FALSE;
    }

    // =========================
    // QUIC CONFIGURATION
    // =========================
    QUIC_CREDENTIAL_CONFIG CredConfig;
    memset(&CredConfig, 0, sizeof(CredConfig));
    CredConfig.Type = QUIC_CREDENTIAL_TYPE_NONE;
    CredConfig.Flags = QUIC_CREDENTIAL_FLAG_CLIENT;

    if (!secure) {
        CredConfig.Flags |= QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;
    }

    QUIC_STATUS Status;

    if (QUIC_FAILED(Status = MsQuic->ConfigurationOpen(
            Registration,
            &Alpn,
            1,
            &Settings,
            sizeof(Settings),
            NULL,
            &Configuration))) {

        printf("ConfigurationOpen failed, 0x%x\n", Status);
        return FALSE;
    }

    if (QUIC_FAILED(Status = MsQuic->ConfigurationLoadCredential(
            Configuration,
            &CredConfig))) {

        printf("ConfigurationLoadCredential failed, 0x%x\n", Status);
        return FALSE;
    }

    return TRUE;
}

// =============================================================================
// Callbacks
// =============================================================================

QUIC_STATUS QuicCommunicator::ServerListenerCallbackWrapper(HQUIC Listener, void* Context, QUIC_LISTENER_EVENT* Event) {
    auto quicCommunicator = static_cast<QuicCommunicator*>(Context);
    return quicCommunicator->ServerListenerCallback(Listener, Context, Event);
}

QUIC_STATUS QuicCommunicator::ServerListenerCallback(HQUIC Listener, void* Context, QUIC_LISTENER_EVENT* Event)
{
    // std::unique_lock<std::mutex> lock(ListenerStartMutex);
    int c = 1024;
    QUIC_STATUS Status = QUIC_STATUS_NOT_SUPPORTED;

    switch (Event->Type) {
        case QUIC_LISTENER_EVENT_NEW_CONNECTION:
            printf("QUIC_LISTENER_EVENT_NEW_CONNECTION\n");

            {
                std::scoped_lock<std::mutex> lock(listenerMutex);
                receivedConnection = Event->NEW_CONNECTION.Connection;
                connectionEventOcurred = true;
                cv.notify_one();
            }

            usleep(100); // TODO: this should be replaced by a condition variable that waits for the connection event to be processed before accepting new connections, otherwise
            std::cout << "Notified connection event, waiting for it to be processed before accepting new connections..." << std::endl;
            return QUIC_STATUS_SUCCESS;

        case QUIC_LISTENER_EVENT_STOP_COMPLETE:
            printf("QUIC_LISTENER_EVENT_STOP_COMPLETE received.\n");
            break;

        case QUIC_LISTENER_EVENT_DOS_MODE_CHANGED:
            printf("QUIC_LISTENER_EVENT_DOS_MODE_CHANGED received.\n");
            break;

        default:
            break;
        }

    return Status;
}


QUIC_STATUS QuicCommunicator::ServerConnectionCallbackWrapper(HQUIC Connection, void* Context, QUIC_CONNECTION_EVENT* Event) {
    auto quicCommunicator = static_cast<QuicCommunicator*>(Context);
    std::cout << "QuicCommunicator::ServerConnectionCallbackWrapper called with Connection: " << Connection << " in instance " << quicCommunicator << std::endl;
    return quicCommunicator->ServerConnectionCallback(Connection, Context, Event);
}

QUIC_STATUS QuicCommunicator::ServerConnectionCallback(HQUIC Connection, void* Context, QUIC_CONNECTION_EVENT* Event)
{

    DEBUG_PRINTF("[conn %p] [msquic %p] %s event type %d\n", Connection, MsQuic, __PRETTY_FUNCTION__, Event->Type);
    // uint32_t sidSize = sizeof(sid);

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

    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
        //
        // The connection has completed the shutdown process and is ready to be
        // safely cleaned up.
        //
        printf("[conn][%p] All done\n", Connection);
        MsQuic->ConnectionClose(Connection);
        // TODO: Should free all streams.
        break;
        
    case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED:

        std::cout << "QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED received. Stream: " << Event->PEER_STREAM_STARTED.Stream << std::endl;
        std::cout << "DefaultStream: " << DefaultStream << std::endl;
        {
            // std::scoped_lock(multiStreamMutex);
            if (DefaultStream == nullptr) {
                DefaultStream = Event->PEER_STREAM_STARTED.Stream;
                std::cout << "DefaultStream set to: " << DefaultStream << std::endl;
                streamStarted[DefaultStream] = true;
            } else {

            }
            MsQuic->SetCallbackHandler(Event->PEER_STREAM_STARTED.Stream, (void *) ServerStreamCallbackWrapper, this);
            {
                std::scoped_lock(multiStreamMutex);
                multiStreams[Event->PEER_STREAM_STARTED.Stream] = InitializePipes();
                
                // Notify Read that a new stream has been created and is ready to be used
                cv.notify_all();
                std::cout << "ServerConnectionCallback: Notified all threads about new stream" << std::endl;
            }
        }

        break;

    default:
        printf("Unkown or unsupported Connection event %i", Event->Type);
        break;
    }
    std::cout << "Received connection event type: " << Event->Type << ", exiting ServerConnectionCallback with success" << std::endl;
    return QUIC_STATUS_SUCCESS;
}

QUIC_STATUS QuicCommunicator::ServerStreamCallbackWrapper(HQUIC Stream, void* Context, QUIC_STREAM_EVENT* Event) {
    auto quicCommunicator = static_cast<QuicCommunicator*>(Context);
    return quicCommunicator->ServerStreamCallback(Stream, Context, Event);
}

QUIC_STATUS QuicCommunicator::ServerStreamCallback(HQUIC Stream, void* Context, QUIC_STREAM_EVENT* Event)
{
    auto communicator = static_cast<QuicCommunicator*>(Context);
    QUIC_BUFFER* qb=NULL;
    DEBUG_PRINTF("[sid %lu] called", sid);
    int wp = -1;
    

    switch (Event->Type) {
        case QUIC_STREAM_EVENT_SEND_COMPLETE:
            qb = (QUIC_BUFFER*)Event->SEND_COMPLETE.ClientContext;
            DEBUG_PRINTF("[strm][%p] Sent is complete.\n", Stream);
            free(Event->SEND_COMPLETE.ClientContext);
            return QUIC_STATUS_SUCCESS;

        case QUIC_STREAM_EVENT_RECEIVE:

            std::cout << "ServerStreamCallback: Received data on stream: " << Stream << std::endl;
            if (multiStreams.find(Stream) == multiStreams.end()) {
                return QUIC_STATUS_NOT_FOUND;
            }
            
            if (streamStarted[Stream] == false) {
                std::cout << "First data received on stream " << Stream << ", treating it as stream initialization and extracting cuda stream pointer..." << std::endl;
                // std::unique_lock<std::mutex> slock(cudaStreamMapMutex); //TODO : THIS NEEDS TO BE LOCKED BUT WITHOUT CAUSING A DEADLOCK (REVISIT)
                std::cout << "Did pass the lock for stream " << Stream << std::endl;
                cuda_stream_ptr ptr;
                uint16_t offset = 0;
                int i = 0;
                while (offset < sizeof(cuda_stream_ptr)) {
                    uint16_t chunk_size = std::min((uint16_t)(Event->RECEIVE.Buffers[i].Length - offset), (uint16_t)sizeof(cuda_stream_ptr));
                    memcpy(((char*)&ptr) + offset, Event->RECEIVE.Buffers[i].Buffer + offset, chunk_size);
                    offset += chunk_size;
                    i++;
                }

                std::cout << "Received new stream with cuda stream pointer: " << ptr << std::endl;
                cudaStreamMap[ptr] = Stream;
                // slock.unlock();

                std::unique_lock<std::mutex> lock(multiStreamMutex);
                streamStarted[Stream] = true;

                cv.notify_all();
                return QUIC_STATUS_SUCCESS;
            }
            
            wp = multiStreams[Stream].write;
            DEBUG_PRINTF("[sid %lu] Get pipe %d for stream %p\n", sid, wp, Stream);
        

            for (uint32_t i = 0; i < Event->RECEIVE.BufferCount; ++i) {
                
                const QUIC_BUFFER* b = &Event->RECEIVE.Buffers[i];
                DEBUG_PRINTF("[sid %lu] [strm %p] [pipe %d] Data received %u, flags %d\n", sid, Stream, wp, b->Length, Event->RECEIVE.Flags);
                
                // TODO: May be substituted by non blocking write
                std::cout << b->Length << " bytes received on stream " << Stream << ", writing to pipe " << wp << std::endl;
                if (write(wp, b->Buffer, b->Length) == -1) {
                    printf("Failed to write to pipe\n");
                    throw std::runtime_error("Failed to write to pipe");
                }
                DEBUG_PRINTF("[sid %lu] [strm %p] [pipe %d] Data written %u, flags %d\n", sid, Stream, wp, b->Length, Event->RECEIVE.Flags);
            }

            return QUIC_STATUS_SUCCESS;

        case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN:
            DEBUG_PRINTF("[strm][%p] Peer shut down\n", Stream);
            return QUIC_STATUS_SUCCESS;

        case QUIC_STREAM_EVENT_PEER_SEND_ABORTED:
            DEBUG_PRINTF("[strm][%p] Peer aborted\n", Stream);
            MsQuic->StreamShutdown(Stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
            return QUIC_STATUS_SUCCESS;

        case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
            DEBUG_PRINTF("[strm][%p] All done\n", Stream);
            MsQuic->StreamClose(Stream);
            
            if (multiStreams.find(Stream) != multiStreams.end()) {
                std::scoped_lock(multiStreamMutex);
                close(multiStreams[Stream].read);
                close(multiStreams[Stream].write);
                multiStreams.erase(Stream);

            }
            return QUIC_STATUS_SUCCESS;

        default:
            break;
    }
    return QUIC_STATUS_NOT_FOUND;
}

QUIC_STATUS QuicCommunicator::ClientConnectionCallbackWrapper(HQUIC Connection, void* Context, QUIC_CONNECTION_EVENT* Event) {
    auto quicCommunicator = static_cast<QuicCommunicator*>(Context);
    return quicCommunicator->ClientConnectionCallback(Connection, Context, Event);
}

QUIC_STATUS QuicCommunicator::ClientConnectionCallback(HQUIC Connection, void* Context, QUIC_CONNECTION_EVENT* Event) {

    switch (Event->Type) {
        case QUIC_CONNECTION_EVENT_CONNECTED:
            {
                std::scoped_lock<std::mutex> lock(listenerMutex);
                this->Connection=Connection;
                connectionEventOcurred = true;
                cv.notify_one();
            }
            usleep(100); // TODO: this should be replaced by a condition variable that waits
            break;

        case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:

            if (Event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status == QUIC_STATUS_CONNECTION_IDLE) {
                printf("[conn][%p] Successfully shut down on idle.\n", Connection);
            } else {
                printf("[conn][%p] Shut down by transport, 0x%x\n", Connection, Event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status);
            }
            break;

        case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
            printf("[conn][%p] Shut down by peer, 0x%llu\n", Connection, (unsigned long long)Event->SHUTDOWN_INITIATED_BY_PEER.ErrorCode);
            break;

        case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
            printf("[conn][%p] All done\n", Connection);
            if (!Event->SHUTDOWN_COMPLETE.AppCloseInProgress) {
                MsQuic->ConnectionClose(Connection);
            }
            break;

        case QUIC_CONNECTION_EVENT_RESUMPTION_TICKET_RECEIVED:
            break;

        default:
            break;
        }
    return QUIC_STATUS_SUCCESS;
}

QUIC_STATUS QuicCommunicator::ClientStreamCallbackWrapper(HQUIC Stream, void* Context, QUIC_STREAM_EVENT* Event) {
    auto quicCommunicator = static_cast<QuicCommunicator*>(Context);
    return quicCommunicator->ClientStreamCallback(Stream, Context, Event);
}
    

QUIC_STATUS QuicCommunicator::ClientStreamCallback(HQUIC Stream, void* Context, QUIC_STREAM_EVENT* Event) {
    int wp = -1;

    QUIC_BUFFER* qb=NULL;

    switch (Event->Type) {
        case QUIC_STREAM_EVENT_START_COMPLETE:
            if (Event->START_COMPLETE.Status !=  QUIC_STATUS_SUCCESS) {
                DEBUG_PRINTF("[strm][%p] Stream failed to start, 0x%x\n", Stream, Event->START_COMPLETE.Status);
                MsQuic->StreamClose(Stream);
            } else {

            }
            break;

        case QUIC_STREAM_EVENT_SEND_COMPLETE:
            qb = (QUIC_BUFFER*)Event->SEND_COMPLETE.ClientContext;

            DEBUG_PRINTF("[strm][%p] Data sent %d\n", Stream, qb->Length) ;
            free(Event->SEND_COMPLETE.ClientContext);
            break;

        case QUIC_STREAM_EVENT_RECEIVE:
            if (multiStreams.find(Stream) == multiStreams.end()) {
                DEBUG_PRINTF("[sid %lu] Pipe not found for stream %p\n", sid, Stream);
                return QUIC_STATUS_NOT_FOUND;
            }
            else {
                wp = multiStreams[Stream].write;
                DEBUG_PRINTF("[sid %lu] Get pipe %d for stream %p\n", sid, wp, Stream);
            }
            
            for (uint32_t i = 0; i < Event->RECEIVE.BufferCount; ++i) {
                
                const QUIC_BUFFER* b = &Event->RECEIVE.Buffers[i];
                DEBUG_PRINTF("[sid %lu] [strm %p] [pipe %d] Data received %u, flags %d\n", sid, Stream, wp, b->Length, Event->RECEIVE.Flags);
                
                // TODO: May be substituted by non blocking write
                if (write(wp, b->Buffer, b->Length) == -1) {
                    printf("Failed to write to pipe\n");
                    throw std::runtime_error("Failed to write to pipe");
                }
                DEBUG_PRINTF("[sid %lu] [strm %p] [pipe %d] Data written %u, flags %d\n", sid, Stream, wp, b->Length, Event->RECEIVE.Flags);
            }
            return QUIC_STATUS_SUCCESS;

        case QUIC_STREAM_EVENT_PEER_SEND_ABORTED:
            DEBUG_PRINTF("[strm][%p] Peer aborted\n", Stream);
            break;

        case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN:
            DEBUG_PRINTF("[strm][%p] Peer shut down\n", Stream);
            break;

        case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
            DEBUG_PRINTF("[strm][%p] All done\n", Stream);
            if (!Event->SHUTDOWN_COMPLETE.AppCloseInProgress) {
                MsQuic->StreamClose(Stream);
            }
            break;

        default:
            break;
    }

    return QUIC_STATUS_SUCCESS;
}

// =============================================================================
// Read / Write Functions
// ============================================================================

size_t QuicCommunicator::Read(char *buffer, size_t size) {

    // TODO: REVIEW THIS FUNCTION
    ssize_t ret_value=0;
    ssize_t size_left=size;
    if (DefaultStream == nullptr) {
        std::unique_lock<std::mutex> lock(listenerMutex);
        cv.wait(lock, [this] { return multiStreams.find(DefaultStream) != multiStreams.end(); });
    } 
    // std::cout << "Pipe for DefaultStream is ready, proceeding with read" << std::endl;

    while(size_left>0) {

        DEBUG_PRINTF("[sid %lu] QuicCommunicator::Read() Block on read() %ld %lu %lu\n", sid, ret_value,size,size_left);
        ssize_t r = read(multiStreams[DefaultStream].read, buffer+ret_value, size_left);

        if (r < 0) {
            DEBUG_PRINTF("errno %d\n",errno);
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                //usleep(10);
                continue;
            }
            continue;
        }
        
        if (r < 0 || r==0){
            ret_value = 0;
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


    return ret_value;
}

size_t QuicCommunicator::Read_Async(char *buffer, size_t size, cuda_stream_ptr stream) {

    // TODO: REVIEW THIS FUNCTION
    ssize_t ret_value=0;
    ssize_t size_left=size;
    // if (cudaStreamMap.find(stream) == cudaStreamMap.end()) {
    //     std::unique_lock<std::mutex> lock(listenerMutex);
    //     cv.wait(lock, [this, stream] { return cudaStreamMap.find(stream) != cudaStreamMap.end(); });
    // } 
    // std::cout << "Pipe for DefaultStream is ready, proceeding with read" << std::endl;
    std::cout << "QuicCommunicator::Read_Async called with stream: " << stream << " and size: " << size << std::endl;
    while(size_left>0) {
        std::cout << "REading data... size left: " << size_left << ", ret_value: " << ret_value << std::endl;
        DEBUG_PRINTF("[sid %lu] QuicCommunicator::Read() Block on read() %ld %lu %lu\n", sid, ret_value,size,size_left);
        ssize_t r = read(multiStreams[cudaStreamMap[stream]].read, buffer+ret_value, size_left);
        std::cout << "Passed read() with return value: " << r << std::endl;

        if (r < 0) {
            DEBUG_PRINTF("errno %d\n",errno);
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                //usleep(10);
                continue;
            }
            continue;
        }
        
        if (r < 0 || r==0){
            ret_value = 0;
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


    return ret_value;
}


size_t QuicCommunicator::Write(const char *buffer, size_t size) {

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
        if (QUIC_FAILED(Status = MsQuic->StreamSend(DefaultStream, SendBuffer, 1, QUIC_SEND_FLAG_NONE, SendBuffer))) {
            printf("StreamSend failed, 0x%x!\n", Status);
            free(SendBufferRaw);
        }
    }
    

    return size;
}


size_t QuicCommunicator::Write_Async(const char *buffer, size_t size, cuda_stream_ptr stream) {

    QUIC_STATUS Status;
    //
    // Allocates and builds the buffer to send over the stream.
    //
    size_t MAX_BUF_SIZE = 4096*4096*4;
    size_t size_left = size;
    size_t send_size = 0;
    size_t send_size_cum = 0;

    if (cudaStreamMap.find(stream) == cudaStreamMap.end()) {
        printf("CUDA stream not found in map\n");
        void * a;
        memcpy(&a, buffer, size);
        // std::unique_lock<std::mutex> lock(cudaStreamMapMutex);
        // cv.wait(lock, [this, stream] { return cudaStreamMap.find(stream) != cudaStreamMap.end(); });
    }


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
        }
        memcpy(SendBufferRaw+sizeof(QUIC_BUFFER), buffer+send_size_cum, send_size);
        SendBuffer = (QUIC_BUFFER*)SendBufferRaw;
        SendBuffer->Buffer = SendBufferRaw + sizeof(QUIC_BUFFER);
        SendBuffer->Length = send_size;

        DEBUG_PRINTF("[strm %p] Sending data... %ld %ld\n", cudaStreamMap[stream], send_size, sizeof(QUIC_BUFFER));
        send_size_cum += send_size;
        size_left -= send_size;


        //s
        // Sends the buffer over the stream. Note the FIN flag is passed along with
        // the buffer. This indicates this is the last buffer on the stream and the
        // the stream is shut down (in the send direction) immediately after.
        //



        if (QUIC_FAILED(Status = MsQuic->StreamSend(cudaStreamMap[stream] , SendBuffer, 1, QUIC_SEND_FLAG_NONE, SendBuffer))) {
            printf("StreamSend failed, 0x%x!\n", Status);
            free(SendBufferRaw);
        }
    }
    

    return size;
}


void QuicCommunicator::Start_Stream(cuda_stream_ptr stream) {

    QUIC_STATUS Status;

    if (cudaStreamMap.find(stream) == cudaStreamMap.end()) {
        std::scoped_lock(cudaStreamMapMutex);
        // Open Stream
        cudaStreamMap[stream] = nullptr;
        if (QUIC_FAILED(Status = MsQuic->StreamOpen(Connection, QUIC_STREAM_OPEN_FLAG_NONE, ClientStreamCallbackWrapper, this, &cudaStreamMap[stream]))) {
            printf("StreamOpen failed, 0x%x!\n", Status);
            throw std::runtime_error("StreamOpen failed");
        }

        // Start Default Stream
        if (QUIC_FAILED(Status = MsQuic->StreamStart(cudaStreamMap[stream], QUIC_STREAM_START_FLAG_NONE))) {
            printf("StreamStart failed, 0x%x!\n", Status);
            MsQuic->StreamClose(cudaStreamMap[stream]);
            throw "StreamStart failed";
        }


        multiStreams[cudaStreamMap[stream]] = InitializePipes();

        // Maybe flag should be

        this->Write_Async((char*) &stream, sizeof(cuda_stream_ptr), stream);
        
        // TODO: THIS NEEDS TO HAVE A Stop_Stream

    }
}

void QuicCommunicator::Server_Start_Stream(cuda_stream_ptr stream) {
    // This function is called when the server receives a new stream from the client, and it needs to associate that stream with the corresponding CUDA stream pointer. The client sends the CUDA stream pointer as the first message on the new stream, so the server needs to read that message and extract the CUDA stream pointer from it.
    std::unique_lock<std::mutex> lock(cudaStreamMapMutex);
    if (cudaStreamMap.find(stream) == cudaStreamMap.end()) {
        std::unique_lock<std::mutex> lock(listenerMutex);
        cv.wait(lock, [this, stream] { return cudaStreamMap.find(stream) != cudaStreamMap.end() && streamStarted[cudaStreamMap[stream]] == true; });
    } 
}

};

// =============================================================================
// Factory function
// =============================================================================

extern "C" std::shared_ptr <gvirtus::communicators::QuicCommunicator> create_communicator(
        std::shared_ptr <gvirtus::communicators::Endpoint> end) {
    std::string arg =
            "quic://" +
            std::dynamic_pointer_cast<gvirtus::communicators::Endpoint_Quic>(end)->address() +
            ":" +
            std::to_string(std::dynamic_pointer_cast<gvirtus::communicators::Endpoint_Quic>(end)->port());
    return std::make_shared<gvirtus::communicators::QuicCommunicator>(arg);
}
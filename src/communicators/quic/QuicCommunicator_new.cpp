#include <stdexcept>
#include "QuicCommunicator_new.h"

// =============================================================================
// Debug printing macro
// =============================================================================

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

QuicCommunicator::QuicCommunicator(const QuicCommunicator& other) {
    mHostname = other.mHostname;
    mPort = other.mPort;
    DefaultStream = NULL;
    Listener = NULL;
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
    if (Connection != NULL)
        MsQuic->ConnectionClose(Connection);
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
    if (QUIC_FAILED(Status = MsQuic->ListenerOpen(Registration, ServerListenerCallback, this, &Listener))) {
        printf("ListenerOpen failed, 0x%x!\n", Status);
        throw std::runtime_error("ListenerOpen failed");
    }
    sleep(1);
    //
#ifdef DEBUG
    printf("QuicCommunicator::Serve() returned\n");
#endif
}


const gvirtus::communicators::Communicator *const QuicCommunicator::Accept() const {

    // Wait for connection request to arrive.
    std::unique_lock<std::mutex> lock(listenerMutex);
    listernerCv.wait(lock, [this] { return connectionEventOcurred; });
    
    QUIC_STATUS Status;

    QUIC_ADDR Address = {0};
    QuicAddrSetFamily(&Address, QUIC_ADDRESS_FAMILY_UNSPEC);
    QuicAddrSetPort(&Address, htons(mPort));

    if (listenerStarted == false){
        if (QUIC_FAILED(Status = MsQuic->ListenerStart(Listener, &Alpn, 1, &Address))) {
            throw std::runtime_error("ListenerStart failed");
        }
        listenerStarted = true;

    }

    listernerCv.wait(lock, [this] { return connectionEventOcurred; });
    DEBUG_PRINTF("New Connection Received\n");

    if (receivedConnection == NULL) {
        throw std::runtime_error("Received connection is null");
    }

    int stream_count = 65535;

    QuicCommunicator* newQuicCommunicator = new QuicCommunicator(*this);
    newQuicCommunicator->Connection = receivedConnection;
    newQuicCommunicator->InitializeQuic();
    newQuicCommunicator->MsQuic->SetCallbackHandler(newQuicCommunicator->Connection, (void*)(newQuicCommunicator->ServerConnectionCallback), NULL);
    newQuicCommunicator->MsQuic->SetParam(newQuicCommunicator->Connection, QUIC_PARAM_CONN_LOCAL_BIDI_STREAM_COUNT, sizeof(int), &stream_count);
    newQuicCommunicator->MsQuic->SetParam(newQuicCommunicator->Connection, QUIC_PARAM_CONN_LOCAL_UNIDI_STREAM_COUNT, sizeof(int), &stream_count);

    receivedConnection = NULL;
    connectionEventOcurred = false;

    // TODO: maybe this communicator shoudl be saved to a list of communicators so that in async the server can handle multiple reads

    return newQuicCommunicator; //new 
}


// Client


void QuicCommunicator::Connect() {
#ifdef DEBUG
    printf("QuicCommunicator::Connect() called\n");
#endif

    if (Connection != nullptr) {
        printf("Connection is already open\n");
        return;
    }

    InitializeQuic();


    // TODO REDO CLIENT LOAD CONFIGURATION
    if (!ClientLoadConfiguration(GetFlag(argc, argv, "unsecure"))) {
        return;
    }

    if (QUIC_FAILED(Status = MsQuic->ConnectionOpen(Registration, ClientConnectionCallback, this, &Connection))) {
        printf("ConnectionOpen failed, 0x%x!\n", Status);
        throw std::runtime_error("ConnectionOpen failed");
    }

    MsQuic->SetParam(Connection, QUIC_PARAM_CONN_LOCAL_BIDI_STREAM_COUNT, sizeof(stream_count), &stream_count);
    MsQuic->SetParam(Connection, QUIC_PARAM_CONN_LOCAL_UNIDI_STREAM_COUNT, sizeof(stream_count), &stream_count);


    // Start Connection
    if (QUIC_FAILED(Status = MsQuic->ConnectionStart(QuicCommunicator::Connection, Configuration, QUIC_ADDRESS_FAMILY_UNSPEC, mHostname.data(), htons(mPort)))) {
        printf("ConnectionStart failed, 0x%x!\n", Status);
        throw std::runtime_error("ConnectionStart failed");
    }
    
    // Open Default Stream
    if (QUIC_FAILED(Status = MsQuic->StreamOpen(Connection, QUIC_STREAM_OPEN_FLAG_NONE, ClientStreamCallback, nullptr, &DefaultStream))) {
        printf("StreamOpen failed, 0x%x!\n", Status);
        throw std::runtime_error("StreamOpen failed");
    }

    // Start Default Stream
    if (QUIC_FAILED(Status = MsQuic->StreamStart(Stream, QUIC_STREAM_START_FLAG_NONE))) {
        printf("StreamStart failed, 0x%x!\n", Status);
        MsQuic->StreamClose(Stream);
        throw "StreamStart failed";
    }


    // TODO: NEED TO LOOK INTO PIPES!!
}


// =============================================================================
// Private methods
// =============================================================================

void QuicCommunicator::InitializeQuic(void) {
    DEBUG_PRINTF("QuicCommunicator::InitializeQuic() called");
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

bool QuicCommunicator::ServerLoadConfiguration(int argc, const char* argv[])
{

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

bool QuicCommunicator::ClientLoadConfiguration(int argc, const char* argv[]) {
    // TODO: Add Client Load Configuration
    return true;
}

// =============================================================================
// Callbacks
// =============================================================================


QUIC_STATUS QuicCommunicator::ServerListenerCallback(HQUIC Listener, void* Context, QUIC_LISTENER_EVENT* Event)
{
    // std::unique_lock<std::mutex> lock(ListenerStartMutex);
    int c = 1024;
    QUIC_STATUS Status = QUIC_STATUS_NOT_SUPPORTED;

    switch (Event->Type) {
        case QUIC_LISTENER_EVENT_NEW_CONNECTION:
            printf("QUIC_LISTENER_EVENT_NEW_CONNECTION\n");

            receivedConnection = Event->NEW_CONNECTION.Connection;
            connectionEventOcurred = true;
            listernerCv.notify_one();
            break;
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
            QuicCommunicator newQuicCommunicator{*this};
            newQuicCommunicator.InitializePipes();

            newQuicCommunicator.DefaultStream = Event->PEER_STREAM_STARTED.Stream;
            NewStreamQuicCommunicator->Stream = Event->PEER_STREAM_STARTED.Stream;
            NewStreamQuicCommunicator->Connection = Connection;
            NewStreamQuicCommunicator->sid = tmpsid;
            //set     HQUIC Registration;  HQUIC Configuration; HQUIC Listener;
            MsQuic->SetCallbackHandler(Event->PEER_STREAM_STARTED.Stream, (void *) ServerStreamCallback, NewStreamQuicCommunicator);

            break;
        }

    default:
        printf("Unkown or unsupported Connection event %i", Event->Type);
        break;
    }
    return QUIC_STATUS_SUCCESS;
}


QUIC_STATUS QuicCommunicator::ServerStreamCallback(HQUIC Stream, void* Context, QUIC_STREAM_EVENT* Event)
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

// =============================================================================
// Factory function
// =============================================================================

extern "C" std::shared_ptr <QuicCommunicator> create_communicator(
        std::shared_ptr <gvirtus::communicators::Endpoint> end) {
    std::string arg =
            "quic://" +
            std::dynamic_pointer_cast<gvirtus::communicators::Endpoint_Quic>(end)->address() +
            ":" +
            std::to_string(std::dynamic_pointer_cast<gvirtus::communicators::Endpoint_Quic>(end)->port());
    return std::make_shared<QuicCommunicator>(arg);
}
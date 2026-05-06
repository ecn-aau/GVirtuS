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
 * Written By: Carlo Palmieri <carlo.palmieri@uniparthenope.it>,
 *             Department of Applied Science
 *             Giuseppe Coviello <giuseppe.coviello@uniparthenope.it>,
 *             Department of Applied Science
 *             Raffaele Montella <raffaele.montella@uniparthenope.it>,
 *             Department of Science and Technologies
 *             Antonio Mentone <antonio.mentone@uniparthenope.it>,
 *             Department of Science and Technologies
 * Edited By: Mariano Aponte <aponte2001@gmail.com>,
 *            Department of Science and Technologies, University of Naples Parthenope
 *            Theodoros Aslanidis <theodoros.aslanidis@ucdconnect.ie>,
 *            Department of Computer Science, University College Dublin
 */

#include <gvirtus/communicators/CommunicatorFactory.h>
#include <gvirtus/communicators/EndpointFactory.h>
#include <gvirtus/frontend/Frontend.h>
#include <pthread.h>
#include <stdlib.h> /* getenv */
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <thread>

#include "communicators/hybrid/HybridCommunicator.h"
#include "log4cplus/configurator.h"
#include "log4cplus/logger.h"
#include "log4cplus/loggingmacros.h"
#include "gvirtus/common/Property.h"

using std::chrono::duration_cast;
using std::chrono::milliseconds;
using std::chrono::steady_clock;

using namespace std;
using namespace log4cplus;

using gvirtus::communicators::Buffer;
using gvirtus::communicators::Communicator;
using gvirtus::communicators::CommunicatorFactory;
using gvirtus::communicators::EndpointFactory;
using gvirtus::frontend::Frontend;

static Frontend msFrontend;
std::mutex gFrontendMutex;
map<pthread_t, Frontend *> *Frontend::mpFrontends = NULL;
std::mutex Frontend::asyncOutputBuffersMutex;
std::map<void*, std::shared_ptr<Frontend::AsyncStreamContext>> Frontend::mpAsyncOutputBuffers;
static bool initialized = false;

Logger logger;

std::string getEnvVar(std::string const &key) {
    char *env_var = getenv(key.c_str());
    return (env_var == nullptr) ? std::string("") : std::string(env_var);
}

void Frontend::Init(Communicator *c) {
    // Logger configuration
    BasicConfigurator basicConfigurator;
    basicConfigurator.configure();

    // Set the logging level
    std::string logLevelString = getEnvVar("GVIRTUS_LOGLEVEL");
    LogLevel logLevel = INFO_LOG_LEVEL;
    if (!logLevelString.empty()) {
        try {
            logLevel = static_cast<LogLevel>(std::stoi(logLevelString));
        } catch (const std::exception &e) {
            std::cerr << "[GVIRTUS WARNING] Invalid GVIRTUS_LOGLEVEL value: '" << logLevelString
                      << "'. Using default INFO_LOG_LEVEL. (" << e.what() << ")\n";
            logLevel = INFO_LOG_LEVEL;
        }
    }

    Logger root = Logger::getRoot();
    root.setLogLevel(logLevel);

    logger = Logger::getInstance(LOG4CPLUS_TEXT("Frontend"));

    pid_t tid = syscall(SYS_gettid);

    // Get the GVIRTUS_CONFIG environment varibale
    std::string config_path = getEnvVar("GVIRTUS_CONFIG");

    // Check if the configuration file is defined
    if (config_path.empty()) {
        // Check if the configuration file is in the GVIRTUS_HOME directory
        config_path = getEnvVar("GVIRTUS_HOME") + "/etc/properties.json";
        if (config_path.empty()) {
            // Finally consider the current directory
            config_path = "./properties.json";
        }
    }

    std::unique_ptr<char> default_endpoint;

    LOG4CPLUS_INFO(logger, "Using properties file: " + config_path);

    try {
        auto endpoint = EndpointFactory::get_endpoint(config_path);
        gvirtus::common::Property _properties = common::JSON<gvirtus::common::Property>(config_path).parser();


        
        this->_communicator = CommunicatorFactory::get_communicator(endpoint, _properties.secure());
        this->_communicator->obj_ptr()->Connect();
    } catch (const std::exception &e) {
        LOG4CPLUS_FATAL(logger, fs::path(__FILE__).filename()
                                    << ":" << __LINE__ << ":"
                                    << " Exception occurred: " << e.what());
        exit(EXIT_FAILURE);
    }

    this->mpInputBuffer = std::make_shared<Buffer>();
    this->mpOutputBuffer = std::make_shared<Buffer>();
    this->mpLaunchBuffer = std::make_shared<Buffer>();
    this->mExitCode = -1;
    this->mpInitialized = true;
}

Frontend::~Frontend() {
    static bool destroying = false;
    if (destroying || mpFrontends == nullptr) return;
    destroying = true;

    std::lock_guard<std::mutex> lock(gFrontendMutex);
    {
        pid_t tid = syscall(SYS_gettid);

        auto env = getenv("GVIRTUS_DUMP_STATS");
        bool dump_stats = env && (strcasecmp(env, "on") == 0 || strcasecmp(env, "true") == 0 ||
                                  strcmp(env, "1") == 0);

        // Safe iteration while erasing entries
        for (auto it = mpFrontends->begin(); it != mpFrontends->end(); /* no increment here */) {
            if (it->second == this) {
                it = mpFrontends->erase(it);
                continue;
            }

            if (dump_stats) {
                std::cerr << "[GVIRTUS_STATS] Executed " << it->second->mRoutinesExecuted
                          << " routine(s) in " << it->second->mRoutineExecutionTime
                          << " second(s)\n"
                          << "[GVIRTUS_STATS] Sent " << it->second->mDataSent / (1024 * 1024.0)
                          << " Mb(s) in " << it->second->mSendingTime << " second(s)\n"
                          << "[GVIRTUS_STATS] Received "
                          << it->second->mDataReceived / (1024 * 1024.0) << " Mb(s) in "
                          << it->second->mReceivingTime << " second(s)\n";
            }
            if (it->first != tid) {
                delete it->second;
            }
            it->second->_communicator->obj_ptr()->Close();
            it = mpFrontends->erase(it);
        }

        // Delete the map itself and set pointer to nullptr
        delete mpFrontends;
        mpFrontends = nullptr;
    }
}

Frontend *Frontend::GetFrontend(Communicator *c) {
    {
        std::lock_guard<std::mutex> lock(gFrontendMutex);
        if (mpFrontends == nullptr) mpFrontends = new map<pthread_t, Frontend *>();
    }

    pid_t tid = syscall(SYS_gettid);  // getting frontend's tid

    {
        std::lock_guard<std::mutex> lock(gFrontendMutex);
        auto it = mpFrontends->find(tid);
        if (it != mpFrontends->end()) return it->second;
    }

    Frontend *f = new Frontend();
    try {
        f->Init(c);
        {
            std::lock_guard<std::mutex> lock(gFrontendMutex);
            mpFrontends->insert(make_pair(tid, f));
        }
    } catch (const std::exception &e) {
        LOG4CPLUS_ERROR(logger, "Error initializing Frontend: " << e.what());
        delete f;  // Clean up on failure
        return nullptr;
    }

    return f;
}

void Frontend::Execute(const char *routine, const Buffer *input_buffer) {
    if (input_buffer == nullptr) input_buffer = mpInputBuffer.get();

    pid_t tid = syscall(SYS_gettid);
    pid_t pid = getpid();
    size_t in_size = input_buffer->GetBufferSize();
    int exit_code = 0;
    double server_exec_sec = 0.0;
    double send_sec = 0.0;
    double recv_sec = 0.0;

    Frontend *frontend = nullptr;
    {
        std::lock_guard<std::mutex> lock(gFrontendMutex);
        auto it = mpFrontends->find(tid);
        if (it == mpFrontends->end()) {
            LOG4CPLUS_ERROR(logger, "Cannot send any job request");
            return;
        }
        frontend = it->second;
    }

    LOG4CPLUS_DEBUG(logger, "DEBUG - Received routine " << routine << " [pid=" << pid
                                                        << ", tid=" << tid << "]");

    frontend->mRoutinesExecuted++;

    // ===== send routine info first（under TCP）=====
    auto start_send = steady_clock::now();
    frontend->_communicator->obj_ptr()->Write(routine, strlen(routine) + 1);

    // ===== chose protocol by different routine =====
    if (frontend->_communicator->obj_ptr()->to_string() == "hybridcommunicator") {
        auto *hybrid = dynamic_cast<gvirtus::communicators::HybridCommunicator *>(
            frontend->_communicator->obj_ptr().get());
        if (hybrid) {
            if (std::string(routine).find("cudaMemcpy") != std::string::npos ||
                std::string(routine).find("cudaRegisterFatBinary") != std::string::npos ||
                std::string(routine).find("cudaRegisterFatBinaryEnd") != std::string::npos ||
                std::string(routine).find("cudaMemcpyAsync") != std::string::npos) {
                hybrid->begin_call(routine, gvirtus::communicators::Transport::RDMA, in_size);
            } else {
                hybrid->begin_call(routine, gvirtus::communicators::Transport::TCP, in_size);
            }
        }
    }

    // ===== send paramemter data =====
    frontend->mDataSent += in_size;
    LOG4CPLUS_DEBUG(logger, "Write " << in_size << " bytes to the buffer");
    input_buffer->Dump(frontend->_communicator->obj_ptr().get());

    // ===== sync by chosen channel =====
    frontend->_communicator->obj_ptr()->Sync();

    send_sec = duration_cast<milliseconds>(steady_clock::now() - start_send).count() / 1000.0;

    frontend->mpOutputBuffer->Reset();

    // ===== receive exit_code =====
    auto start_recv = steady_clock::now();
    frontend->_communicator->obj_ptr()->Read((char *)&exit_code, sizeof(int));
    frontend->mExitCode = exit_code;

    // ===== receive backend time cost =====
    frontend->_communicator->obj_ptr()->Read(reinterpret_cast<char *>(&server_exec_sec),
                                             sizeof(server_exec_sec));

    // ===== receive output buffer =====
    size_t out_buffer_size = 0;
    frontend->_communicator->obj_ptr()->Read((char *)&out_buffer_size, sizeof(size_t));
    frontend->mDataReceived += out_buffer_size;
    LOG4CPLUS_DEBUG(logger, "Read " << out_buffer_size << " bytes from the buffer");
    if (out_buffer_size > 0) {
        LOG4CPLUS_DEBUG(logger, "Output buffer size is greater than 0, reading...");
        frontend->mpOutputBuffer->Read<char>(frontend->_communicator->obj_ptr().get(),
                                             out_buffer_size);
    }
    recv_sec = duration_cast<milliseconds>(steady_clock::now() - start_recv).count() / 1000.0;

    // ===== update info =====
    frontend->mRoutineExecutionTime += server_exec_sec;
    frontend->mSendingTime += send_sec;
    frontend->mReceivingTime += recv_sec;

    // ===== print log =====
    LOG4CPLUS_DEBUG(logger, "Routine '" << routine << "' returned " << exit_code
                                        << " | server_exec=" << server_exec_sec << "s"
                                        << " | send=" << send_sec << "s"
                                        << " | recv=" << recv_sec << "s"
                                        << " | in=" << in_size << "B"
                                        << " | out=" << out_buffer_size << "B"
                                        << " | pid=" << pid << " tid=" << tid);

    LOG4CPLUS_DEBUG(logger, "DEBUG - Called: " << routine);

    // ===== stop this call，clean HybridCommunicator status =====
    if (frontend->_communicator->obj_ptr()->to_string() == "hybridcommunicator") {
        auto hybrid = std::dynamic_pointer_cast<gvirtus::communicators::HybridCommunicator>(
            frontend->_communicator->obj_ptr());
        if (hybrid) {
            hybrid->end_call();
        }
    }

    std::cout << "[GVIRTUS] Routine '" << routine << "' executed with exit code " << exit_code
              << " in " << server_exec_sec << " second(s)\n";
}

void Frontend::Execute_Async(const char *routine, const Buffer *input_buffer, void* stream) {
    if (input_buffer == nullptr) input_buffer = mpInputBuffer.get();
    std::cout << "Execute_Async Called" << std::endl;
    pid_t tid = syscall(SYS_gettid);
    Frontend *frontend = nullptr;
    {
        std::lock_guard<std::mutex> lock(gFrontendMutex);
        auto it = mpFrontends->find(tid);
        if (it == mpFrontends->end()) {
            LOG4CPLUS_ERROR(logger, "Cannot send any job request");
            return;
        }
        frontend = it->second;
    }

    if (frontend->_communicator->obj_ptr()->to_string() != "quiccommunicator") {
        Execute(routine, input_buffer);
        return;
    }

    std::shared_ptr<Buffer> queued_input = std::make_shared<Buffer>(*input_buffer);
    auto job = std::make_shared<AsyncJob>(std::string(routine), queued_input);
    std::shared_ptr<AsyncStreamContext> context;
    {
        std::lock_guard<std::mutex> lock(asyncOutputBuffersMutex);
        auto it = mpAsyncOutputBuffers.find(stream);
        if (it == mpAsyncOutputBuffers.end()) {
            LOG4CPLUS_ERROR(logger, "Execute_Async called on an unknown stream");
            return;
        }
        context = it->second;
    }

    {
        std::lock_guard<std::mutex> lock(context->mutex);
        if (context->stop_requested) {
            LOG4CPLUS_ERROR(logger, "Execute_Async called after Stop_Stream on stream");
            return;
        }
        context->queue.push(job);
    }
    context->cv.notify_one();
    frontend->mRoutinesExecuted++;
}

void Frontend::Execute_Async_Wait(const char *routine, const Buffer *input_buffer, void* stream) {
    if (input_buffer == nullptr) input_buffer = mpInputBuffer.get();

    pid_t tid = syscall(SYS_gettid);
    Frontend *frontend = nullptr;
    {
        std::lock_guard<std::mutex> lock(gFrontendMutex);
        auto it = mpFrontends->find(tid);
        if (it == mpFrontends->end()) {
            LOG4CPLUS_ERROR(logger, "Cannot send any job request");
            return;
        }
        frontend = it->second;
    }

    if (frontend->_communicator->obj_ptr()->to_string() != "quiccommunicator") {
        Execute(routine, input_buffer);
        return;
    }

    std::shared_ptr<Buffer> queued_input = std::make_shared<Buffer>(*input_buffer);
    auto job = std::make_shared<AsyncJob>(std::string(routine), queued_input);
    std::shared_ptr<AsyncStreamContext> context;
    {
        std::lock_guard<std::mutex> lock(asyncOutputBuffersMutex);
        auto it = mpAsyncOutputBuffers.find(stream);
        if (it == mpAsyncOutputBuffers.end()) {
            LOG4CPLUS_ERROR(logger, "Execute_Async_Wait called on an unknown stream");
            return;
        }
        context = it->second;
    }

    {
        std::lock_guard<std::mutex> lock(context->mutex);
        if (context->stop_requested) {
            LOG4CPLUS_ERROR(logger, "Execute_Async_Wait called after Stop_Stream on stream");
            return;
        }
        context->queue.push(job);
    }
    context->cv.notify_one();
    frontend->mRoutinesExecuted++;
    job->future.get();
}

void Frontend::Execute_Detached(void *stream, Frontend* frontend) {
    std::shared_ptr<AsyncStreamContext> context;
    {
        std::lock_guard<std::mutex> lock(asyncOutputBuffersMutex);
        auto it = mpAsyncOutputBuffers.find(stream);
        if (it == mpAsyncOutputBuffers.end()) {
            return;
        }
        context = it->second;
    }

    while (true) {
        std::shared_ptr<AsyncJob> job;
        {
            std::unique_lock<std::mutex> lock(context->mutex);
            context->cv.wait(lock, [context] {
                return context->stop_requested || !context->queue.empty();
            });
            if (context->queue.empty() && context->stop_requested) {
                break;
            }
            job = context->queue.front();
            context->queue.pop();
        }

        const std::string &routine = job->routine;
        std::cout << "Processing async routine '" << routine << "' [pid=" << getpid()
                  << ", tid=" << syscall(SYS_gettid) << "]\n";
        std::shared_ptr<Buffer> input_buffer = job->input_buffer;
        size_t in_size = input_buffer->GetBufferSize();
        int exit_code = 0;
        double server_exec_sec = 0.0;
        double send_sec = 0.0;
        double recv_sec = 0.0;

        try {
            std::cout << "Executing asynchonously routine '" << routine << "' [pid=" << getpid()
                      << ", tid=" << syscall(SYS_gettid) << "]\n";

            auto start_send = steady_clock::now();
            frontend->_communicator->obj_ptr()->Write_Async(routine.c_str(), routine.size() + 1, stream);
            frontend->mDataSent += in_size;
            input_buffer->Dump_Async(frontend->_communicator->obj_ptr().get(), stream);
            send_sec = duration_cast<milliseconds>(steady_clock::now() - start_send).count() / 1000.0;

            frontend->mpOutputBuffer->Reset();
            auto start_recv = steady_clock::now();
            frontend->_communicator->obj_ptr()->Read_Async((char *)&exit_code, sizeof(int), stream);
            frontend->mExitCode = exit_code;
            frontend->_communicator->obj_ptr()->Read_Async(reinterpret_cast<char *>(&server_exec_sec),
                                                          sizeof(server_exec_sec), stream);

            size_t out_buffer_size = 0;
            frontend->_communicator->obj_ptr()->Read_Async((char *)&out_buffer_size, sizeof(size_t), stream);
            frontend->mDataReceived += out_buffer_size;

            std::cout << "Received output buffer of size " << out_buffer_size << " bytes\n";
            recv_sec = duration_cast<milliseconds>(steady_clock::now() - start_recv).count() / 1000.0;

            frontend->mRoutineExecutionTime += server_exec_sec;
            frontend->mSendingTime += send_sec;
            frontend->mReceivingTime += recv_sec;
            job->promise.set_value();
        } catch (...) {
            try {
                job->promise.set_exception(std::current_exception());
            } catch (...) {
                // If promise is already satisfied or cannot be set, ignore.
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(asyncOutputBuffersMutex);
        mpAsyncOutputBuffers.erase(stream);
    }
}


void Frontend::Start_Stream(void* stream) {
    if (this->_communicator->obj_ptr()->to_string() == "quiccommunicator") {
        this->_communicator->obj_ptr()->Start_Stream(stream);

        std::shared_ptr<AsyncStreamContext> context = std::make_shared<AsyncStreamContext>();
        {
            std::lock_guard<std::mutex> lock(asyncOutputBuffersMutex);
            mpAsyncOutputBuffers[stream] = context;
        }

        pid_t tid = syscall(SYS_gettid);
        Frontend *frontend = nullptr;
        {
            std::lock_guard<std::mutex> lock(gFrontendMutex);
            auto it = mpFrontends->find(tid);
            if (it == mpFrontends->end()) {
                LOG4CPLUS_ERROR(logger, "Cannot send any job request");
                return;
            }
            frontend = it->second;
        }

        std::thread(Execute_Detached, stream, frontend).detach();
    }
}

void Frontend::Stop_Stream(void* stream) {
    if (this->_communicator->obj_ptr()->to_string() == "quiccommunicator") {
        std::shared_ptr<AsyncStreamContext> context;
        {
            std::lock_guard<std::mutex> lock(asyncOutputBuffersMutex);
            auto it = mpAsyncOutputBuffers.find(stream);
            if (it != mpAsyncOutputBuffers.end()) {
                context = it->second;
            }
        }
        if (context) {
            {
                std::lock_guard<std::mutex> lock(context->mutex);
                context->stop_requested = true;
            }
            context->cv.notify_one();
        }
    }
}

void Frontend::Prepare() {
    pid_t tid = syscall(SYS_gettid);
    {
        // Hold the mutex while reading mpFrontends to prevent a data race with
        // GetFrontend() calls from other threads (e.g. OpenPose worker threads)
        // that may be inserting a new entry for their own tid at the same time.
        std::lock_guard<std::mutex> lock(gFrontendMutex);
        if (mpFrontends->find(tid) != mpFrontends->end())
            mpFrontends->find(tid)->second->mpInputBuffer->Reset();
    }
}

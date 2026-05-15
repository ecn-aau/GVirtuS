#pragma once

#include <pthread.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>
#include <map>
#include <string>
#include <vector>

#include <gvirtus/common/LD_Lib.h>
#include <gvirtus/communicators/Buffer.h>
#include <gvirtus/communicators/Communicator.h>
#include <gvirtus/communicators/CommunicatorFactory.h>
#include <gvirtus/communicators/EndpointFactory.h>
#include <gvirtus/common/Property.h>

//using gvirtus::communicators::Communicator;
//using gvirtus::communicators::CommunicatorFactory;
//using gvirtus::communicators::EndpointFactory;

namespace gvirtus::tests {

class CommunicatorTest {
    public:
        CommunicatorTest(void);
        void Listen(void);
        void Accept(void);
        void Connect(void);
        void Read(void);
        void Close(void);
        void Write(void) const;

    private:
        communicators::Communicator *c;
        std::shared_ptr<common::LD_Lib<communicators::Communicator,
                                 std::shared_ptr<communicators::Endpoint>>>
      _communicator;
        std::shared_ptr<communicators::Buffer> mpInputBuffer;
        std::shared_ptr<communicators::Buffer> mpOutputBuffer;
        std::shared_ptr<communicators::Buffer> mpLaunchBuffer;
        common::Property _properties;
        int array_size;

};

}
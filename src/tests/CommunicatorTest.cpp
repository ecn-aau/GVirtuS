#include <gvirtus/tests/CommunicatorTest.h>

using gvirtus::tests::CommunicatorTest;

// create class

    CommunicatorTest::CommunicatorTest(void) {
        c=NULL;
        const std::string config_path="../etc/properties.json";
        const fs::path &path = config_path;
        _properties = common::JSON<gvirtus::common::Property>(config_path).parser();
        _communicator = communicators::CommunicatorFactory::get_communicator(communicators::EndpointFactory::get_endpoint(config_path.c_str()), _properties.secure());
        array_size = 4000000;
    }

    void CommunicatorTest::Listen(void) {
        _communicator.get()->obj_ptr()->Serve();
    }

    void CommunicatorTest::Accept(void) {
        c=const_cast<communicators::Communicator *> (_communicator.get()->obj_ptr()->Accept());
    }

    void CommunicatorTest::Connect(void) {
        _communicator.get()->obj_ptr()->Connect();
    }

    void CommunicatorTest::Write(void) const {
        char *dynamic_const_array = (char*)malloc(array_size * sizeof(char));
    
        if (dynamic_const_array != NULL) {
        // Initialize the array
        for (int i = 0; i < array_size - 1; i++) {
            dynamic_const_array[i] = 'A' + (i % 26);
        }
        dynamic_const_array[array_size - 1] = '\0';

        if (c==NULL)  {
            _communicator.get()->obj_ptr()->Write(dynamic_const_array, array_size);
            _communicator.get()->obj_ptr()->Sync();
        }
        else {
            c->Write(dynamic_const_array, array_size);
            c->Sync();
        }
    }
}

    void CommunicatorTest::Read(void) {
        char * str = (char *) calloc(array_size, sizeof(char));
        if (c==NULL) {
            printf("Read content server: %s",str);
            _communicator.get()->obj_ptr()->Read(str, array_size);
        }
        else {
            printf("Read content client: %s",str);
            c->Read(str, array_size);
        }   
            
        printf("Read content: %s",str);
        free (str);
    }

    void CommunicatorTest::Close(void) {
        if (c==NULL) {
            _communicator.get()->obj_ptr()->Close();
        }
        else {
            c->Close();
        }   
    }

int main (int argc, char **argv)
{
    
    try {
        CommunicatorTest ct;
        printf("%i", argc);
        if (argc==2)
        {
            printf("%c",argv[1][0]);
            if (argv[1][0]=='s')
            {
                printf("Server ...\n");
                printf("Listen ...\n");
                ct.Listen();
                printf("Accept ...\n");
                ct.Accept();
                printf("Read ...\n");
                ct.Read();
                sleep(15);
                printf("Write ...\n");
                ct.Write();
                sleep(15);
                ct.Close();
            } 
            else if (argv[1][0]=='c')
            {
            printf("Client ...");
            printf("Connect ...\n");
            ct.Connect();
            
            printf("Write ...\n");
            ct.Write();
            sleep(15);
            printf("Read ...\n");
            ct.Read();
            sleep(15);
            ct.Close();
            }
            else{
                printf("Use ...");
            }
        }
        else{
            printf("Use ...");
        }
    }
    catch(const std::string& e) {
        std::cout << "Caught exception: " << e << std::endl;
    } catch (...) {
        // Catch any other exceptions
        std::cout << "Caught an unknown exception." << std::endl;
    }
    
    
    return 0;
}

// add communicator

// read and write data


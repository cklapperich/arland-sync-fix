#include <iostream>
#include <dlfcn.h>

// Function pointer type for our exported function
typedef void (*log_message_func)(const char*);

int main() {
    std::cout << "Loading library..." << std::endl;
    
    // Load the shared library
    void* handle = dlopen("./liblogger.so", RTLD_LAZY);
    if (!handle) {
        std::cerr << "Failed to load library: " << dlerror() << std::endl;
        return 1;
    }
    
    std::cout << "Library loaded! (check dll_log.txt)" << std::endl;
    
    // Get the exported function
    log_message_func log_msg = (log_message_func)dlsym(handle, "log_message");
    if (!log_msg) {
        std::cerr << "Failed to find log_message function: " << dlerror() << std::endl;
        dlclose(handle);
        return 1;
    }
    
    // Call the function
    std::cout << "Calling log_message function..." << std::endl;
    log_msg("Hello from test program!");
    
    // Unload the library
    std::cout << "Unloading library..." << std::endl;
    dlclose(handle);
    
    std::cout << "Done! Check dll_log.txt for output" << std::endl;
    return 0;
}
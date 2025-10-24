#include <fstream>
#include <ctime>
#include <string>

// Define export macro for cross-platform
#ifdef _WIN32
    #define EXPORT __declspec(dllexport)
#else
    #define EXPORT __attribute__((visibility("default")))
#endif

// Constructor - called when library loads
__attribute__((constructor))
void on_load() {
    std::ofstream logfile("dll_log.txt", std::ios::app);
    
    time_t now = time(0);
    char* dt = ctime(&now);
    
    logfile << "=== DLL Loaded ===" << std::endl;
    logfile << "Timestamp: " << dt;
    logfile << std::endl;
    
    logfile.close();
}

// Destructor - called when library unloads
__attribute__((destructor))
void on_unload() {
    std::ofstream logfile("dll_log.txt", std::ios::app);
    
    logfile << "=== DLL Unloaded ===" << std::endl;
    logfile << std::endl;
    
    logfile.close();
}

// Example exported function
extern "C" EXPORT void log_message(const char* message) {
    std::ofstream logfile("dll_log.txt", std::ios::app);
    logfile << "Message: " << message << std::endl;
    logfile.close();
}
// SingleThreadedJSONRPCStdioSubprocess.cpp
// #include "SingleThreadedJSONRPCStdioSubprocess.h"
#include "mcp.hpp"

#include <unistd.h>     // pipe, fork, dup2, close, execvp
#include <sys/wait.h>   // waitpid
#include <vector>
#include <stdexcept>
#include <cstdio>       // fdopen, fprintf, fgets, fclose
#include <cstring>      // strerror
#include <cerrno>       // errno

using json = nlohmann::ordered_json;

class SingleThreadedJSONRPCStdioSubprocessImpl
    : public SingleThreadedJSONRPCStdioSubprocess
{
public:
    SingleThreadedJSONRPCStdioSubprocessImpl(int writeFd, int readFd, pid_t pid)
        : m_writeFd(writeFd), m_readFd(readFd), m_pid(pid)
    {
        // Wrap FDs in FILE* for buffered IO
        m_writeFile = fdopen(m_writeFd, "w");
        if (!m_writeFile) {
            throw std::runtime_error("fdopen for writeFd failed");
        }
        m_readFile = fdopen(m_readFd, "r");
        if (!m_readFile) {
            fclose(m_writeFile);
            throw std::runtime_error("fdopen for readFd failed");
        }

        // Use full buffering instead of line buffering
        setvbuf(m_writeFile, nullptr, _IOFBF, 4096);
        setvbuf(m_readFile, nullptr, _IOFBF, 4096);
    }
    
    nlohmann::ordered_json call(const std::string & methodName, const nlohmann::ordered_json & arguments) override
    {
        json request = {
            {"jsonrpc", "2.0"},
            {"method",  methodName},
            {"params",  arguments},
            {"id",      nextId++}
        };

        auto requestStr = request.dump();
        fprintf(stderr, "Parent: Sending request: %s\n", requestStr.c_str());
        
        // Make sure we're in a good state
        if (ferror(m_writeFile) || ferror(m_readFile)) {
            fprintf(stderr, "Parent: File error before write\n");
            throw std::runtime_error("File error before write");
        }
        
        // Write request
        if (fprintf(m_writeFile, "%s\n", requestStr.c_str()) < 0) {
            fprintf(stderr, "Parent: Write failed: %s\n", strerror(errno));
            throw std::runtime_error("Write failed");
        }
        
        // Flush output
        if (fflush(m_writeFile) != 0) {
            fprintf(stderr, "Parent: Flush failed: %s\n", strerror(errno));
            throw std::runtime_error("Flush failed");
        }

        fprintf(stderr, "Parent: Write complete, waiting for response...\n");

        // Read response with timeout
        char buffer[4096];
        fd_set readfds;
        struct timeval tv;
        
        // Set up the fd_set for select
        FD_ZERO(&readfds);
        FD_SET(m_readFd, &readfds);
        
        // Set timeout to 5 seconds
        tv.tv_sec = 5;
        tv.tv_usec = 0;
        
        // Wait for data to be available
        int ready = select(m_readFd + 1, &readfds, NULL, NULL, &tv);
        if (ready < 0) {
            fprintf(stderr, "Parent: Select error: %s\n", strerror(errno));
            throw std::runtime_error("Select failed");
        } else if (ready == 0) {
            fprintf(stderr, "Parent: Read timeout after 5 seconds\n");
            throw std::runtime_error("Read timeout");
        }
        
        // Data is available, try to read it
        if (!fgets(buffer, sizeof(buffer), m_readFile)) {
            if (feof(m_readFile)) {
                fprintf(stderr, "Parent: EOF while reading response\n");
                throw std::runtime_error("EOF while reading response");
            } else {
                fprintf(stderr, "Parent: Read error: %s\n", strerror(errno));
                throw std::runtime_error("Read error");
            }
        }

        fprintf(stderr, "Parent: Got response: %s\n", buffer);
        return json::parse(buffer);
    }

private:
    int   m_writeFd;
    int   m_readFd;
    pid_t m_pid;
    FILE* m_writeFile;
    FILE* m_readFile;
    int   nextId = 1;
};
#include <cerrno>
#include <cstring>

std::unique_ptr<SingleThreadedJSONRPCStdioSubprocess>
SingleThreadedJSONRPCStdioSubprocess::create(
    const std::string & program,
    const std::vector<std::string> & args)
{
    // Debug print at start
    fprintf(stderr, "Starting subprocess creation for program: %s\n", program.c_str());

    int toChild[2];
    int fromChild[2];
    
    if (pipe(toChild) != 0) {
        fprintf(stderr, "Failed to create toChild pipe: %s\n", strerror(errno));
        throw std::runtime_error("Failed to create pipe for child stdin");
    }
    fprintf(stderr, "Created toChild pipe: read=%d, write=%d\n", toChild[0], toChild[1]);

    if (pipe(fromChild) != 0) {
        fprintf(stderr, "Failed to create fromChild pipe: %s\n", strerror(errno));
        close(toChild[0]); close(toChild[1]);
        throw std::runtime_error("Failed to create pipe for child stdout");
    }
    fprintf(stderr, "Created fromChild pipe: read=%d, write=%d\n", fromChild[0], fromChild[1]);

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "Fork failed: %s\n", strerror(errno));
        close(toChild[0]);   close(toChild[1]);
        close(fromChild[0]); close(fromChild[1]);
        throw std::runtime_error("fork() failed");
    }
    else if (pid == 0) {
        // Child process
        fprintf(stderr, "Child process started (pid=%d)\n", getpid());

        if (dup2(toChild[0], STDIN_FILENO) == -1) {
            fprintf(stderr, "Child: dup2 failed for stdin: %s\n", strerror(errno));
            _exit(1);
        }
        if (dup2(fromChild[1], STDOUT_FILENO) == -1) {
            fprintf(stderr, "Child: dup2 failed for stdout: %s\n", strerror(errno));
            _exit(1);
        }

        // Close unused pipe ends
        close(toChild[1]);
        close(fromChild[0]);

        // Convert args to argv
        std::vector<char*> argv;
        argv.reserve(args.size() + 2);
        argv.push_back(const_cast<char*>(program.c_str()));
        for (auto & arg : args) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);

        fprintf(stderr, "Child: about to exec: %s\n", program.c_str());
        for (int i = 0; argv[i] != nullptr; i++) {
            fprintf(stderr, "  arg[%d]: %s\n", i, argv[i]);
        }

        execvp(argv[0], argv.data());
        
        // If we get here, exec failed
        fprintf(stderr, "Child: execvp failed: %s\n", strerror(errno));
        _exit(127);
    }
    else {
        // Parent process
        fprintf(stderr, "Parent: child pid is %d\n", pid);

        // Close unused pipe ends
        close(toChild[0]);
        close(fromChild[1]);

        fprintf(stderr, "Parent: creating implementation with write=%d, read=%d\n", 
                toChild[1], fromChild[0]);

        // Return new instance
        return std::unique_ptr<SingleThreadedJSONRPCStdioSubprocess>(
            new SingleThreadedJSONRPCStdioSubprocessImpl(toChild[1], fromChild[0], pid)
        );
    }
}

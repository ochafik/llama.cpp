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
        // Wrap FDs in FILE* for simpler line-based IO
        m_writeFile = fdopen(m_writeFd, "w");
        if (!m_writeFile) {
            throw std::runtime_error("fdopen for writeFd failed");
        }
        m_readFile = fdopen(m_readFd, "r");
        if (!m_readFile) {
            fclose(m_writeFile);
            throw std::runtime_error("fdopen for readFd failed");
        }

        // // Optional: enable line buffering
        // setvbuf(m_writeFile, nullptr, _IOLBF, 0);
        // setvbuf(m_readFile,  nullptr, _IOLBF, 0);

        // Use full buffering instead of line buffering
        setvbuf(m_writeFile, nullptr, _IOFBF, 4096);
        setvbuf(m_readFile, nullptr, _IOFBF, 4096);
    }

    ~SingleThreadedJSONRPCStdioSubprocessImpl() override
    {
        if (m_writeFile) {
            fclose(m_writeFile);
            m_writeFile = nullptr;
        }
        if (m_readFile) {
            fclose(m_readFile);
            m_readFile = nullptr;
        }

        // Optionally wait for child or send a signal, depending on your needs:
        if (m_pid > 0) {
            int status;
            waitpid(m_pid, &status, 0);
        }
    }

    nlohmann::ordered_json call(const std::string & methodName, const nlohmann::ordered_json & arguments) override
    {
        // Build a minimal JSON-RPC request
        json request = {
            {"jsonrpc", "2.0"},
            {"method",  methodName},
            {"params",  arguments},
            {"id",      nextId++}
        };

        // Write request as a single line to child's stdin
        auto requestStr = request.dump();
        fprintf(stderr, "Sending request: %s\n", requestStr.c_str());
        if (fprintf(m_writeFile, "%s\n", requestStr.c_str()) < 0) {
            throw std::runtime_error("Failed to write to child stdin");
        }
        fflush(m_writeFile);

        // Read a single line as the response
        char buffer[4096];
        if (!fgets(buffer, sizeof(buffer), m_readFile)) {
            throw std::runtime_error("Failed to read child stdout line");
        }

        // Parse into a JSON object
        json response = json::parse(buffer);
        return response;
    }

private:
    int   m_writeFd;
    int   m_readFd;
    pid_t m_pid;
    FILE* m_writeFile;
    FILE* m_readFile;
    int   nextId = 1;
};

std::unique_ptr<SingleThreadedJSONRPCStdioSubprocess>
SingleThreadedJSONRPCStdioSubprocess::create(
    const std::string & program,
    const std::vector<std::string> & args)
{
    // We'll need two pipes: one for parent->child (stdin), one for child->parent (stdout)
    int toChild[2];
    int fromChild[2];
    if (pipe(toChild) != 0) {
        throw std::runtime_error("Failed to create pipe for child stdin");
    }
    if (pipe(fromChild) != 0) {
        close(toChild[0]); close(toChild[1]);
        throw std::runtime_error("Failed to create pipe for child stdout");
    }

    pid_t pid = fork();
    if (pid < 0) {
        // Fork failed
        close(toChild[0]);   close(toChild[1]);
        close(fromChild[0]); close(fromChild[1]);
        throw std::runtime_error("fork() failed");
    }
    else if (pid == 0) {
        // In child process
        // Hook up child's stdin to read end of toChild
        dup2(toChild[0], STDIN_FILENO);
        // Hook up child's stdout to write end of fromChild
        dup2(fromChild[1], STDOUT_FILENO);

        // Close unused FDs
        close(toChild[1]);
        close(fromChild[0]);

        // Convert std::vector<std::string> to the proper argv form
        std::vector<char*> argv;
        argv.reserve(args.size() + 2); // +1 for program +1 for null terminator
        argv.push_back(const_cast<char*>(program.c_str()));
        for (auto & arg : args) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);

        // Replace child process image
        execvp(argv[0], argv.data());

        // If execvp returns, there was an error
        _exit(127);
    }
    else {
        // In parent process
        // Close unused FDs
        close(toChild[0]);
        close(fromChild[1]);

        // Return a new instance
        return std::unique_ptr<SingleThreadedJSONRPCStdioSubprocess>(
            new SingleThreadedJSONRPCStdioSubprocessImpl(toChild[1], fromChild[0], pid)
        );
    }
}

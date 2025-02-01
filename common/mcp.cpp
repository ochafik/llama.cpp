#include "mcp.hpp"
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <vector>
#include <stdexcept>
#include <cstdio>

using json = nlohmann::ordered_json;

class SingleThreadedJSONRPCStdioSubprocessImpl : public SingleThreadedJSONRPCStdioSubprocess {
public:
    SingleThreadedJSONRPCStdioSubprocessImpl(int writeFd, int readFd, pid_t pid)
        : m_writeFile(fdopen(writeFd, "w")), m_readFile(fdopen(readFd, "r")), m_pid(pid) {
        if (!m_writeFile || !m_readFile) {
            if (m_writeFile) fclose(m_writeFile);
            throw std::runtime_error("Failed to open pipes");
        }
    }

    ~SingleThreadedJSONRPCStdioSubprocessImpl() {
        if (m_writeFile) fclose(m_writeFile);
        if (m_readFile) fclose(m_readFile);
        if (m_pid > 0) {
            kill(m_pid, SIGTERM);
            int status;
            waitpid(m_pid, &status, 0);
        }
    }
    
    nlohmann::ordered_json call(const std::string& methodName, const nlohmann::ordered_json& arguments) override {
        json request = {
            {"jsonrpc", "2.0"},
            {"method", methodName},
            {"params", arguments},
            {"id", nextId++}
        };

        if (fprintf(m_writeFile, "%s\n", request.dump().c_str()) < 0 || fflush(m_writeFile) != 0) {
            throw std::runtime_error("Write failed");
        }

        char buffer[4096];
        if (!fgets(buffer, sizeof(buffer), m_readFile)) {
            throw std::runtime_error("Read failed");
        }

        return json::parse(buffer);
    }

private:
    FILE* m_writeFile;
    FILE* m_readFile;
    pid_t m_pid;
    int nextId = 1;
};

std::unique_ptr<SingleThreadedJSONRPCStdioSubprocess>
SingleThreadedJSONRPCStdioSubprocess::create(const std::string& program, const std::vector<std::string>& args) {
    int toChild[2], fromChild[2];
    
    if (pipe(toChild) != 0 || pipe(fromChild) != 0) {
        throw std::runtime_error("Failed to create pipes");
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(toChild[0]); close(toChild[1]);
        close(fromChild[0]); close(fromChild[1]);
        throw std::runtime_error("Fork failed");
    }
    
    if (pid == 0) {
        if (dup2(toChild[0], STDIN_FILENO) == -1 || dup2(fromChild[1], STDOUT_FILENO) == -1) {
            _exit(1);
        }
        close(toChild[1]); close(fromChild[0]);

        std::vector<char*> argv;
        argv.reserve(args.size() + 2);
        argv.push_back(const_cast<char*>(program.c_str()));
        for (const auto& arg : args) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);

        execvp(argv[0], argv.data());
        _exit(127);
    }

    close(toChild[0]); close(fromChild[1]);
    return std::make_unique<SingleThreadedJSONRPCStdioSubprocessImpl>(toChild[1], fromChild[0], pid);
}

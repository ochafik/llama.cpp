#include "mcp.hpp"
#include <iostream>

int main()
{
    // Suppose you have a little Python script that reads lines of JSON and echoes them back
    //   e.g. python test_server.py
    // that does JSON-RPC style responses on stdout.
    auto subprocess = SingleThreadedJSONRPCStdioSubprocess::create(
      "/opt/homebrew/Caskroom/miniforge/base/envs/ai/bin/python",
      // "python",
      { "../test_server.py" });

    // Make a call
    auto result = subprocess->call("myMethod", {{"param1", 42}, {"param2", "hello"}});
    std::cout << "Got response: " << result.dump() << std::endl;
    return 0;
}
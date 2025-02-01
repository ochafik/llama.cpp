#include "json.hpp"

class SingleThreadedJSONRPCStdioSubprocess {
public:
  virtual ~SingleThreadedJSONRPCStdioSubprocess() = default;
  static std::unique_ptr<SingleThreadedJSONRPCStdioSubprocess> create(const std::string & program, const std::vector<std::string> & args);
  virtual nlohmann::ordered_json call(const std::string & name, const nlohmann::ordered_json & arguments) = 0;
};

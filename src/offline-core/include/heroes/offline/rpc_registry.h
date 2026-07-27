#pragma once

#include "heroes/offline/model.h"

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace heroes::offline {

using RpcBytes = std::vector<std::uint8_t>;
using RpcHandler =
    std::function<RpcBytes(std::span<const std::uint8_t>, PlayerState&)>;

class RpcRegistry {
 public:
  RpcRegistry();

  [[nodiscard]] bool supports(const std::string& service,
                              const std::string& method) const;
  [[nodiscard]] RpcBytes dispatch(
      const std::string& service, const std::string& method,
      std::span<const std::uint8_t> request, PlayerState& state) const;

 private:
  [[nodiscard]] static std::string key(const std::string& service,
                                       const std::string& method);

  std::unordered_map<std::string, RpcHandler> handlers_;
};

[[nodiscard]] RpcBytes make_response_envelope(
    std::span<const std::uint8_t> payload, std::int64_t server_time_seconds,
    std::int32_t response_code = 0, std::string_view message = {});

}  // namespace heroes::offline

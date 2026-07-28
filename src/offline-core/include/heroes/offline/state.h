#pragma once

#include "heroes/offline/model.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace heroes::offline {

inline constexpr char kSaveMagic[] = "HEROES_OFFLINE_SAVE_V1";
inline constexpr std::uint32_t kSchemaVersion = 1;
inline constexpr std::size_t kAiProfileCount = 360;

[[nodiscard]] std::vector<AiProfile> generate_ai_profiles(
    std::uint64_t seed, std::uint32_t generation = 0,
    std::int64_t player_gp = 0);

class StateStore {
 public:
  explicit StateStore(std::filesystem::path directory);

  [[nodiscard]] PlayerState load_or_create(
      const std::string& content_version, std::int64_t initial_utc_seconds,
      std::uint64_t seed);

  void save(const PlayerState& state) const;

  [[nodiscard]] const std::filesystem::path& directory() const noexcept {
    return directory_;
  }

  [[nodiscard]] std::filesystem::path profile_path() const {
    return directory_ / "profile.hos";
  }

 private:
  std::filesystem::path directory_;
};

}  // namespace heroes::offline

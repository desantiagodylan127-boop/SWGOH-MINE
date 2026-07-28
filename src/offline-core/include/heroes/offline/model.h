#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace heroes::offline {

struct Settings {
  double reward_multiplier = 1.0;
  bool guaranteed_drops = false;
  bool tutorial_skipped = false;
};

struct VirtualClock {
  std::int64_t utc_seconds = 0;
  std::uint64_t revision = 0;
};

struct AiProfile {
  std::uint32_t id = 0;
  std::uint64_t roster_seed = 0;
  std::int32_t strength_band = 0;
  std::int64_t galactic_power = 0;
  bool permanent_elite = false;
};

struct ModeState {
  std::string phase;
  std::int64_t phase_started_utc = 0;
  std::uint32_t attempt_count = 0;
  std::uint32_t reset_count = 0;
};

struct PlayerState {
  std::uint32_t schema_version = 1;
  std::string content_version;
  std::string player_id;
  std::string player_name = "Player";
  VirtualClock clock;
  Settings settings;
  std::uint64_t ai_seed = 0;
  std::uint32_t ai_generation = 0;
  std::vector<AiProfile> ai_profiles;
  std::map<std::string, std::int64_t> inventory;
  std::map<std::string, ModeState> modes;
  std::map<std::string, std::int64_t> event_claim_cycle;
};

}  // namespace heroes::offline

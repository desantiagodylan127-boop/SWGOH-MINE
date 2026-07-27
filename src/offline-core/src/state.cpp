#include "heroes/offline/state.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

namespace heroes::offline {
namespace {

constexpr std::uintmax_t kMaximumSaveBytes = 64U * 1024U;

[[noreturn]] void parse_error(std::size_t line, std::string_view message) {
  throw std::runtime_error("Invalid profile.hos at line " +
                           std::to_string(line) + ": " +
                           std::string(message));
}

bool is_safe_raw_line(std::string_view value) {
  if (value.empty()) {
    return false;
  }
  for (const unsigned char character : value) {
    if (character < 0x20 || character == 0x7f) {
      return false;
    }
  }
  return true;
}

void require_safe_raw_line(std::string_view value, std::string_view field) {
  if (!is_safe_raw_line(value)) {
    throw std::invalid_argument(std::string(field) +
                                " must be a non-empty printable line");
  }
}

template <typename Value>
bool parse_one(std::istringstream& input, Value& value) {
  input >> std::ws;
  if constexpr (std::is_unsigned_v<Value>) {
    if (input.peek() == '-') {
      return false;
    }
  }
  return static_cast<bool>(input >> value);
}

template <typename... Values>
void parse_values(const std::string& line, std::size_t line_number,
                  Values&... values) {
  std::istringstream input(line);
  if (!(parse_one(input, values) && ...)) {
    parse_error(line_number, "invalid value");
  }
  input >> std::ws;
  if (!input.eof()) {
    parse_error(line_number, "unexpected trailing data");
  }
}

bool parse_bool(int value, std::size_t line_number) {
  if (value != 0 && value != 1) {
    parse_error(line_number, "boolean must be 0 or 1");
  }
  return value != 0;
}

std::string make_player_id(std::uint64_t seed) {
  std::ostringstream output;
  output << "offline-" << std::hex << seed;
  return output.str();
}

std::string read_required_line(std::ifstream& input, std::size_t line_number) {
  std::string line;
  if (!std::getline(input, line)) {
    parse_error(line_number, "missing line");
  }
  return line;
}

PlayerState decode(const std::filesystem::path& path,
                   const std::string& expected_content_version) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error) {
    throw std::runtime_error("Unable to inspect profile.hos: " +
                             error.message());
  }
  if (size > kMaximumSaveBytes) {
    throw std::runtime_error("Invalid profile.hos: file is too large");
  }

  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("Unable to open profile.hos");
  }

  if (read_required_line(input, 1) != kSaveMagic) {
    parse_error(1, "bad save magic");
  }

  PlayerState state;
  const std::string schema = read_required_line(input, 2);
  parse_values(schema, 2, state.schema_version);

  state.content_version = read_required_line(input, 3);
  state.player_id = read_required_line(input, 4);
  state.player_name = read_required_line(input, 5);
  if (!is_safe_raw_line(state.content_version) ||
      !is_safe_raw_line(state.player_id) ||
      !is_safe_raw_line(state.player_name)) {
    parse_error(3, "invalid raw string field");
  }

  const std::string clock = read_required_line(input, 6);
  parse_values(clock, 6, state.clock.utc_seconds, state.clock.revision);

  const std::string ai = read_required_line(input, 7);
  parse_values(ai, 7, state.ai_seed, state.ai_generation);

  double reward_multiplier = 0.0;
  int guaranteed_drops = 0;
  int tutorial_skipped = 0;
  const std::string settings = read_required_line(input, 8);
  parse_values(settings, 8, reward_multiplier, guaranteed_drops,
               tutorial_skipped);
  if (!std::isfinite(reward_multiplier)) {
    parse_error(8, "reward multiplier must be finite");
  }
  state.settings.reward_multiplier = reward_multiplier;
  state.settings.guaranteed_drops = parse_bool(guaranteed_drops, 8);
  state.settings.tutorial_skipped = parse_bool(tutorial_skipped, 8);

  std::string extra;
  if (std::getline(input, extra)) {
    parse_error(9, "unexpected extra line");
  }
  if (!input.eof()) {
    throw std::runtime_error("Unable to read profile.hos");
  }
  if (state.schema_version != kSchemaVersion) {
    throw std::runtime_error("Unsupported profile schema version");
  }
  if (state.content_version != expected_content_version) {
    throw std::runtime_error("Profile content version mismatch");
  }

  state.ai_profiles =
      generate_ai_profiles(state.ai_seed, state.ai_generation);
  state.inventory.emplace("currency:*", 2'000'000'000LL);
  return state;
}

void rename_if_present(const std::filesystem::path& from,
                       const std::filesystem::path& to) {
  std::error_code error;
  if (!std::filesystem::exists(from, error)) {
    if (error) {
      throw std::runtime_error("Unable to inspect save backup: " +
                               error.message());
    }
    return;
  }
  std::filesystem::remove(to, error);
  if (error) {
    throw std::runtime_error("Unable to replace save backup: " +
                             error.message());
  }
  std::filesystem::rename(from, to, error);
  if (error) {
    throw std::runtime_error("Unable to rotate save backup: " +
                             error.message());
  }
}

}  // namespace

std::vector<AiProfile> generate_ai_profiles(std::uint64_t seed,
                                            std::uint32_t generation,
                                            std::int64_t player_gp) {
  std::seed_seq sequence{
      static_cast<std::uint32_t>(seed),
      static_cast<std::uint32_t>(seed >> 32U),
      generation,
      0x484f4646U,
  };
  std::mt19937_64 random(sequence);
  const std::int64_t baseline = std::max<std::int64_t>(player_gp, 50'000);

  std::vector<AiProfile> profiles;
  profiles.reserve(kAiProfileCount);
  for (std::uint32_t index = 0; index < kAiProfileCount; ++index) {
    const bool elite = index > 339U;
    const std::int32_t band =
        elite ? 3 : static_cast<std::int32_t>(index % 3U) - 1;
    const double factor =
        elite ? 2.0 + static_cast<double>(index % 5U) * 0.15
              : 1.0 + static_cast<double>(band) * 0.25;
    const std::int64_t jitter =
        static_cast<std::int64_t>(random() % 20'001U) - 10'000;

    AiProfile profile;
    profile.id = index + 1U;
    profile.roster_seed = random();
    profile.strength_band = band;
    profile.galactic_power =
        std::max<std::int64_t>(
            10'000, static_cast<std::int64_t>(
                        static_cast<double>(baseline) * factor) +
                        jitter);
    profile.permanent_elite = elite;
    profiles.push_back(profile);
  }
  return profiles;
}

StateStore::StateStore(std::filesystem::path directory)
    : directory_(std::move(directory)) {
  if (directory_.empty()) {
    throw std::invalid_argument("StateStore directory must not be empty");
  }
}

PlayerState StateStore::load_or_create(const std::string& content_version,
                                       std::int64_t initial_utc_seconds,
                                       std::uint64_t seed) {
  require_safe_raw_line(content_version, "content_version");
  std::error_code error;
  const bool exists = std::filesystem::exists(profile_path(), error);
  if (error) {
    throw std::runtime_error("Unable to inspect profile.hos: " +
                             error.message());
  }
  if (exists) {
    return decode(profile_path(), content_version);
  }

  PlayerState state;
  state.content_version = content_version;
  state.player_id = make_player_id(seed);
  state.clock.utc_seconds = initial_utc_seconds;
  state.ai_seed = seed;
  state.ai_profiles = generate_ai_profiles(seed, state.ai_generation);
  state.inventory.emplace("currency:*", 2'000'000'000LL);
  save(state);
  return state;
}

void StateStore::save(const PlayerState& state) const {
  if (state.schema_version != kSchemaVersion) {
    throw std::invalid_argument("Cannot save an unsupported schema version");
  }
  require_safe_raw_line(state.content_version, "content_version");
  require_safe_raw_line(state.player_id, "player_id");
  require_safe_raw_line(state.player_name, "player_name");
  if (!std::isfinite(state.settings.reward_multiplier)) {
    throw std::invalid_argument("reward_multiplier must be finite");
  }

  std::error_code error;
  std::filesystem::create_directories(directory_, error);
  if (error) {
    throw std::runtime_error("Unable to create state directory: " +
                             error.message());
  }

  const auto pending = directory_ / "profile.hos.pending";
  {
    std::ofstream output(pending, std::ios::binary | std::ios::trunc);
    if (!output) {
      throw std::runtime_error("Unable to open pending save");
    }
    output << kSaveMagic << '\n'
           << state.schema_version << '\n'
           << state.content_version << '\n'
           << state.player_id << '\n'
           << state.player_name << '\n'
           << state.clock.utc_seconds << ' ' << state.clock.revision << '\n'
           << state.ai_seed << ' ' << state.ai_generation << '\n'
           << state.settings.reward_multiplier << ' '
           << static_cast<int>(state.settings.guaranteed_drops) << ' '
           << static_cast<int>(state.settings.tutorial_skipped) << '\n';
    output.flush();
    if (!output) {
      output.close();
      std::filesystem::remove(pending, error);
      throw std::runtime_error("Unable to write pending save");
    }
  }

  try {
    rename_if_present(directory_ / "profile.hos.bak2",
                      directory_ / "profile.hos.bak3");
    rename_if_present(directory_ / "profile.hos.bak1",
                      directory_ / "profile.hos.bak2");
    rename_if_present(profile_path(), directory_ / "profile.hos.bak1");
    std::filesystem::rename(pending, profile_path(), error);
    if (error) {
      throw std::runtime_error("Unable to commit pending save: " +
                               error.message());
    }
  } catch (...) {
    std::filesystem::remove(pending, error);
    if (!std::filesystem::exists(profile_path(), error) &&
        std::filesystem::exists(directory_ / "profile.hos.bak1", error)) {
      std::filesystem::rename(directory_ / "profile.hos.bak1", profile_path(),
                              error);
    }
    throw;
  }
}

}  // namespace heroes::offline

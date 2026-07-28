#include "heroes/offline/state.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {

#define ASSERT(condition)       \
  do {                          \
    if (!(condition)) {         \
      return __LINE__;          \
    }                           \
  } while (false)

std::filesystem::path temporary_directory() {
  const auto stamp = std::chrono::steady_clock::now()
                         .time_since_epoch()
                         .count();
  return std::filesystem::temp_directory_path() /
         ("heroes-offline-state-test-" + std::to_string(stamp));
}

std::string read_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

template <typename Function>
bool throws(Function&& function) {
  try {
    function();
  } catch (...) {
    return true;
  }
  return false;
}

}  // namespace

int run_state_tests() {
  namespace fs = std::filesystem;
  using heroes::offline::StateStore;
  using heroes::offline::generate_ai_profiles;
  using heroes::offline::kAiProfileCount;

  const fs::path directory = temporary_directory();
  fs::remove_all(directory);
  StateStore store(directory);

  auto state = store.load_or_create("content-1", 1'700'000'000, 0x12345678U);
  ASSERT(state.schema_version == 1);
  ASSERT(state.player_id == "offline-12345678");
  ASSERT(state.ai_profiles.size() == kAiProfileCount);
  ASSERT(state.inventory.size() == 1);
  ASSERT(state.inventory.at("currency:*") == 2'000'000'000LL);

  const std::string golden =
      "HEROES_OFFLINE_SAVE_V1\n"
      "1\n"
      "content-1\n"
      "offline-12345678\n"
      "Player\n"
      "1700000000 0\n"
      "305419896 0\n"
      "1 0 0\n";
  ASSERT(read_file(directory / "profile.hos") == golden);

  const auto profiles =
      generate_ai_profiles(0x123456789abcdef0ULL, 7, 123'456);
  ASSERT(profiles.size() == 360);
  ASSERT(profiles[0].id == 1);
  ASSERT(profiles[0].roster_seed == 6'590'403'335'669'963'424ULL);
  ASSERT(profiles[0].strength_band == -1);
  ASSERT(profiles[0].galactic_power == 84'121);
  ASSERT(!profiles[0].permanent_elite);
  ASSERT(profiles[1].roster_seed == 9'537'246'474'578'853'764ULL);
  ASSERT(profiles[1].galactic_power == 130'958);
  ASSERT(profiles[339].id == 340);
  ASSERT(!profiles[339].permanent_elite);
  ASSERT(profiles[340].id == 341);
  ASSERT(profiles[340].roster_seed == 18'252'096'640'913'028'124ULL);
  ASSERT(profiles[340].strength_band == 3);
  ASSERT(profiles[340].galactic_power == 241'054);
  ASSERT(profiles[340].permanent_elite);
  ASSERT(profiles[359].roster_seed == 935'446'279'831'775'601ULL);
  ASSERT(profiles[359].galactic_power == 311'998);

  state.player_name = "Player Two";
  state.clock.revision = 9;
  state.settings.reward_multiplier = 2.25;
  state.settings.guaranteed_drops = true;
  state.settings.tutorial_skipped = true;
  state.inventory["transient"] = 12;
  state.modes["arena"] = {"active", 123, 4, 2};
  state.event_claim_cycle["event"] = 55;
  store.save(state);
  ASSERT(fs::exists(directory / "profile.hos.bak1"));

  const auto loaded = store.load_or_create("content-1", 0, 0);
  ASSERT(loaded.player_name == "Player Two");
  ASSERT(loaded.clock.revision == 9);
  ASSERT(loaded.settings.reward_multiplier == 2.25);
  ASSERT(loaded.settings.guaranteed_drops);
  ASSERT(loaded.settings.tutorial_skipped);
  ASSERT(loaded.ai_profiles.size() == 360);
  ASSERT(loaded.inventory.size() == 1);
  ASSERT(loaded.inventory.contains("currency:*"));
  ASSERT(loaded.modes.empty());
  ASSERT(loaded.event_claim_cycle.empty());

  store.save(loaded);
  store.save(loaded);
  ASSERT(fs::exists(directory / "profile.hos.bak1"));
  ASSERT(fs::exists(directory / "profile.hos.bak2"));
  ASSERT(fs::exists(directory / "profile.hos.bak3"));
  ASSERT(!fs::exists(directory / "profile.hos.pending"));

  ASSERT(throws(
      [&] { (void)store.load_or_create("wrong-content", 0, 0); }));

  {
    std::ofstream corrupt(directory / "profile.hos",
                          std::ios::binary | std::ios::app);
    corrupt << "extra\n";
  }
  ASSERT(throws(
      [&] { (void)store.load_or_create("content-1", 0, 0); }));

  fs::remove_all(directory);
  return 0;
}

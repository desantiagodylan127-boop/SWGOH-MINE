#include "heroes/offline/rpc_registry.h"

#include "heroes/offline/protobuf.h"

#include <array>
#include <initializer_list>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace heroes::offline {
namespace {

using protobuf::Bytes;
using protobuf::write_integer;
using protobuf::write_message;
using protobuf::write_string;

constexpr std::uint64_t kCurrencyBalance = 2'000'000'000ULL;

std::uint64_t milliseconds(const PlayerState& state) {
  return state.clock.utc_seconds > 0
             ? static_cast<std::uint64_t>(state.clock.utc_seconds) * 1000U
             : 0U;
}

Bytes skill(std::string_view id, std::uint32_t tier = 1) {
  Bytes output;
  write_string(output, 1, id);
  write_integer(output, 2, tier);
  return output;
}

Bytes inventory_unit(std::string_view id, std::string_view definition_id,
                     std::initializer_list<std::string_view> skills) {
  Bytes output;
  write_string(output, 1, id);
  write_string(output, 2, definition_id);
  write_integer(output, 8, 1);
  write_integer(output, 9, 1);
  write_integer(output, 10, 0);
  for (const auto id : skills) {
    const Bytes value = skill(id);
    write_message(output, 16, value);
  }
  write_integer(output, 19, 1);
  return output;
}

Bytes battle_stat(std::string_view unit_id, std::string_view definition_id,
                  std::initializer_list<std::string_view> skills) {
  Bytes output;
  write_integer(output, 8, 1);
  write_integer(output, 9, 1);
  for (const auto id : skills) {
    const Bytes value = skill(id);
    write_message(output, 11, value);
  }
  write_string(output, 13, unit_id);
  write_string(output, 14, definition_id);
  write_integer(output, 15, 1);
  return output;
}

Bytes squad_cell(std::string_view unit_id, std::string_view definition_id,
                 std::uint32_t index, const Bytes* stat = nullptr) {
  Bytes output;
  write_string(output, 1, unit_id);
  write_string(output, 2, definition_id);
  write_integer(output, 3, 1);
  write_integer(output, 4, index);
  if (stat != nullptr) {
    write_message(output, 6, *stat);
  }
  return output;
}

Bytes squad(std::initializer_list<Bytes> cells, std::uint32_t type) {
  Bytes output;
  for (const auto& cell : cells) {
    write_message(output, 1, cell);
  }
  write_integer(output, 3, 1);
  write_integer(output, 4, type);
  return output;
}

Bytes initial_data(std::span<const std::uint8_t>, PlayerState& state) {
  const Bytes anakin = inventory_unit(
      "offline-anakin", "ANAKINKNIGHT:ONE_STAR",
      {"basicskill_ANAKINKNIGHT", "leaderskill_ANAKINKNIGHT",
       "specialskill_ANAKINKNIGHT01", "uniqueskill_ANAKINKNIGHT01"});
  const Bytes ahsoka = inventory_unit(
      "offline-ahsoka", "AHSOKATANO:ONE_STAR",
      {"basicskill_AHSOKATANO", "leaderskill_AHSOKATANO",
       "specialskill_AHSOKATANO01", "uniqueskill_AHSOKATANO01"});
  const Bytes onboarding_squad =
      squad({squad_cell("offline-anakin", "ANAKINKNIGHT:ONE_STAR", 0),
             squad_cell("offline-ahsoka", "AHSOKATANO:ONE_STAR", 1)},
            7);

  Bytes player;
  if (state.settings.tutorial_skipped) {
    write_integer(player, 1, 1);
  }
  write_integer(player, 2, 99'999'999);
  write_integer(player, 3, 85);
  constexpr std::array<std::uint32_t, 5> energy_types{7, 1, 3, 5, 6};
  for (const std::uint32_t type : energy_types) {
    Bytes energy;
    write_integer(energy, 1, type);
    write_integer(energy, 2, milliseconds(state));
    write_integer(energy, 3, kCurrencyBalance);
    write_integer(energy, 4, 1);
    write_integer(energy, 6, kCurrencyBalance);
    write_message(player, 4, energy);
  }
  const std::string_view player_id =
      state.player_id.empty() ? std::string_view{"offline-player"}
                              : std::string_view{state.player_id};
  write_string(player, 7, state.player_name);
  write_integer(player, 9, 1);
  write_integer(player, 11, 1);
  write_integer(player, 19, 100'000'001);
  write_integer(player, 20, 1);
  write_string(player, 35, "US");
  write_integer(player, 36, milliseconds(state));
  write_integer(player, 46, 1);
  write_integer(player, 50, 1);
  write_integer(player, 52, 1);

  Bytes inventory;
  write_message(inventory, 1, anakin);
  write_message(inventory, 1, ahsoka);
  for (std::uint32_t id = 1; id <= 50; ++id) {
    if (id == 5 || id == 6 || id == 7) {
      continue;
    }
    Bytes currency;
    write_integer(currency, 1, id);
    write_integer(currency, 2, kCurrencyBalance);
    write_message(inventory, 9, currency);
  }

  Bytes output;
  write_string(output, 1, player_id);
  write_message(output, 2, player);
  write_message(output, 5, inventory);
  if (!state.settings.tutorial_skipped) {
    write_message(output, 25, onboarding_squad);
  }
  write_string(output, 51, "offline-identity");
  write_integer(output, 55, 1);
  if (state.settings.tutorial_skipped) {
    write_integer(output, 68, 1);
  }
  return output;
}

Bytes empty_response(std::span<const std::uint8_t>, PlayerState&) { return {}; }

Bytes battle_start(std::span<const std::uint8_t>, PlayerState& state) {
  const Bytes stormtrooper_stat = battle_stat(
      "offline-enemy-stormtrooper", "STORMTROOPER:ONE_STAR",
      {"basicskill_STORMTROOPER01", "specialskill_STORMTROOPER01",
       "uniqueskill_STORMTROOPER01"});
  const Bytes tusken_stat = battle_stat(
      "offline-enemy-tusken", "TUSKENRAIDER:ONE_STAR",
      {"basicskill_TUSKENRAIDER", "specialskill_TUSKENRAIDER01",
       "uniqueskill_TUSKENRAIDER01"});
  const Bytes enemy_squad =
      squad({squad_cell("offline-enemy-stormtrooper",
                        "STORMTROOPER:ONE_STAR", 0, &stormtrooper_stat),
             squad_cell("offline-enemy-tusken", "TUSKENRAIDER:ONE_STAR", 1,
                        &tusken_stat)},
            3);

  Bytes encounter;
  write_message(encounter, 1, enemy_squad);
  write_string(encounter, 3, "1");

  Bytes battle_data;
  write_integer(battle_data, 1,
                static_cast<std::uint32_t>(state.ai_seed) ^
                    state.ai_generation);
  write_message(battle_data, 4, encounter);
  write_integer(battle_data, 9,
                static_cast<std::uint32_t>(state.ai_seed >> 32U) ^
                    0x53474f48U);
  write_integer(battle_data, 10, 0);

  Bytes output;
  write_message(output, 1, battle_data);
  return output;
}

Bytes battle_finish(std::span<const std::uint8_t>, PlayerState& state) {
  state.settings.tutorial_skipped = true;
  Bytes output;
  write_integer(output, 7, 1);
  return output;
}

Bytes auth_guest(std::span<const std::uint8_t>, PlayerState& state) {
  Bytes output;
  write_string(output, 1,
               state.player_id.empty() ? std::string_view{"offline-player"}
                                       : std::string_view{state.player_id});
  write_string(output, 2, "offline-auth-token");
  return output;
}

Bytes session_active(std::span<const std::uint8_t>, PlayerState&) {
  return {0x08, 0x01};
}

Bytes config_entry(std::string_view key, std::string_view value) {
  Bytes output;
  write_string(output, 1, key);
  write_string(output, 2, value);
  return output;
}

Bytes metadata(std::span<const std::uint8_t>, PlayerState& state) {
  Bytes output;
  constexpr std::array<std::pair<std::string_view, std::string_view>, 4>
      config{{
          {"client-asset-manifest-caching-enabled", "true"},
          {"client-available-device-storage-confirmation-enabled", "false"},
          {"push-notification-events-enabled", "false"},
          {"client-local-push-notes-enabled", "false"},
      }};
  for (const auto& [key, value] : config) {
    const Bytes entry = config_entry(key, value);
    write_message(output, 1, entry);
  }
  write_integer(output, 4, 100'042);
  write_string(output, 5, "100042/Android/ETC");
  write_string(output, 6, "ETC");
  write_integer(output, 7, milliseconds(state));
  write_string(output, 9, "F2K9SgKJRpGFeT4C9dqhBQ");
  write_string(output, 10, "0.40.3:Ad8NIoEjSterR5OOmkn9mw");
  write_integer(output, 11, 150'000'000);
  write_string(output, 12, "2054156.xml");
  write_integer(output, 13, 0);
  write_string(output, 18, "2053518.zip");
  write_string(output, 19, "2048857");
  write_string(output, 23, "bUAF-dLTQtSR5wLvcwte6w");
  write_string(output, 25, "offline://assets/");
  return output;
}

Bytes segmented_content(std::span<const std::uint8_t>, PlayerState&) {
  Bytes output;
  const Bytes empty;
  write_message(output, 1, empty);
  return output;
}

}  // namespace

RpcRegistry::RpcRegistry() {
  handlers_.reserve(13);
  handlers_.emplace(key("PlayerRpc", "GetInitialData"), initial_data);
  handlers_.emplace(key("PlayerRpc", "Ping"), empty_response);
  handlers_.emplace(key("PlayerRpc", "TrackFtueStep"), empty_response);
  handlers_.emplace(key("PlayerRpc", "SkipTutorial"), empty_response);
  handlers_.emplace(key("PlayerRpc", "UpdatePlayerPrefs"), empty_response);
  handlers_.emplace(key("PlayerRpc", "SetPlayerPref"), empty_response);
  handlers_.emplace(key("BattleRpc", "BattleStart"), battle_start);
  handlers_.emplace(key("BattleRpc", "BattleFinish"), battle_finish);
  handlers_.emplace(key("ContentRpc", "GetMetadata"), metadata);
  handlers_.emplace(key("ContentRpc", "GetSegmentedContentDetails"),
                    segmented_content);
  handlers_.emplace(key("AuthRpc", "DoAuthGuest"), auth_guest);
  handlers_.emplace(key("AuthRpc", "IsSessionActive"), session_active);
  handlers_.emplace(key("AuthRpc", "DoLogout"), empty_response);
}

bool RpcRegistry::supports(const std::string& service,
                           const std::string& method) const {
  return handlers_.contains(key(service, method));
}

RpcBytes RpcRegistry::dispatch(const std::string& service,
                               const std::string& method,
                               std::span<const std::uint8_t> request,
                               PlayerState& state) const {
  const auto found = handlers_.find(key(service, method));
  if (found == handlers_.end()) {
    throw std::runtime_error("Unsupported offline RPC: " + service + "/" +
                             method);
  }
  return found->second(request, state);
}

std::string RpcRegistry::key(const std::string& service,
                             const std::string& method) {
  return service + '\x1f' + method;
}

RpcBytes make_response_envelope(std::span<const std::uint8_t> payload,
                                std::int64_t server_time_seconds,
                                std::int32_t response_code,
                                std::string_view message) {
  RpcBytes output;
  write_integer(output, 2, static_cast<std::uint64_t>(server_time_seconds));
  if (!payload.empty()) {
    write_message(output, 4, payload);
  }
  write_integer(
      output, 5,
      static_cast<std::uint64_t>(static_cast<std::int64_t>(response_code)));
  if (!message.empty()) {
    write_string(output, 6, message);
  }
  return output;
}

}  // namespace heroes::offline

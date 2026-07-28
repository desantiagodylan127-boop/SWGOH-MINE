#include "heroes/offline/protobuf.h"
#include "heroes/offline/rpc_registry.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using heroes::offline::RpcBytes;

struct Field {
  std::uint32_t number = 0;
  std::uint32_t wire = 0;
  std::uint64_t integer = 0;
  RpcBytes bytes;
};

bool read_varint(std::span<const std::uint8_t> input, std::size_t& offset,
                 std::uint64_t& value) {
  value = 0;
  for (unsigned shift = 0; shift < 64 && offset < input.size(); shift += 7) {
    const std::uint8_t byte = input[offset++];
    value |= static_cast<std::uint64_t>(byte & 0x7fU) << shift;
    if ((byte & 0x80U) == 0) {
      return true;
    }
  }
  return false;
}

bool parse(std::span<const std::uint8_t> input, std::vector<Field>& fields) {
  std::size_t offset = 0;
  while (offset < input.size()) {
    std::uint64_t key = 0;
    if (!read_varint(input, offset, key) || (key >> 3U) == 0) {
      return false;
    }
    Field field;
    field.number = static_cast<std::uint32_t>(key >> 3U);
    field.wire = static_cast<std::uint32_t>(key & 7U);
    if (field.wire == 0) {
      if (!read_varint(input, offset, field.integer)) {
        return false;
      }
    } else if (field.wire == 2) {
      std::uint64_t size = 0;
      if (!read_varint(input, offset, size) ||
          size > input.size() - offset) {
        return false;
      }
      field.bytes.assign(input.begin() + static_cast<std::ptrdiff_t>(offset),
                         input.begin() +
                             static_cast<std::ptrdiff_t>(offset + size));
      offset += static_cast<std::size_t>(size);
    } else {
      return false;
    }
    fields.push_back(std::move(field));
  }
  return true;
}

const Field* first(const std::vector<Field>& fields, std::uint32_t number) {
  const auto found =
      std::find_if(fields.begin(), fields.end(), [number](const Field& field) {
        return field.number == number;
      });
  return found == fields.end() ? nullptr : &*found;
}

std::string as_string(const Field& field) {
  return {field.bytes.begin(), field.bytes.end()};
}

bool contains_text(std::span<const std::uint8_t> bytes,
                   std::string_view text) {
  return std::search(bytes.begin(), bytes.end(), text.begin(), text.end()) !=
         bytes.end();
}

std::string hex(std::span<const std::uint8_t> bytes) {
  constexpr char digits[] = "0123456789abcdef";
  std::string output;
  output.reserve(bytes.size() * 2);
  for (const std::uint8_t byte : bytes) {
    output.push_back(digits[byte >> 4U]);
    output.push_back(digits[byte & 0x0fU]);
  }
  return output;
}

}  // namespace

int run_rpc_tests() {
  int failures = 0;
  const auto check = [&failures](bool condition) {
    if (!condition) {
      ++failures;
    }
  };

  {
    heroes::offline::protobuf::Bytes output;
    heroes::offline::protobuf::write_varint(output, 300);
    check(output == heroes::offline::protobuf::Bytes({0xac, 0x02}));

    output.clear();
    heroes::offline::protobuf::write_integer(output, 1, 150);
    heroes::offline::protobuf::write_string(output, 2, "hi");
    const heroes::offline::protobuf::Bytes child{0x08, 0x01};
    heroes::offline::protobuf::write_message(output, 3, child);
    check(output == heroes::offline::protobuf::Bytes(
                        {0x08, 0x96, 0x01, 0x12, 0x02, 'h', 'i', 0x1a,
                         0x02, 0x08, 0x01}));
  }

  heroes::offline::RpcRegistry registry;
  heroes::offline::PlayerState state;
  state.player_id = "local-profile";
  state.player_name = "Recovered Player";
  state.clock.utc_seconds = 1'700'000'000;
  state.ai_seed = 0x1234'5678'9abc'def0ULL;
  state.ai_generation = 9;
  const std::span<const std::uint8_t> request;

  constexpr std::array<std::pair<std::string_view, std::string_view>, 13>
      supported{{
          {"PlayerRpc", "GetInitialData"},
          {"PlayerRpc", "Ping"},
          {"PlayerRpc", "TrackFtueStep"},
          {"PlayerRpc", "SkipTutorial"},
          {"PlayerRpc", "UpdatePlayerPrefs"},
          {"PlayerRpc", "SetPlayerPref"},
          {"BattleRpc", "BattleStart"},
          {"BattleRpc", "BattleFinish"},
          {"ContentRpc", "GetMetadata"},
          {"ContentRpc", "GetSegmentedContentDetails"},
          {"AuthRpc", "DoAuthGuest"},
          {"AuthRpc", "IsSessionActive"},
          {"AuthRpc", "DoLogout"},
      }};
  for (const auto& [service, method] : supported) {
    check(registry.supports(std::string(service), std::string(method)));
  }
  check(!registry.supports("PlayerRpc", "Unknown"));
  try {
    (void)registry.dispatch("PlayerRpc", "Unknown", request, state);
    check(false);
  } catch (const std::exception&) {
    check(true);
  }

  const RpcBytes initial =
      registry.dispatch("PlayerRpc", "GetInitialData", request, state);
  std::vector<Field> root;
  check(parse(initial, root));
  check(first(root, 1) != nullptr && as_string(*first(root, 1)) ==
                                         "local-profile");
  const Field* player_field = first(root, 2);
  const Field* inventory_field = first(root, 5);
  check(player_field != nullptr && inventory_field != nullptr);
  if (player_field != nullptr) {
    std::vector<Field> player;
    check(parse(player_field->bytes, player));
    check(first(player, 3) != nullptr && first(player, 3)->integer == 85);
    check(first(player, 7) != nullptr &&
          as_string(*first(player, 7)) == "Recovered Player");
    constexpr std::array<std::uint64_t, 5> expected_energy_types{7, 1, 3, 5,
                                                                 6};
    std::vector<std::uint64_t> energy_types;
    for (const auto& field : player) {
      if (field.number != 4) {
        continue;
      }
      std::vector<Field> energy;
      check(parse(field.bytes, energy));
      const Field* type = first(energy, 1);
      const Field* balance = first(energy, 3);
      check(type != nullptr && balance != nullptr);
      if (type != nullptr) {
        energy_types.push_back(type->integer);
      }
      check(balance != nullptr &&
            balance->integer == 2'000'000'000ULL);
    }
    check(std::equal(energy_types.begin(), energy_types.end(),
                     expected_energy_types.begin(),
                     expected_energy_types.end()));
  }
  if (inventory_field != nullptr) {
    std::vector<Field> inventory;
    check(parse(inventory_field->bytes, inventory));
    std::set<std::uint64_t> currency_ids;
    std::size_t unit_count = 0;
    for (const auto& field : inventory) {
      if (field.number == 1) {
        ++unit_count;
        std::vector<Field> unit;
        check(parse(field.bytes, unit));
        check(first(unit, 8) != nullptr && first(unit, 8)->integer == 1);
        check(first(unit, 9) != nullptr && first(unit, 9)->integer == 1);
        check(first(unit, 10) != nullptr && first(unit, 10)->integer == 0);
        check(first(unit, 19) != nullptr && first(unit, 19)->integer == 1);
        check(std::count_if(unit.begin(), unit.end(), [](const Field& value) {
                return value.number == 16;
              }) == 4);
      } else if (field.number == 9) {
        std::vector<Field> currency;
        check(parse(field.bytes, currency));
        const Field* id = first(currency, 1);
        const Field* balance = first(currency, 2);
        check(id != nullptr && balance != nullptr);
        if (id != nullptr) {
          currency_ids.insert(id->integer);
        }
        check(balance != nullptr &&
              balance->integer == 2'000'000'000ULL);
      }
    }
    check(unit_count == 2);
    check(currency_ids.size() == 47);
    check(!currency_ids.contains(5) && !currency_ids.contains(6) &&
          !currency_ids.contains(7));
    check(currency_ids.contains(1) && currency_ids.contains(50));
  }
  const Field* onboarding_field = first(root, 25);
  check(onboarding_field != nullptr);
  if (onboarding_field != nullptr) {
    std::vector<Field> onboarding;
    check(parse(onboarding_field->bytes, onboarding));
    check(first(onboarding, 3) != nullptr &&
          first(onboarding, 3)->integer == 1);
    check(first(onboarding, 4) != nullptr &&
          first(onboarding, 4)->integer == 7);
    std::uint64_t expected_index = 0;
    for (const auto& value : onboarding) {
      if (value.number != 1) {
        continue;
      }
      std::vector<Field> cell;
      check(parse(value.bytes, cell));
      check(first(cell, 3) != nullptr && first(cell, 3)->integer == 1);
      check(first(cell, 4) != nullptr &&
            first(cell, 4)->integer == expected_index++);
    }
    check(expected_index == 2);
  }
  check(contains_text(initial, "ANAKINKNIGHT:ONE_STAR"));
  check(contains_text(initial, "AHSOKATANO:ONE_STAR"));

  const RpcBytes auth =
      registry.dispatch("AuthRpc", "DoAuthGuest", request, state);
  std::vector<Field> auth_fields;
  check(parse(auth, auth_fields));
  check(first(auth_fields, 1) != nullptr &&
        as_string(*first(auth_fields, 1)) == "local-profile");
  check(first(auth_fields, 2) != nullptr &&
        as_string(*first(auth_fields, 2)) == "offline-auth-token");
  check(hex(registry.dispatch("AuthRpc", "IsSessionActive", request, state)) ==
        "0801");

  const RpcBytes metadata =
      registry.dispatch("ContentRpc", "GetMetadata", request, state);
  check(contains_text(metadata, "100042/Android/ETC"));
  check(contains_text(metadata, "offline://assets/"));
  check(contains_text(metadata, "0.40.3:Ad8NIoEjSterR5OOmkn9mw"));
  check(hex(registry.dispatch("ContentRpc", "GetSegmentedContentDetails",
                              request, state)) == "0a00");

  const RpcBytes battle_one =
      registry.dispatch("BattleRpc", "BattleStart", request, state);
  const RpcBytes battle_two =
      registry.dispatch("BattleRpc", "BattleStart", request, state);
  check(battle_one == battle_two);
  check(contains_text(battle_one, "offline-enemy-stormtrooper"));
  check(contains_text(battle_one, "offline-enemy-tusken"));
  std::vector<Field> battle_root;
  check(parse(battle_one, battle_root));
  const Field* battle_data_field = first(battle_root, 1);
  check(battle_data_field != nullptr);
  if (battle_data_field != nullptr) {
    std::vector<Field> battle_data;
    check(parse(battle_data_field->bytes, battle_data));
    check(first(battle_data, 9) != nullptr &&
          first(battle_data, 9)->integer ==
              (0x1234'5678U ^ 0x5347'4f48U));
    const Field* encounter_field = first(battle_data, 4);
    check(encounter_field != nullptr);
    if (encounter_field != nullptr) {
      std::vector<Field> encounter;
      check(parse(encounter_field->bytes, encounter));
      const Field* squad_field = first(encounter, 1);
      check(squad_field != nullptr);
      if (squad_field != nullptr) {
        std::vector<Field> enemy_squad;
        check(parse(squad_field->bytes, enemy_squad));
        constexpr std::array<std::string_view, 2> expected_ids{
            "offline-enemy-stormtrooper", "offline-enemy-tusken"};
        constexpr std::array<std::string_view, 2> expected_definitions{
            "STORMTROOPER:ONE_STAR", "TUSKENRAIDER:ONE_STAR"};
        std::size_t index = 0;
        for (const auto& value : enemy_squad) {
          if (value.number != 1 || index >= expected_ids.size()) {
            continue;
          }
          std::vector<Field> cell;
          check(parse(value.bytes, cell));
          const Field* stat_field = first(cell, 6);
          check(stat_field != nullptr);
          if (stat_field != nullptr) {
            std::vector<Field> stat;
            check(parse(stat_field->bytes, stat));
            check(first(stat, 13) != nullptr &&
                  as_string(*first(stat, 13)) == expected_ids[index]);
            check(first(stat, 14) != nullptr &&
                  as_string(*first(stat, 14)) ==
                      expected_definitions[index]);
          }
          ++index;
        }
        check(index == 2);
      }
    }
  }
  check(!state.settings.tutorial_skipped);
  check(hex(registry.dispatch("BattleRpc", "BattleFinish", request, state)) ==
        "3801");
  check(state.settings.tutorial_skipped);

  return failures;
}

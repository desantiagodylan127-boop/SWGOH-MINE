#include "heroes/offline/il2cpp_bridge.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace bridge = heroes::offline::il2cpp_bridge;

#define CHECK(condition)       \
  do {                         \
    if (!(condition)) {        \
      return __LINE__;         \
    }                          \
  } while (false)

namespace {

struct FakeClass {
  std::string name;
  std::string name_space;
  const FakeClass* parent;
};

class FakeInspector final : public bridge::ClassInspector {
 public:
  bool describe(const void* klass,
                bridge::ClassDescription& description) const noexcept override {
    if (klass == nullptr || rejected == klass) {
      return false;
    }
    const auto* value = static_cast<const FakeClass*>(klass);
    description = {value->name, value->name_space, value->parent};
    return true;
  }

  const void* rejected = nullptr;
};

class FakeInstaller final : public bridge::HookInstaller {
 public:
  bool install(void* target, void*, void** original) noexcept override {
    installed.push_back(target);
    if (installed.size() == failure_index) {
      return false;
    }
    *original = target;
    return true;
  }

  void rollback(void* target) noexcept override {
    rolled_back.push_back(target);
  }

  std::size_t failure_index = 0;
  std::vector<void*> installed;
  std::vector<void*> rolled_back;
};

int test_utf16_and_array_bounds() {
  constexpr std::array<char16_t, 4> text{u'A', 0xD83D, 0xDE80, u'Z'};
  const auto converted = bridge::utf16_to_utf8(text.data(), text.size());
  CHECK(converted.has_value());
  CHECK(*converted == "A\xF0\x9F\x9A\x80Z");
  CHECK(!bridge::utf16_to_utf8(nullptr, 1).has_value());
  CHECK(!bridge::utf16_to_utf8(text.data(), -1).has_value());
  CHECK(!bridge::utf16_to_utf8(text.data(), text.size(), 3).has_value());

  constexpr std::array<char16_t, 1> lone_surrogate{0xD800};
  CHECK(!bridge::utf16_to_utf8(lone_surrogate.data(),
                               lone_surrogate.size()).has_value());

  alignas(void*) std::array<std::byte, 64> array{};
  const std::uintptr_t acceptable = 12;
  std::memcpy(array.data() + 0x18, &acceptable, sizeof(acceptable));
  const auto view = bridge::il2cpp_byte_array_view(array.data(), 12);
  CHECK(view.has_value());
  CHECK(view->size == 12);
  CHECK(view->data ==
        reinterpret_cast<const std::uint8_t*>(array.data() + 0x20));

  const std::uintptr_t oversized = bridge::kMaximumPayloadSize + 1;
  std::memcpy(array.data() + 0x18, &oversized, sizeof(oversized));
  CHECK(!bridge::il2cpp_byte_array_view(array.data()).has_value());
  CHECK(!bridge::il2cpp_byte_array_view(nullptr).has_value());
  return 0;
}

int test_class_gate() {
  const FakeClass rpc_base{"RPCBattleCallback", "Game.Network.Rpc",
                           nullptr};
  const FakeClass generated{"GeneratedCallback", "Game.Generated", &rpc_base};
  const FakeClass wrong_case{"RpcBattleCallback", "Game.Network.Rpc", nullptr};
  const FakeClass wrong_namespace{"RPCBattleCallback", "Game.Network", nullptr};
  FakeInspector inspector;

  CHECK(bridge::is_rpc_callback_target(&generated, inspector));
  CHECK(!bridge::is_rpc_callback_target(&wrong_case, inspector));
  CHECK(!bridge::is_rpc_callback_target(&wrong_namespace, inspector));
  CHECK(!bridge::is_rpc_callback_target(nullptr, inspector));
  inspector.rejected = &rpc_base;
  CHECK(!bridge::is_rpc_callback_target(&generated, inspector));
  return 0;
}

int test_bundle_rewrite() {
  const std::filesystem::path root("/offline/bundles");
  const auto only_expected = [](const std::filesystem::path& path) {
    return path == "/offline/bundles/characters.bundle";
  };
  CHECK(bridge::rewrite_bundle_url(
            "https://cdn.invalid/v1/characters.bundle?token=old#part", root,
            only_expected) == "file:///offline/bundles/characters.bundle");
  CHECK(bridge::rewrite_bundle_url(
            "https://cdn.invalid/v1/missing.bundle?token=old", root,
            only_expected) ==
        "https://cdn.invalid/v1/missing.bundle?token=old");
  CHECK(bridge::rewrite_bundle_url("https://cdn.invalid/v1/data.json", root,
                                   only_expected) ==
        "https://cdn.invalid/v1/data.json");
  CHECK(bridge::rewrite_bundle_url("", root, only_expected).empty());
  return 0;
}

int test_transaction_rollback() {
  int targets[3]{};
  int replacements[3]{};
  void* originals[3]{
      reinterpret_cast<void*>(1),
      reinterpret_cast<void*>(1),
      reinterpret_cast<void*>(1),
  };
  const std::array<bridge::HookRequest, 3> hooks{{
      {&targets[0], &replacements[0], &originals[0]},
      {&targets[1], &replacements[1], &originals[1]},
      {&targets[2], &replacements[2], &originals[2]},
  }};
  FakeInstaller installer;
  installer.failure_index = 3;
  CHECK(!bridge::install_transaction(hooks, installer));
  CHECK(installer.rolled_back.size() == 2);
  CHECK(installer.rolled_back[0] == &targets[1]);
  CHECK(installer.rolled_back[1] == &targets[0]);
  CHECK(originals[0] == nullptr);
  CHECK(originals[1] == nullptr);

  FakeInstaller success;
  success.failure_index = 99;
  CHECK(bridge::install_transaction(hooks, success));
  CHECK(success.rolled_back.empty());
  return 0;
}

}  // namespace

int main() {
  const struct {
    const char* name;
    int (*run)();
  } tests[] = {
      {"utf16_and_array_bounds", test_utf16_and_array_bounds},
      {"class_gate", test_class_gate},
      {"bundle_rewrite", test_bundle_rewrite},
      {"transaction_rollback", test_transaction_rollback},
  };
  for (const auto& test : tests) {
    const int line = test.run();
    if (line != 0) {
      std::cerr << test.name << " failed at line " << line << '\n';
      return 1;
    }
  }
  std::cout << "il2cpp_bridge tests passed\n";
  return 0;
}

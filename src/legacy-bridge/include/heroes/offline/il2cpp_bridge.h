#ifndef HEROES_OFFLINE_IL2CPP_BRIDGE_H_
#define HEROES_OFFLINE_IL2CPP_BRIDGE_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace heroes::offline::il2cpp_bridge {

inline constexpr std::size_t kMaximumPayloadSize = 128U * 1024U * 1024U;
inline constexpr std::uintptr_t kHttpSendRva = 0x030E4CD4U;
inline constexpr std::uintptr_t kAssetBundleOneRva = 0x054B8228U;
inline constexpr std::uintptr_t kAssetBundleTwoRva = 0x054B8398U;

// These helpers reject null input, negative lengths, malformed surrogate pairs,
// and caller-defined size-limit violations.
std::optional<std::string> utf16_to_utf8(
    const char16_t* data, std::ptrdiff_t length,
    std::size_t maximum_code_units = 1024U * 1024U) noexcept;
std::optional<std::string> il2cpp_string_to_utf8(
    const void* string_object,
    std::size_t maximum_code_units = 1024U * 1024U) noexcept;

struct ByteArrayView {
  const std::uint8_t* data;
  std::size_t size;
};

std::optional<ByteArrayView> il2cpp_byte_array_view(
    const void* array_object,
    std::size_t maximum_size = kMaximumPayloadSize) noexcept;

using RegularFilePredicate =
    std::function<bool(const std::filesystem::path&)>;

// The query and fragment are ignored when selecting the bundle filename.
// A rewrite is returned only for a basename ending in ".bundle" that exists
// as a regular file under bundle_directory.
std::string rewrite_bundle_url(std::string_view url,
                               const std::filesystem::path& bundle_directory,
                               const RegularFilePredicate& is_regular_file)
    noexcept;
std::string rewrite_bundle_url(
    std::string_view url,
    const std::filesystem::path& bundle_directory) noexcept;

struct ClassDescription {
  std::string_view name;
  std::string_view name_space;
  const void* parent;
};

class ClassInspector {
 public:
  virtual ~ClassInspector() = default;
  virtual bool describe(const void* klass,
                        ClassDescription& description) const noexcept = 0;
};

// Walks at most 64 parents. A matching class must start with "RPC" and have
// "Rpc" somewhere in its namespace.
bool is_rpc_callback_target(const void* initial_class,
                            const ClassInspector& inspector) noexcept;

struct HookRequest {
  void* target;
  void* replacement;
  void** original;
};

class HookInstaller {
 public:
  virtual ~HookInstaller() = default;
  virtual bool install(void* target, void* replacement,
                       void** original) noexcept = 0;
  virtual void rollback(void* target) noexcept = 0;
};

// Installs in order and rolls back installed hooks in reverse order on error.
bool install_transaction(std::span<const HookRequest> hooks,
                         HookInstaller& installer) noexcept;

struct CoreDirectories {
  std::string storage_directory;
  std::string cache_directory;
  std::string pack_path;
  std::string bundle_directory;
};

struct HookSignatures {
  std::array<std::uint8_t, 8> http_send{};
  std::array<std::uint8_t, 8> asset_bundle_one{};
  std::array<std::uint8_t, 8> asset_bundle_two{};
};

using RequestDelegateInvoker = bool (*)(void* delegate_object,
                                        void* delegate_target,
                                        void* request_object,
                                        void* user_data) noexcept;
using ManagedClassResolver = void* (*)(std::string_view name_space,
                                       std::string_view name,
                                       void* user_data) noexcept;
using ReloadContentCallback = bool (*)(const CoreDirectories& directories,
                                       void* user_data) noexcept;

struct RuntimeCallbacks {
  HookInstaller* hook_installer = nullptr;
  RequestDelegateInvoker request_delegate_invoker = nullptr;
  ManagedClassResolver managed_class_resolver = nullptr;
  ReloadContentCallback reload_content = nullptr;
  void* user_data = nullptr;
  HookSignatures signatures{};
};

// Configuration is accepted only before the observer is armed.
bool configure(CoreDirectories directories, RuntimeCallbacks callbacks) noexcept;
bool start_observer() noexcept;
bool armed() noexcept;
bool installed() noexcept;
bool reload_content() noexcept;

}  // namespace heroes::offline::il2cpp_bridge

#endif  // HEROES_OFFLINE_IL2CPP_BRIDGE_H_

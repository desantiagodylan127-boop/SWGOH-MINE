#include "heroes/offline/il2cpp_bridge.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <link.h>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "heroes/offline/offline_core.h"

namespace heroes::offline::il2cpp_bridge {
namespace {

constexpr std::size_t kStringLengthOffset = 0x10;
constexpr std::size_t kStringDataOffset = 0x14;
constexpr std::size_t kArrayLengthOffset = 0x18;
constexpr std::size_t kArrayDataOffset = 0x20;
constexpr std::size_t kCallbackOffset = 0x50;
constexpr std::size_t kDelegateTargetOffset = 0x20;
constexpr std::size_t kServiceOffset = 0x88;
constexpr std::size_t kMethodOffset = 0x90;
constexpr std::size_t kRequestResponseOffset = 0xA0;
constexpr std::size_t kPayloadOffset = 0x150;
constexpr std::size_t kRequestStateOffset = 0xE4;
constexpr std::size_t kResponseStatusOffset = 0x18;
constexpr std::size_t kResponseDataOffset = 0x48;
constexpr std::int32_t kHttpOk = 200;
constexpr std::int32_t kRequestFinished = 3;

template <typename T>
T read_field(const void* object, const std::size_t offset) noexcept {
  T value{};
  if (object != nullptr) {
    std::memcpy(&value, static_cast<const std::byte*>(object) + offset,
                sizeof(value));
  }
  return value;
}

template <typename T>
void write_field(void* object, const std::size_t offset,
                 const T& value) noexcept {
  if (object != nullptr) {
    std::memcpy(static_cast<std::byte*>(object) + offset, &value, sizeof(value));
  }
}

bool signature_is_set(const std::array<std::uint8_t, 8>& signature) noexcept {
  return std::any_of(signature.begin(), signature.end(),
                     [](const std::uint8_t byte) { return byte != 0; });
}

struct Configuration {
  CoreDirectories directories;
  RuntimeCallbacks callbacks;
};

std::mutex configuration_mutex;
Configuration configuration;
std::atomic<bool> observer_armed{false};
std::atomic<bool> hooks_installed{false};
[[maybe_unused]] void* il2cpp_barrier = nullptr;

using DomainGet = void* (*)();
using ThreadAttach = void* (*)(void*);
using ObjectGetClass = void* (*)(void*);
using ClassGetName = const char* (*)(void*);
using ClassGetNamespace = const char* (*)(void*);
using ClassGetParent = void* (*)(void*);
using ArrayNew = void* (*)(void*, std::uintptr_t);
using ObjectNew = void* (*)(void*);
using RuntimeObjectInit = void (*)(void*);
using StringNew = void* (*)(const char*);
using GcWriteBarrier = void (*)(void*, void**, void*);

struct Il2CppApi {
  DomainGet domain_get = nullptr;
  ThreadAttach thread_attach = nullptr;
  ObjectGetClass object_get_class = nullptr;
  ClassGetName class_get_name = nullptr;
  ClassGetNamespace class_get_namespace = nullptr;
  ClassGetParent class_get_parent = nullptr;
  ArrayNew array_new = nullptr;
  ObjectNew object_new = nullptr;
  RuntimeObjectInit runtime_object_init = nullptr;
  StringNew string_new = nullptr;
  GcWriteBarrier gc_wbarrier_set_field = nullptr;

  bool complete() const noexcept {
    return domain_get != nullptr && thread_attach != nullptr &&
           object_get_class != nullptr && class_get_name != nullptr &&
           class_get_namespace != nullptr && class_get_parent != nullptr &&
           array_new != nullptr && object_new != nullptr &&
           runtime_object_init != nullptr && string_new != nullptr &&
           gc_wbarrier_set_field != nullptr;
  }
};

Il2CppApi api;

class RuntimeClassInspector final : public ClassInspector {
 public:
  bool describe(const void* klass,
                ClassDescription& description) const noexcept override {
    if (klass == nullptr || api.class_get_name == nullptr ||
        api.class_get_namespace == nullptr || api.class_get_parent == nullptr) {
      return false;
    }
    const char* name = api.class_get_name(const_cast<void*>(klass));
    const char* name_space =
        api.class_get_namespace(const_cast<void*>(klass));
    if (name == nullptr || name_space == nullptr) {
      return false;
    }
    description = {name, name_space,
                   api.class_get_parent(const_cast<void*>(klass))};
    return true;
  }
};

using HttpSend = void (*)(void*, void*);
using AssetBundleOne = void* (*)(void*, void*);
using AssetBundleTwo = void* (*)(void*, std::uint32_t, void*);

HttpSend original_http_send = nullptr;
AssetBundleOne original_asset_bundle_one = nullptr;
AssetBundleTwo original_asset_bundle_two = nullptr;

Configuration snapshot_configuration() {
  const std::lock_guard lock(configuration_mutex);
  return configuration;
}

void call_original_http(void* request, void* method) noexcept {
  try {
    if (original_http_send != nullptr) {
      original_http_send(request, method);
    }
  } catch (...) {
  }
}

void* call_original_asset_one(void* path, void* method) noexcept {
  try {
    return original_asset_bundle_one != nullptr
               ? original_asset_bundle_one(path, method)
               : nullptr;
  } catch (...) {
    return nullptr;
  }
}

void* call_original_asset_two(void* path, const std::uint32_t crc,
                              void* method) noexcept {
  try {
    return original_asset_bundle_two != nullptr
               ? original_asset_bundle_two(path, crc, method)
               : nullptr;
  } catch (...) {
    return nullptr;
  }
}

bool valid_managed_object(void* object) noexcept {
  if (object == nullptr || api.object_get_class == nullptr) {
    return false;
  }
  try {
    return api.object_get_class(object) != nullptr;
  } catch (...) {
    return false;
  }
}

bool managed_object_has_class(void* object, const std::string_view name_space,
                              const std::string_view name) noexcept {
  try {
    if (!valid_managed_object(object)) {
      return false;
    }
    void* klass = api.object_get_class(object);
    const char* actual_name = api.class_get_name(klass);
    const char* actual_namespace = api.class_get_namespace(klass);
    return actual_name != nullptr && actual_namespace != nullptr &&
           name == actual_name && name_space == actual_namespace;
  } catch (...) {
    return false;
  }
}

void set_managed_reference(void* owner, const std::size_t offset,
                           void* value) noexcept {
  void** location =
      reinterpret_cast<void**>(static_cast<std::byte*>(owner) + offset);
  api.gc_wbarrier_set_field(owner, location, value);
}

[[maybe_unused]] void http_send_proxy(void* request, void* method) noexcept {
  try {
    if (!valid_managed_object(request)) {
      call_original_http(request, method);
      return;
    }

    void* delegate = read_field<void*>(request, kCallbackOffset);
    if (!valid_managed_object(delegate)) {
      call_original_http(request, method);
      return;
    }
    void* target = read_field<void*>(delegate, kDelegateTargetOffset);
    if (!valid_managed_object(target)) {
      call_original_http(request, method);
      return;
    }

    RuntimeClassInspector inspector;
    if (!is_rpc_callback_target(api.object_get_class(target), inspector)) {
      call_original_http(request, method);
      return;
    }

    void* service_string = read_field<void*>(request, kServiceOffset);
    void* method_string = read_field<void*>(request, kMethodOffset);
    if (!managed_object_has_class(service_string, "System", "String") ||
        !managed_object_has_class(method_string, "System", "String")) {
      call_original_http(request, method);
      return;
    }
    const auto service = il2cpp_string_to_utf8(service_string);
    const auto rpc_method = il2cpp_string_to_utf8(method_string);
    if (!service || service->empty() || !rpc_method || rpc_method->empty()) {
      call_original_http(request, method);
      return;
    }

    void* payload_object = read_field<void*>(request, kPayloadOffset);
    ByteArrayView payload{nullptr, 0};
    if (payload_object != nullptr) {
      if (!managed_object_has_class(payload_object, "System", "Byte[]")) {
        call_original_http(request, method);
        return;
      }
      const auto payload_view = il2cpp_byte_array_view(payload_object);
      if (!payload_view) {
        call_original_http(request, method);
        return;
      }
      payload = *payload_view;
    }

    ho_owned_bytes native_response{nullptr, 0};
    const ho_status dispatch_status =
        ho_dispatch_rpc(service->c_str(), rpc_method->c_str(), payload.data,
                        payload.size, &native_response);
    struct ResponseOwner {
      ho_owned_bytes* response;
      ~ResponseOwner() {
        if (response->data != nullptr) {
          ho_free(response->data);
        }
      }
    } response_owner{&native_response};
    if (dispatch_status != HO_STATUS_OK ||
        native_response.size > kMaximumPayloadSize ||
        (native_response.data == nullptr && native_response.size != 0)) {
      call_original_http(request, method);
      return;
    }

    const Configuration current = snapshot_configuration();
    if (current.callbacks.managed_class_resolver == nullptr ||
        current.callbacks.request_delegate_invoker == nullptr) {
      call_original_http(request, method);
      return;
    }
    void* byte_class = current.callbacks.managed_class_resolver(
        "System", "Byte", current.callbacks.user_data);
    void* response_class = current.callbacks.managed_class_resolver(
        "BestHTTP", "HTTPResponse", current.callbacks.user_data);
    if (byte_class == nullptr || response_class == nullptr) {
      call_original_http(request, method);
      return;
    }

    void* managed_payload = api.array_new(byte_class, native_response.size);
    if (!valid_managed_object(managed_payload)) {
      call_original_http(request, method);
      return;
    }
    if (native_response.size != 0) {
      std::memcpy(static_cast<std::byte*>(managed_payload) + kArrayDataOffset,
                  native_response.data, native_response.size);
    }

    void* response = api.object_new(response_class);
    if (!valid_managed_object(response)) {
      call_original_http(request, method);
      return;
    }
    api.runtime_object_init(response);
    if (!valid_managed_object(response)) {
      call_original_http(request, method);
      return;
    }

    write_field(response, kResponseStatusOffset, kHttpOk);
    set_managed_reference(response, kResponseDataOffset, managed_payload);
    set_managed_reference(request, kRequestResponseOffset, response);
    write_field(request, kRequestStateOffset, kRequestFinished);

    if (!current.callbacks.request_delegate_invoker(
            delegate, target, request, current.callbacks.user_data)) {
      // The request is already completed. Calling the network original here
      // would duplicate the request after mutating its managed state.
      return;
    }
  } catch (...) {
    call_original_http(request, method);
  }
}

[[maybe_unused]] void* asset_bundle_one_proxy(void* path,
                                               void* method) noexcept {
  try {
    if (original_asset_bundle_one == nullptr) {
      return nullptr;
    }
    if (!managed_object_has_class(path, "System", "String")) {
      return call_original_asset_one(path, method);
    }
    const auto url = il2cpp_string_to_utf8(path);
    if (!url) {
      return call_original_asset_one(path, method);
    }
    const Configuration current = snapshot_configuration();
    const std::string rewritten =
        rewrite_bundle_url(*url, current.directories.bundle_directory);
    if (rewritten == *url) {
      return call_original_asset_one(path, method);
    }
    void* managed_path = api.string_new(rewritten.c_str());
    return managed_path != nullptr
               ? call_original_asset_one(managed_path, method)
               : call_original_asset_one(path, method);
  } catch (...) {
    return call_original_asset_one(path, method);
  }
}

[[maybe_unused]] void* asset_bundle_two_proxy(
    void* path, const std::uint32_t crc, void* method) noexcept {
  try {
    if (original_asset_bundle_two == nullptr) {
      return nullptr;
    }
    if (!managed_object_has_class(path, "System", "String")) {
      return call_original_asset_two(path, crc, method);
    }
    const auto url = il2cpp_string_to_utf8(path);
    if (!url) {
      return call_original_asset_two(path, crc, method);
    }
    const Configuration current = snapshot_configuration();
    const std::string rewritten =
        rewrite_bundle_url(*url, current.directories.bundle_directory);
    if (rewritten == *url) {
      return call_original_asset_two(path, crc, method);
    }
    void* managed_path = api.string_new(rewritten.c_str());
    return managed_path != nullptr
               ? call_original_asset_two(managed_path, crc, method)
               : call_original_asset_two(path, crc, method);
  } catch (...) {
    return call_original_asset_two(path, crc, method);
  }
}

struct ModuleInfo {
  std::uintptr_t base = 0;
  std::string path;
  std::vector<std::pair<std::uintptr_t, std::uintptr_t>> executable_ranges;
};

[[maybe_unused]] int find_il2cpp_module(dl_phdr_info* info, std::size_t,
                                        void* data) {
  try {
    if (info == nullptr || info->dlpi_name == nullptr) {
      return 0;
    }
    const std::string_view path(info->dlpi_name);
    const std::size_t slash = path.find_last_of('/');
    if (path.substr(slash == std::string_view::npos ? 0 : slash + 1) !=
        "libil2cpp.so") {
      return 0;
    }
    auto& module = *static_cast<ModuleInfo*>(data);
    module.base = static_cast<std::uintptr_t>(info->dlpi_addr);
    module.path = std::string(path);
    for (std::size_t index = 0; index < info->dlpi_phnum; ++index) {
      const ElfW(Phdr)& header = info->dlpi_phdr[index];
      if (header.p_type == PT_LOAD && (header.p_flags & PF_X) != 0) {
        const std::uintptr_t begin = module.base + header.p_vaddr;
        module.executable_ranges.emplace_back(begin, begin + header.p_memsz);
      }
    }
    return 1;
  } catch (...) {
    return 0;
  }
}

bool range_contains(const ModuleInfo& module, const std::uintptr_t address,
                    const std::size_t size) noexcept {
  if (address > UINTPTR_MAX - size) {
    return false;
  }
  return std::any_of(
      module.executable_ranges.begin(), module.executable_ranges.end(),
      [address, size](const auto& range) {
        return address >= range.first && address + size <= range.second;
      });
}

[[maybe_unused]] bool matches_signature(
    const ModuleInfo& module, const std::uintptr_t rva,
    const std::array<std::uint8_t, 8>& expected) noexcept {
  if (module.base > UINTPTR_MAX - rva) {
    return false;
  }
  const std::uintptr_t address = module.base + rva;
  return range_contains(module, address, expected.size()) &&
         std::memcmp(reinterpret_cast<const void*>(address), expected.data(),
                     expected.size()) == 0;
}

template <typename T>
T load_symbol(void* handle, const char* name) noexcept {
  return reinterpret_cast<T>(dlsym(handle, name));
}

[[maybe_unused]] bool resolve_api(void* handle) noexcept {
  Il2CppApi resolved;
  resolved.domain_get = load_symbol<DomainGet>(handle, "il2cpp_domain_get");
  resolved.thread_attach =
      load_symbol<ThreadAttach>(handle, "il2cpp_thread_attach");
  resolved.object_get_class =
      load_symbol<ObjectGetClass>(handle, "il2cpp_object_get_class");
  resolved.class_get_name =
      load_symbol<ClassGetName>(handle, "il2cpp_class_get_name");
  resolved.class_get_namespace =
      load_symbol<ClassGetNamespace>(handle, "il2cpp_class_get_namespace");
  resolved.class_get_parent =
      load_symbol<ClassGetParent>(handle, "il2cpp_class_get_parent");
  resolved.array_new = load_symbol<ArrayNew>(handle, "il2cpp_array_new");
  resolved.object_new = load_symbol<ObjectNew>(handle, "il2cpp_object_new");
  resolved.runtime_object_init =
      load_symbol<RuntimeObjectInit>(handle, "il2cpp_runtime_object_init");
  resolved.string_new = load_symbol<StringNew>(handle, "il2cpp_string_new");
  resolved.gc_wbarrier_set_field =
      load_symbol<GcWriteBarrier>(handle, "il2cpp_gc_wbarrier_set_field");
  if (!resolved.complete()) {
    return false;
  }
  api = resolved;
  void* domain = api.domain_get();
  return domain != nullptr && api.thread_attach(domain) != nullptr;
}

bool try_install() noexcept {
#if !defined(__aarch64__)
  return false;
#else
  try {
    ModuleInfo module;
    dl_iterate_phdr(find_il2cpp_module, &module);
    if (module.base == 0 || module.path.empty()) {
      return false;
    }

    void* barrier = dlopen(module.path.c_str(), RTLD_NOW | RTLD_NOLOAD);
    if (barrier == nullptr) {
      barrier = dlopen("libil2cpp.so", RTLD_NOW | RTLD_NOLOAD);
    }
    if (barrier == nullptr || !resolve_api(barrier)) {
      if (barrier != nullptr) {
        dlclose(barrier);
      }
      return false;
    }

    const Configuration current = snapshot_configuration();
    if (!matches_signature(module, kHttpSendRva,
                           current.callbacks.signatures.http_send) ||
        !matches_signature(module, kAssetBundleOneRva,
                           current.callbacks.signatures.asset_bundle_one) ||
        !matches_signature(module, kAssetBundleTwoRva,
                           current.callbacks.signatures.asset_bundle_two)) {
      dlclose(barrier);
      return false;
    }

    const std::array<HookRequest, 3> hooks{{
        {reinterpret_cast<void*>(module.base + kHttpSendRva),
         reinterpret_cast<void*>(&http_send_proxy),
         reinterpret_cast<void**>(&original_http_send)},
        {reinterpret_cast<void*>(module.base + kAssetBundleOneRva),
         reinterpret_cast<void*>(&asset_bundle_one_proxy),
         reinterpret_cast<void**>(&original_asset_bundle_one)},
        {reinterpret_cast<void*>(module.base + kAssetBundleTwoRva),
         reinterpret_cast<void*>(&asset_bundle_two_proxy),
         reinterpret_cast<void**>(&original_asset_bundle_two)},
    }};
    if (current.callbacks.hook_installer == nullptr ||
        !install_transaction(hooks, *current.callbacks.hook_installer)) {
      original_http_send = nullptr;
      original_asset_bundle_one = nullptr;
      original_asset_bundle_two = nullptr;
      dlclose(barrier);
      return false;
    }
    il2cpp_barrier = barrier;
    hooks_installed.store(true, std::memory_order_release);
    return true;
  } catch (...) {
    return false;
  }
#endif
}

void observer_main() noexcept {
  while (!hooks_installed.load(std::memory_order_acquire)) {
    if (try_install()) {
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

}  // namespace

std::optional<std::string> utf16_to_utf8(
    const char16_t* data, const std::ptrdiff_t length,
    const std::size_t maximum_code_units) noexcept {
  try {
    if (data == nullptr || length < 0 ||
        static_cast<std::size_t>(length) > maximum_code_units) {
      return std::nullopt;
    }
    std::string result;
    result.reserve(static_cast<std::size_t>(length));
    for (std::size_t index = 0; index < static_cast<std::size_t>(length);
         ++index) {
      std::uint32_t code_point = data[index];
      if (code_point >= 0xD800U && code_point <= 0xDBFFU) {
        if (++index >= static_cast<std::size_t>(length)) {
          return std::nullopt;
        }
        const std::uint32_t low = data[index];
        if (low < 0xDC00U || low > 0xDFFFU) {
          return std::nullopt;
        }
        code_point =
            0x10000U + ((code_point - 0xD800U) << 10U) + (low - 0xDC00U);
      } else if (code_point >= 0xDC00U && code_point <= 0xDFFFU) {
        return std::nullopt;
      }

      if (code_point <= 0x7FU) {
        result.push_back(static_cast<char>(code_point));
      } else if (code_point <= 0x7FFU) {
        result.push_back(static_cast<char>(0xC0U | (code_point >> 6U)));
        result.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
      } else if (code_point <= 0xFFFFU) {
        result.push_back(static_cast<char>(0xE0U | (code_point >> 12U)));
        result.push_back(
            static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
        result.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
      } else {
        result.push_back(static_cast<char>(0xF0U | (code_point >> 18U)));
        result.push_back(
            static_cast<char>(0x80U | ((code_point >> 12U) & 0x3FU)));
        result.push_back(
            static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
        result.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
      }
    }
    return result;
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<std::string> il2cpp_string_to_utf8(
    const void* string_object,
    const std::size_t maximum_code_units) noexcept {
  if (string_object == nullptr) {
    return std::nullopt;
  }
  const std::int32_t length =
      read_field<std::int32_t>(string_object, kStringLengthOffset);
  return utf16_to_utf8(
      reinterpret_cast<const char16_t*>(
          static_cast<const std::byte*>(string_object) + kStringDataOffset),
      length, maximum_code_units);
}

std::optional<ByteArrayView> il2cpp_byte_array_view(
    const void* array_object, const std::size_t maximum_size) noexcept {
  if (array_object == nullptr) {
    return std::nullopt;
  }
  const std::uintptr_t length =
      read_field<std::uintptr_t>(array_object, kArrayLengthOffset);
  if (length > maximum_size) {
    return std::nullopt;
  }
  return ByteArrayView{
      reinterpret_cast<const std::uint8_t*>(
          static_cast<const std::byte*>(array_object) + kArrayDataOffset),
      static_cast<std::size_t>(length)};
}

std::string rewrite_bundle_url(
    const std::string_view url,
    const std::filesystem::path& bundle_directory,
    const RegularFilePredicate& is_regular_file) noexcept {
  try {
    if (!is_regular_file || bundle_directory.empty()) {
      return std::string(url);
    }
    const std::size_t suffix = url.find_first_of("?#");
    const std::string_view without_suffix = url.substr(0, suffix);
    const std::size_t slash = without_suffix.find_last_of("/\\");
    const std::string_view basename =
        without_suffix.substr(slash == std::string_view::npos ? 0 : slash + 1);
    constexpr std::string_view extension = ".bundle";
    if (basename.size() <= extension.size() ||
        !basename.ends_with(extension) || basename == "." ||
        basename == "..") {
      return std::string(url);
    }
    const std::filesystem::path candidate =
        bundle_directory / std::string(basename);
    if (!is_regular_file(candidate)) {
      return std::string(url);
    }
    return std::string("file://") + candidate.lexically_normal().generic_string();
  } catch (...) {
    return std::string(url);
  }
}

std::string rewrite_bundle_url(
    const std::string_view url,
    const std::filesystem::path& bundle_directory) noexcept {
  return rewrite_bundle_url(
      url, bundle_directory, [](const std::filesystem::path& path) {
        std::error_code error;
        return std::filesystem::is_regular_file(path, error) && !error;
      });
}

bool is_rpc_callback_target(const void* initial_class,
                            const ClassInspector& inspector) noexcept {
  try {
    const void* current = initial_class;
    for (std::size_t depth = 0; current != nullptr && depth < 64; ++depth) {
      ClassDescription description;
      if (!inspector.describe(current, description)) {
        return false;
      }
      if (description.name.starts_with("RPC") &&
          description.name_space.find("Rpc") != std::string_view::npos) {
        return true;
      }
      if (description.parent == current) {
        return false;
      }
      current = description.parent;
    }
    return false;
  } catch (...) {
    return false;
  }
}

bool install_transaction(const std::span<const HookRequest> hooks,
                         HookInstaller& installer) noexcept {
  std::size_t installed_count = 0;
  try {
    for (const HookRequest& hook : hooks) {
      if (hook.target == nullptr || hook.replacement == nullptr ||
          hook.original == nullptr ||
          !installer.install(hook.target, hook.replacement, hook.original)) {
        while (installed_count != 0) {
          --installed_count;
          installer.rollback(hooks[installed_count].target);
          *hooks[installed_count].original = nullptr;
        }
        return false;
      }
      ++installed_count;
    }
    return true;
  } catch (...) {
    while (installed_count != 0) {
      --installed_count;
      try {
        installer.rollback(hooks[installed_count].target);
      } catch (...) {
      }
      *hooks[installed_count].original = nullptr;
    }
    return false;
  }
}

bool configure(CoreDirectories directories,
               RuntimeCallbacks callbacks) noexcept {
  try {
    if (observer_armed.load(std::memory_order_acquire) ||
        callbacks.hook_installer == nullptr ||
        callbacks.request_delegate_invoker == nullptr ||
        callbacks.managed_class_resolver == nullptr ||
        !signature_is_set(callbacks.signatures.http_send) ||
        !signature_is_set(callbacks.signatures.asset_bundle_one) ||
        !signature_is_set(callbacks.signatures.asset_bundle_two)) {
      return false;
    }
    const std::lock_guard lock(configuration_mutex);
    if (observer_armed.load(std::memory_order_relaxed)) {
      return false;
    }
    configuration = {std::move(directories), callbacks};
    return true;
  } catch (...) {
    return false;
  }
}

bool start_observer() noexcept {
  try {
    {
      const std::lock_guard lock(configuration_mutex);
      if (configuration.callbacks.hook_installer == nullptr) {
        return false;
      }
    }
    bool expected = false;
    if (!observer_armed.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
      return true;
    }
    std::thread(observer_main).detach();
    return true;
  } catch (...) {
    observer_armed.store(false, std::memory_order_release);
    return false;
  }
}

bool armed() noexcept {
  return observer_armed.load(std::memory_order_acquire);
}

bool installed() noexcept {
  return hooks_installed.load(std::memory_order_acquire);
}

bool reload_content() noexcept {
  try {
    const Configuration current = snapshot_configuration();
    return current.callbacks.reload_content != nullptr &&
           current.callbacks.reload_content(current.directories,
                                             current.callbacks.user_data);
  } catch (...) {
    return false;
  }
}

}  // namespace heroes::offline::il2cpp_bridge

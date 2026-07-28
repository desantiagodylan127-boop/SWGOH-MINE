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
#include <type_traits>
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

HttpRequestSend original_http_send = nullptr;
AssetBundleVersionLoad original_asset_bundle_one = nullptr;
AssetBundleHashLoad original_asset_bundle_two = nullptr;
DoGameServiceLoginManaged original_do_game_service_login = nullptr;
IniLoadFromUrl original_ini_load_from_url = nullptr;
ImportAccountViewReady original_import_account_view_ready = nullptr;
std::uintptr_t il2cpp_image_base = 0;

Configuration snapshot_configuration() {
  const std::lock_guard lock(configuration_mutex);
  return configuration;
}

void* call_original_http(void* request, const void* method) noexcept {
  try {
    return original_http_send != nullptr ? original_http_send(request, method)
                                         : nullptr;
  } catch (...) {
    return nullptr;
  }
}

void* call_original_asset_one(Il2CppString* path,
                              const std::uint32_t version,
                              const std::uint32_t crc,
                              const void* method) noexcept {
  try {
    return original_asset_bundle_one != nullptr
               ? original_asset_bundle_one(path, version, crc, method)
               : nullptr;
  } catch (...) {
    return nullptr;
  }
}

void* call_original_asset_two(Il2CppString* path, const Hash128 hash,
                              const std::uint32_t crc,
                              const void* method) noexcept {
  try {
    return original_asset_bundle_two != nullptr
               ? original_asset_bundle_two(path, hash, crc, method)
               : nullptr;
  } catch (...) {
    return nullptr;
  }
}

// object_get_class cannot make an arbitrary native pointer safe: a bad pointer
// can raise SIGSEGV, which C++ catch cannot handle. Callers use this only for
// the managed hook argument or managed references read from an object whose
// class chain has already been validated.
void* managed_reference_class(void* object) noexcept {
  if (object == nullptr || api.object_get_class == nullptr) {
    return nullptr;
  }
  try {
    return api.object_get_class(object);
  } catch (...) {
    return nullptr;
  }
}

bool valid_class_chain(const void* initial_class,
                       const RuntimeClassInspector& inspector) noexcept {
  try {
    const void* current = initial_class;
    for (std::size_t depth = 0; current != nullptr && depth < 64; ++depth) {
      ClassDescription description;
      if (!inspector.describe(current, description) ||
          description.name.empty()) {
        return false;
      }
      if (description.parent == current) {
        return false;
      }
      current = description.parent;
    }
    return current == nullptr;
  } catch (...) {
    return false;
  }
}

bool class_chain_contains(const void* initial_class,
                          const RuntimeClassInspector& inspector,
                          const std::string_view name_space,
                          const std::string_view name) noexcept {
  try {
    const void* current = initial_class;
    for (std::size_t depth = 0; current != nullptr && depth < 64; ++depth) {
      ClassDescription description;
      if (!inspector.describe(current, description) ||
          description.name.empty()) {
        return false;
      }
      if (description.name == name && description.name_space == name_space) {
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

bool managed_reference_has_class(void* object,
                                 const std::string_view name_space,
                                 const std::string_view name) noexcept {
  RuntimeClassInspector inspector;
  void* klass = managed_reference_class(object);
  if (!valid_class_chain(klass, inspector)) {
    return false;
  }
  ClassDescription description;
  return inspector.describe(klass, description) &&
         description.name == name && description.name_space == name_space;
}

void set_managed_reference(void* owner, const std::size_t offset,
                           void* value) noexcept {
  void** location =
      reinterpret_cast<void**>(static_cast<std::byte*>(owner) + offset);
  api.gc_wbarrier_set_field(owner, location, value);
}

[[maybe_unused]] void* http_send_proxy(void* request,
                                       const void* method) {
  bool committed = false;
  try {
    RuntimeClassInspector inspector;
    void* request_class = managed_reference_class(request);
    if (!valid_class_chain(request_class, inspector) ||
        !class_chain_contains(request_class, inspector, "BestHTTP",
                              "HTTPRequest")) {
      return call_original_http(request, method);
    }

    void* delegate = read_field<void*>(request, kCallbackOffset);
    void* delegate_class = managed_reference_class(delegate);
    if (!valid_class_chain(delegate_class, inspector)) {
      return call_original_http(request, method);
    }
    void* target = read_field<void*>(delegate, kDelegateTargetOffset);
    void* target_class = managed_reference_class(target);
    if (!valid_class_chain(target_class, inspector)) {
      return call_original_http(request, method);
    }

    if (!is_rpc_callback_target(target_class, inspector)) {
      return call_original_http(request, method);
    }

    void* service_string = read_field<void*>(target, kServiceOffset);
    void* method_string = read_field<void*>(target, kMethodOffset);
    if (!managed_reference_has_class(service_string, "System", "String") ||
        !managed_reference_has_class(method_string, "System", "String")) {
      return call_original_http(request, method);
    }
    const auto service = il2cpp_string_to_utf8(service_string);
    const auto rpc_method = il2cpp_string_to_utf8(method_string);
    if (!service || service->empty() || !rpc_method || rpc_method->empty()) {
      return call_original_http(request, method);
    }

    void* payload_object = read_field<void*>(target, kPayloadOffset);
    ByteArrayView payload{nullptr, 0};
    if (payload_object != nullptr) {
      if (!managed_reference_has_class(payload_object, "System", "Byte[]")) {
        return call_original_http(request, method);
      }
      const auto payload_view = il2cpp_byte_array_view(payload_object);
      if (!payload_view) {
        return call_original_http(request, method);
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
      return call_original_http(request, method);
    }

    const Configuration current = snapshot_configuration();
    if (current.callbacks.managed_class_resolver == nullptr ||
        current.callbacks.request_delegate_invoker == nullptr) {
      return call_original_http(request, method);
    }
    void* byte_class = current.callbacks.managed_class_resolver(
        "System", "Byte", current.callbacks.user_data);
    void* response_class = current.callbacks.managed_class_resolver(
        "BestHTTP", "HTTPResponse", current.callbacks.user_data);
    if (byte_class == nullptr || response_class == nullptr) {
      return call_original_http(request, method);
    }

    void* managed_payload = api.array_new(byte_class, native_response.size);
    if (managed_payload == nullptr) {
      return call_original_http(request, method);
    }
    if (native_response.size != 0) {
      std::memcpy(static_cast<std::byte*>(managed_payload) + kArrayDataOffset,
                  native_response.data, native_response.size);
    }

    void* response = api.object_new(response_class);
    if (response == nullptr) {
      return call_original_http(request, method);
    }
    api.runtime_object_init(response);

    write_field(response, kResponseStatusOffset, kHttpOk);
    set_managed_reference(response, kResponseDataOffset, managed_payload);
    committed = true;
    set_managed_reference(request, kRequestResponseOffset, response);
    write_field(request, kRequestStateOffset, kRequestFinished);

    (void)invoke_request_delegate(current.callbacks, delegate, target, request,
                                  response);
    return request;
  } catch (...) {
    return committed ? request : call_original_http(request, method);
  }
}

static_assert(
    std::is_same_v<decltype(&http_send_proxy), HttpRequestSend>);

[[maybe_unused]] void* asset_bundle_one_proxy(
    Il2CppString* path, const std::uint32_t version,
    const std::uint32_t crc, const void* method) {
  try {
    if (original_asset_bundle_one == nullptr) {
      return nullptr;
    }
    if (!managed_reference_has_class(path, "System", "String")) {
      return call_original_asset_one(path, version, crc, method);
    }
    const auto url = il2cpp_string_to_utf8(path);
    if (!url) {
      return call_original_asset_one(path, version, crc, method);
    }
    const Configuration current = snapshot_configuration();
    BundleMaterializer materializer;
    if (current.callbacks.materialize_bundle != nullptr) {
      materializer = [&current](const std::string_view basename) {
        std::array<char, 1024> output{};
        if (!current.callbacks.materialize_bundle(
                std::string(basename).c_str(), output.data(), output.size(),
                current.callbacks.user_data) ||
            output[0] == '\0') {
          return std::string();
        }
        return std::string(output.data());
      };
    }
    const std::string rewritten = rewrite_bundle_url(
        *url, current.directories.bundle_directory, materializer);
    if (rewritten == *url) {
      return call_original_asset_one(path, version, crc, method);
    }
    auto* managed_path =
        static_cast<Il2CppString*>(api.string_new(rewritten.c_str()));
    return managed_path != nullptr
               ? call_original_asset_one(managed_path, version, crc, method)
               : call_original_asset_one(path, version, crc, method);
  } catch (...) {
    return call_original_asset_one(path, version, crc, method);
  }
}

[[maybe_unused]] void* asset_bundle_two_proxy(
    Il2CppString* path, const Hash128 hash, const std::uint32_t crc,
    const void* method) {
  try {
    if (original_asset_bundle_two == nullptr) {
      return nullptr;
    }
    if (!managed_reference_has_class(path, "System", "String")) {
      return call_original_asset_two(path, hash, crc, method);
    }
    const auto url = il2cpp_string_to_utf8(path);
    if (!url) {
      return call_original_asset_two(path, hash, crc, method);
    }
    const Configuration current = snapshot_configuration();
    BundleMaterializer materializer;
    if (current.callbacks.materialize_bundle != nullptr) {
      materializer = [&current](const std::string_view basename) {
        std::array<char, 1024> output{};
        if (!current.callbacks.materialize_bundle(
                std::string(basename).c_str(), output.data(), output.size(),
                current.callbacks.user_data) ||
            output[0] == '\0') {
          return std::string();
        }
        return std::string(output.data());
      };
    }
    const std::string rewritten = rewrite_bundle_url(
        *url, current.directories.bundle_directory, materializer);
    if (rewritten == *url) {
      return call_original_asset_two(path, hash, crc, method);
    }
    auto* managed_path =
        static_cast<Il2CppString*>(api.string_new(rewritten.c_str()));
    return managed_path != nullptr
               ? call_original_asset_two(managed_path, hash, crc, method)
               : call_original_asset_two(path, hash, crc, method);
  } catch (...) {
    return call_original_asset_two(path, hash, crc, method);
  }
}

static_assert(std::is_same_v<decltype(&asset_bundle_one_proxy),
                             AssetBundleVersionLoad>);
static_assert(std::is_same_v<decltype(&asset_bundle_two_proxy),
                             AssetBundleHashLoad>);

[[maybe_unused]] void do_game_service_login_proxy(void* self,
                                                  Il2CppString* auth_type,
                                                  bool, bool,
                                                  const void* method) {
  // Match offline12: do not call the hooked trampoline (wrong path / network).
  // Drive guest login through the AuthSelected sibling with forceGuest=true,
  // then the post-auth helper and finish entry. MethodInfo for AuthSelected is
  // intentionally null — that helper never reads it in this build.
  try {
    if (il2cpp_image_base == 0 || self == nullptr) {
      if (original_do_game_service_login != nullptr) {
        original_do_game_service_login(self, auth_type, true, false, method);
      }
      return;
    }
    const auto auth_selected = reinterpret_cast<DoGameServiceLoginAuthSelected>(
        il2cpp_image_base + kDoGameServiceLoginAuthSelectedRva);
    const auto post_auth = reinterpret_cast<DoGameServiceLoginPostAuth>(
        il2cpp_image_base + kDoGameServiceLoginPostAuthRva);
    const auto finish = reinterpret_cast<DoGameServiceLoginFinish>(
        il2cpp_image_base + kDoGameServiceLoginFinishRva);
    auth_selected(self, true, auth_type, nullptr);
    post_auth(self, true, nullptr);
    finish(self, nullptr, reinterpret_cast<const void*>(finish));
  } catch (...) {
  }
}

[[maybe_unused]] void* ini_load_from_url_proxy(void* self, Il2CppString* url,
                                               Il2CppString* section,
                                               void* cache,
                                               Il2CppString* fallback,
                                               const void* method) {
  try {
    if (original_ini_load_from_url == nullptr) {
      return nullptr;
    }
    if (!managed_reference_has_class(url, "System", "String")) {
      return original_ini_load_from_url(self, url, section, cache, fallback,
                                        method);
    }
    const auto text = il2cpp_string_to_utf8(url);
    if (!text) {
      return original_ini_load_from_url(self, url, section, cache, fallback,
                                        method);
    }
    const Configuration current = snapshot_configuration();
    const std::string rewritten =
        rewrite_ini_url(*text, current.directories.local_ini_path);
    if (rewritten == *text) {
      return original_ini_load_from_url(self, url, section, cache, fallback,
                                        method);
    }
    auto* managed_url =
        static_cast<Il2CppString*>(api.string_new(rewritten.c_str()));
    return original_ini_load_from_url(
        self, managed_url != nullptr ? managed_url : url, section, cache,
        fallback, method);
  } catch (...) {
    return original_ini_load_from_url != nullptr
               ? original_ini_load_from_url(self, url, section, cache, fallback,
                                            method)
               : nullptr;
  }
}

[[maybe_unused]] void import_account_view_ready_proxy(void* self, void* unused,
                                                      const void* method) {
  // Pass-through only. The offline12 sibling "continue" call is MethodInfo-
  // sensitive and was crashing on device.
  try {
    if (original_import_account_view_ready != nullptr) {
      original_import_account_view_ready(self, unused, method);
    }
  } catch (...) {
  }
}

static_assert(std::is_same_v<decltype(&do_game_service_login_proxy),
                             DoGameServiceLoginManaged>);
static_assert(
    std::is_same_v<decltype(&ini_load_from_url_proxy), IniLoadFromUrl>);
static_assert(std::is_same_v<decltype(&import_account_view_ready_proxy),
                             ImportAccountViewReady>);

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
        !matches_signature(module, kIniLoadFromUrlRva,
                           current.callbacks.signatures.ini_load_from_url) ||
        !matches_signature(
            module, kDoGameServiceLoginRva,
            current.callbacks.signatures.do_game_service_login)) {
      dlclose(barrier);
      return false;
    }

    il2cpp_image_base = module.base;
    // r3 installs only the post-splash transport/login hooks. AssetBundle and
    // ImportAccount hooks are intentionally omitted until those paths are
    // crash-free on device.
    const std::array<HookRequest, 3> hooks{{
        {reinterpret_cast<void*>(module.base + kHttpSendRva),
         reinterpret_cast<void*>(&http_send_proxy),
         reinterpret_cast<void**>(&original_http_send)},
        {reinterpret_cast<void*>(module.base + kIniLoadFromUrlRva),
         reinterpret_cast<void*>(&ini_load_from_url_proxy),
         reinterpret_cast<void**>(&original_ini_load_from_url)},
        {reinterpret_cast<void*>(module.base + kDoGameServiceLoginRva),
         reinterpret_cast<void*>(&do_game_service_login_proxy),
         reinterpret_cast<void**>(&original_do_game_service_login)},
    }};
    if (current.callbacks.hook_installer == nullptr ||
        !install_transaction(hooks, *current.callbacks.hook_installer)) {
      original_http_send = nullptr;
      original_asset_bundle_one = nullptr;
      original_asset_bundle_two = nullptr;
      original_do_game_service_login = nullptr;
      original_ini_load_from_url = nullptr;
      original_import_account_view_ready = nullptr;
      il2cpp_image_base = 0;
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
    const RegularFilePredicate& is_regular_file,
    const BundleMaterializer& materialize_bundle) noexcept {
  try {
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

    if (is_regular_file && !bundle_directory.empty()) {
      const std::filesystem::path candidate =
          bundle_directory / std::string(basename);
      if (is_regular_file(candidate)) {
        return std::string("file://") +
               candidate.lexically_normal().generic_string();
      }
    }

    if (materialize_bundle) {
      const std::string materialized = materialize_bundle(basename);
      if (!materialized.empty()) {
        return std::string("file://") +
               std::filesystem::path(materialized)
                   .lexically_normal()
                   .generic_string();
      }
    }
    return std::string(url);
  } catch (...) {
    return std::string(url);
  }
}

std::string rewrite_bundle_url(
    const std::string_view url, const std::filesystem::path& bundle_directory,
    const BundleMaterializer& materialize_bundle) noexcept {
  return rewrite_bundle_url(
      url, bundle_directory,
      [](const std::filesystem::path& path) {
        std::error_code error;
        return std::filesystem::is_regular_file(path, error) && !error;
      },
      materialize_bundle);
}

std::string rewrite_ini_url(const std::string_view url,
                            const std::string_view local_ini_path) noexcept {
  try {
    if (local_ini_path.empty()) {
      return std::string(url);
    }
    constexpr std::string_view marker = "env-list.ini";
    if (url.find(marker) == std::string_view::npos) {
      return std::string(url);
    }
    std::error_code error;
    if (!std::filesystem::is_regular_file(local_ini_path, error) || error) {
      return std::string(url);
    }
    return std::string("file://") +
           std::filesystem::path(local_ini_path)
               .lexically_normal()
               .generic_string();
  } catch (...) {
    return std::string(url);
  }
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

bool invoke_request_delegate(const RuntimeCallbacks& callbacks,
                             void* delegate_object, void* delegate_target,
                             void* request_object,
                             void* response_object) noexcept {
  if (callbacks.request_delegate_invoker == nullptr) {
    return false;
  }
  return callbacks.request_delegate_invoker(
      delegate_object, delegate_target, request_object, response_object,
      callbacks.user_data);
}

bool configure(CoreDirectories directories,
               RuntimeCallbacks callbacks) noexcept {
  try {
    if (observer_armed.load(std::memory_order_acquire) ||
        callbacks.hook_installer == nullptr ||
        callbacks.request_delegate_invoker == nullptr ||
        callbacks.managed_class_resolver == nullptr ||
        !signature_is_set(callbacks.signatures.http_send) ||
        !signature_is_set(callbacks.signatures.ini_load_from_url) ||
        !signature_is_set(callbacks.signatures.do_game_service_login)) {
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

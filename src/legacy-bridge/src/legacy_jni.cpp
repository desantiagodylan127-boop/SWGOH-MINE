#include <jni.h>

#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <android/log.h>
#include <dlfcn.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "shadowhook.h"
#include "heroes/offline/il2cpp_bridge.h"
#include "heroes/offline/offline_core.h"

namespace {

namespace bridge = heroes::offline::il2cpp_bridge;

constexpr char kLogTag[] = "offlinecore";
constexpr char kContentVersion[] = "0.40.2044608";
constexpr std::uint64_t kDeterministicSeed = 0x4845524f45534f46ULL;
constexpr std::size_t kDelegateMethodInfoOffset = 0x28;

std::mutex initialization_mutex;
std::atomic<bool> core_initialized{false};

class UtfChars final {
 public:
  UtfChars(JNIEnv* env, jstring value) noexcept : env_(env), value_(value) {
    if (env_ != nullptr && value_ != nullptr) {
      chars_ = env_->GetStringUTFChars(value_, nullptr);
    }
  }

  ~UtfChars() {
    if (chars_ != nullptr) {
      env_->ReleaseStringUTFChars(value_, chars_);
    }
  }

  UtfChars(const UtfChars&) = delete;
  UtfChars& operator=(const UtfChars&) = delete;

  [[nodiscard]] const char* get() const noexcept { return chars_; }
  [[nodiscard]] bool valid() const noexcept { return chars_ != nullptr; }
  [[nodiscard]] bool empty() const noexcept {
    return chars_ == nullptr || chars_[0] == '\0';
  }

 private:
  JNIEnv* env_;
  jstring value_;
  const char* chars_ = nullptr;
};

void log_error(const char* operation) noexcept {
  const char* detail = ho_last_error();
  __android_log_print(ANDROID_LOG_ERROR, kLogTag, "%s failed: %s", operation,
                      detail != nullptr ? detail : "unknown error");
}

class ShadowHookInstaller final : public bridge::HookInstaller {
 public:
  bool install(void* target, void* replacement,
               void** original) noexcept override {
    if (target == nullptr || replacement == nullptr || original == nullptr) {
      return false;
    }
    void* stub = shadowhook_hook_func_addr(target, replacement, original);
    if (stub == nullptr) {
      return false;
    }
    const std::lock_guard lock(mutex_);
    stubs_[target] = stub;
    return true;
  }

  void rollback(void* target) noexcept override {
    void* stub = nullptr;
    {
      const std::lock_guard lock(mutex_);
      const auto found = stubs_.find(target);
      if (found != stubs_.end()) {
        stub = found->second;
        stubs_.erase(found);
      }
    }
    if (stub != nullptr) {
      (void)shadowhook_unhook(stub);
    }
  }

 private:
  std::mutex mutex_;
  std::unordered_map<void*, void*> stubs_;
};

ShadowHookInstaller hook_installer;

template <typename Function>
Function resolve_il2cpp(const char* name) noexcept {
  void* symbol = dlsym(RTLD_DEFAULT, name);
  if (symbol == nullptr) {
    void* handle = dlopen("libil2cpp.so", RTLD_NOW | RTLD_NOLOAD);
    if (handle != nullptr) {
      symbol = dlsym(handle, name);
      dlclose(handle);
    }
  }
  return reinterpret_cast<Function>(symbol);
}

void* resolve_managed_class(std::string_view name_space, std::string_view name,
                            void*) noexcept {
  try {
    using DomainGet = void* (*)();
    using DomainGetAssemblies = const void** (*)(const void*, std::size_t*);
    using AssemblyGetImage = void* (*)(const void*);
    using ClassFromName = void* (*)(const void*, const char*, const char*);

    const auto domain_get =
        resolve_il2cpp<DomainGet>("il2cpp_domain_get");
    const auto domain_get_assemblies =
        resolve_il2cpp<DomainGetAssemblies>("il2cpp_domain_get_assemblies");
    const auto assembly_get_image =
        resolve_il2cpp<AssemblyGetImage>("il2cpp_assembly_get_image");
    const auto class_from_name =
        resolve_il2cpp<ClassFromName>("il2cpp_class_from_name");
    if (domain_get == nullptr || domain_get_assemblies == nullptr ||
        assembly_get_image == nullptr || class_from_name == nullptr) {
      return nullptr;
    }

    void* domain = domain_get();
    std::size_t assembly_count = 0;
    const void** assemblies =
        domain != nullptr ? domain_get_assemblies(domain, &assembly_count)
                          : nullptr;
    if (assemblies == nullptr) {
      return nullptr;
    }

    const std::string namespace_text(name_space);
    const std::string name_text(name);
    for (std::size_t index = 0; index < assembly_count; ++index) {
      void* image = assembly_get_image(assemblies[index]);
      if (image != nullptr) {
        void* klass =
            class_from_name(image, namespace_text.c_str(), name_text.c_str());
        if (klass != nullptr) {
          return klass;
        }
      }
    }
  } catch (...) {
  }
  return nullptr;
}

bool invoke_delegate(void* delegate_object, void* delegate_target,
                     void* request_object, void* response_object,
                     void*) noexcept {
  try {
    using RuntimeInvoke = void* (*)(const void*, void*, void**, void**);
    const auto runtime_invoke =
        resolve_il2cpp<RuntimeInvoke>("il2cpp_runtime_invoke");
    if (runtime_invoke == nullptr || delegate_object == nullptr) {
      return false;
    }

    const void* method_info = nullptr;
    std::memcpy(&method_info,
                static_cast<const std::byte*>(delegate_object) +
                    kDelegateMethodInfoOffset,
                sizeof(method_info));
    if (method_info == nullptr) {
      return false;
    }

    void* parameters[] = {request_object, response_object};
    void* exception = nullptr;
    (void)runtime_invoke(method_info, delegate_target, parameters, &exception);
    return exception == nullptr;
  } catch (...) {
    return false;
  }
}

bool reload_configured_content(const bridge::CoreDirectories& directories,
                               void*) noexcept {
  try {
    return !directories.pack_path.empty() &&
           ho_reload_content(directories.pack_path.c_str()) == HO_STATUS_OK;
  } catch (...) {
    return false;
  }
}

bool valid_required_inputs(JNIEnv* env, jstring storage, jstring cache,
                           jstring apk, jstring bundle,
                           jobject asset_manager) noexcept {
  return env != nullptr && storage != nullptr && cache != nullptr &&
         apk != nullptr && bundle != nullptr && asset_manager != nullptr &&
         AAssetManager_fromJava(env, asset_manager) != nullptr;
}

bool extract_asset_file(AAssetManager* assets, const char* asset_path,
                        const std::filesystem::path& destination) noexcept {
  try {
    if (assets == nullptr || asset_path == nullptr || asset_path[0] == '\0') {
      return false;
    }
    AAsset* asset =
        AAssetManager_open(assets, asset_path, AASSET_MODE_STREAMING);
    if (asset == nullptr) {
      return false;
    }
    std::error_code error;
    std::filesystem::create_directories(destination.parent_path(), error);
    if (error) {
      AAsset_close(asset);
      return false;
    }
    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    if (!output) {
      AAsset_close(asset);
      return false;
    }
    std::array<char, 1 << 16> buffer{};
    int read_bytes = 0;
    while ((read_bytes = AAsset_read(asset, buffer.data(), buffer.size())) >
           0) {
      output.write(buffer.data(), read_bytes);
      if (!output) {
        AAsset_close(asset);
        return false;
      }
    }
    AAsset_close(asset);
    output.close();
    return read_bytes == 0 && output.good();
  } catch (...) {
    return false;
  }
}

std::string ensure_pack_path(const std::filesystem::path& storage,
                             AAssetManager* assets) {
  const std::filesystem::path candidate = storage / "offline-content.pack";
  std::error_code file_error;
  if (std::filesystem::is_regular_file(candidate, file_error) && !file_error) {
    return candidate.string();
  }
  if (extract_asset_file(assets, "offline/offline-content.pack", candidate)) {
    return candidate.string();
  }
  return {};
}

std::string ensure_ini_path(const std::filesystem::path& storage,
                            AAssetManager* assets) {
  const std::filesystem::path candidate = storage / "env-list.ini";
  std::error_code file_error;
  if (std::filesystem::is_regular_file(candidate, file_error) && !file_error) {
    return candidate.string();
  }
  if (extract_asset_file(assets, "offline/env-list.ini", candidate)) {
    return candidate.string();
  }
  return {};
}

std::mutex asset_mutex;
AAssetManager* retained_assets = nullptr;
std::string retained_bundle_directory;

bool materialize_bundle_file(const char* basename, char* output_path,
                             const std::size_t output_capacity,
                             void*) noexcept {
  try {
    if (basename == nullptr || basename[0] == '\0' || output_path == nullptr ||
        output_capacity == 0) {
      return false;
    }
    const std::string name(basename);
    if (name.find('/') != std::string::npos ||
        name.find('\\') != std::string::npos || name == "." || name == "..") {
      return false;
    }

    const std::lock_guard lock(asset_mutex);
    if (retained_assets == nullptr || retained_bundle_directory.empty()) {
      return false;
    }
    const std::filesystem::path destination =
        std::filesystem::path(retained_bundle_directory) / name;
    std::error_code error;
    if (std::filesystem::is_regular_file(destination, error) && !error) {
      const std::string text = destination.string();
      if (text.size() + 1 > output_capacity) {
        return false;
      }
      std::memcpy(output_path, text.c_str(), text.size() + 1);
      return true;
    }

    const std::string asset_path = "offline/UnityBundles/" + name;
    if (!extract_asset_file(retained_assets, asset_path.c_str(), destination)) {
      return false;
    }
    const std::string text = destination.string();
    if (text.size() + 1 > output_capacity) {
      return false;
    }
    std::memcpy(output_path, text.c_str(), text.size() + 1);
    return true;
  } catch (...) {
    return false;
  }
}

}  // namespace

extern "C" JNIEXPORT jboolean JNICALL
Java_local_swgoh_heroesoffline2_OfflineBootstrapProvider_nativeInitialize(
    JNIEnv* env, jclass, jstring storage_value, jstring cache_value,
    jstring apk_value, jstring bundle_value, jobject asset_manager) {
  try {
    if (!valid_required_inputs(env, storage_value, cache_value, apk_value,
                               bundle_value, asset_manager)) {
      return JNI_FALSE;
    }

    UtfChars storage(env, storage_value);
    UtfChars cache(env, cache_value);
    UtfChars apk(env, apk_value);
    UtfChars bundle(env, bundle_value);
    if (!storage.valid() || !cache.valid() || !apk.valid() ||
        !bundle.valid() || storage.empty() || cache.empty() || apk.empty() ||
        bundle.empty()) {
      return JNI_FALSE;
    }

    AAssetManager* native_assets =
        AAssetManager_fromJava(env, asset_manager);
    if (native_assets == nullptr) {
      return JNI_FALSE;
    }

    const std::lock_guard lock(initialization_mutex);
    if (core_initialized.load(std::memory_order_acquire)) {
      return bridge::armed() ? JNI_TRUE : JNI_FALSE;
    }

    (void)ho_shutdown();
    const std::string pack_path =
        ensure_pack_path(storage.get(), native_assets);
    const std::string ini_path = ensure_ini_path(storage.get(), native_assets);

    {
      const std::lock_guard assets_lock(asset_mutex);
      retained_assets = native_assets;
      retained_bundle_directory = bundle.get();
    }
    std::error_code directory_error;
    std::filesystem::create_directories(bundle.get(), directory_error);

    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    const ho_init_options options{
        .storage_directory = storage.get(),
        .cache_directory = cache.get(),
        .pack_path = pack_path.empty() ? nullptr : pack_path.c_str(),
        .bundle_directory = bundle.get(),
        .content_version = kContentVersion,
        .initial_utc_seconds = static_cast<std::int64_t>(now),
        .ai_seed = kDeterministicSeed,
    };
    if (ho_initialize(&options) != HO_STATUS_OK) {
      log_error("ho_initialize");
      return JNI_FALSE;
    }
    if (shadowhook_init(SHADOWHOOK_MODE_UNIQUE, false) != 0) {
      __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                          "shadowhook engine init failed: %s",
                          shadowhook_to_errmsg(shadowhook_get_init_errno()));
      (void)ho_shutdown();
      return JNI_FALSE;
    }

    bridge::CoreDirectories directories{
        .storage_directory = storage.get(),
        .cache_directory = cache.get(),
        .pack_path = pack_path,
        .bundle_directory = bundle.get(),
        .apk_path = apk.get(),
        .local_ini_path = ini_path,
    };
    bridge::RuntimeCallbacks callbacks;
    callbacks.hook_installer = &hook_installer;
    callbacks.request_delegate_invoker = &invoke_delegate;
    callbacks.managed_class_resolver = &resolve_managed_class;
    callbacks.reload_content = &reload_configured_content;
    callbacks.materialize_bundle = &materialize_bundle_file;
    callbacks.signatures.http_send =
        std::array<std::uint8_t, 8>{0xfe, 0x57, 0xbe, 0xa9,
                                    0xf4, 0x4f, 0x01, 0xa9};
    callbacks.signatures.asset_bundle_one =
        std::array<std::uint8_t, 8>{0xfe, 0x67, 0xbc, 0xa9,
                                    0xf8, 0x5f, 0x01, 0xa9};
    callbacks.signatures.asset_bundle_two =
        std::array<std::uint8_t, 8>{0xfe, 0x0f, 0x1b, 0xf8,
                                    0xfa, 0x67, 0x01, 0xa9};
    callbacks.signatures.do_game_service_login =
        std::array<std::uint8_t, 8>{0xfe, 0x67, 0xbc, 0xa9,
                                    0xf8, 0x5f, 0x01, 0xa9};
    callbacks.signatures.ini_load_from_url =
        std::array<std::uint8_t, 8>{0xfe, 0x67, 0xbc, 0xa9,
                                    0xf8, 0x5f, 0x01, 0xa9};
    callbacks.signatures.import_account_view_ready =
        std::array<std::uint8_t, 8>{0xfe, 0x5f, 0xbd, 0xa9,
                                    0xf6, 0x57, 0x01, 0xa9};
    if (!bridge::configure(std::move(directories), callbacks) ||
        !bridge::start_observer()) {
      (void)ho_shutdown();
      return JNI_FALSE;
    }

    core_initialized.store(true, std::memory_order_release);
    return JNI_TRUE;
  } catch (...) {
    (void)ho_shutdown();
    core_initialized.store(false, std::memory_order_release);
    return JNI_FALSE;
  }
}

extern "C" JNIEXPORT jboolean JNICALL
Java_local_swgoh_heroesoffline2_OfflineBootstrapActivity_nativeBridgeArmed(
    JNIEnv*, jclass) {
  try {
    return core_initialized.load(std::memory_order_acquire) && bridge::armed()
               ? JNI_TRUE
               : JNI_FALSE;
  } catch (...) {
    return JNI_FALSE;
  }
}

extern "C" JNIEXPORT jboolean JNICALL
Java_local_swgoh_heroesoffline2_OfflineBootstrapActivity_nativeContentReady(
    JNIEnv* env, jclass, jstring pack_value, jstring bundle_value,
    jstring cache_value, jobject asset_manager) {
  try {
    if (env == nullptr || pack_value == nullptr || bundle_value == nullptr ||
        cache_value == nullptr || asset_manager == nullptr ||
        AAssetManager_fromJava(env, asset_manager) == nullptr ||
        !core_initialized.load(std::memory_order_acquire)) {
      return JNI_FALSE;
    }

    UtfChars pack(env, pack_value);
    UtfChars bundle(env, bundle_value);
    UtfChars cache(env, cache_value);
    if (!pack.valid() || !bundle.valid() || !cache.valid() || pack.empty() ||
        bundle.empty() || cache.empty()) {
      return JNI_FALSE;
    }

    // The bridge intentionally freezes its directories after start_observer().
    // Reload the core now; future bridge directory mutation requires a bridge
    // API that does not exist in the current contract.
    return ho_reload_content(pack.get()) == HO_STATUS_OK ? JNI_TRUE
                                                         : JNI_FALSE;
  } catch (...) {
    return JNI_FALSE;
  }
}

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM*, void*) {
  try {
    return JNI_VERSION_1_6;
  } catch (...) {
    return JNI_ERR;
  }
}

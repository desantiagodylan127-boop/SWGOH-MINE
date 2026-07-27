#include <jni.h>

#include "heroes/offline/offline_core.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>

namespace {

constexpr char kContentVersion[] = "0.40.2044608";
constexpr std::uint64_t kDeterministicSeed = 0x4845524f45534f46ULL;
constexpr char kResultClass[] =
    "local/swgoh/heroesoffline/source/NativeCore$Result";

class UtfChars final {
 public:
  UtfChars(JNIEnv* env, jstring value) noexcept : env_(env), value_(value) {
    chars_ = env_->GetStringUTFChars(value_, nullptr);
  }

  ~UtfChars() {
    if (chars_ != nullptr) {
      env_->ReleaseStringUTFChars(value_, chars_);
    }
  }

  UtfChars(const UtfChars&) = delete;
  UtfChars& operator=(const UtfChars&) = delete;

  [[nodiscard]] const char* get() const noexcept { return chars_; }
  [[nodiscard]] bool empty() const noexcept {
    return chars_ == nullptr || chars_[0] == '\0';
  }

 private:
  JNIEnv* env_;
  jstring value_;
  const char* chars_ = nullptr;
};

class OwnedResponse final {
 public:
  OwnedResponse() = default;
  ~OwnedResponse() { ho_free(value.data); }

  OwnedResponse(const OwnedResponse&) = delete;
  OwnedResponse& operator=(const OwnedResponse&) = delete;

  ho_owned_bytes value{};
};

void ThrowJava(JNIEnv* env, const char* class_name,
               const char* message) noexcept {
  if (env->ExceptionCheck()) {
    return;
  }
  jclass exception_class = env->FindClass(class_name);
  if (exception_class != nullptr) {
    env->ThrowNew(exception_class, message);
    env->DeleteLocalRef(exception_class);
  }
}

void ThrowCoreFailure(JNIEnv* env, const char* operation,
                      ho_status status) noexcept {
  const char* detail = ho_last_error();
  if (detail == nullptr || detail[0] == '\0') {
    detail = "<empty>";
  }
  char message[1400];
  std::snprintf(message, sizeof(message),
                "%s failed (offline-core status %d; ho_last_error: %s)",
                operation, static_cast<int>(status), detail);
  ThrowJava(env, "java/lang/IllegalStateException", message);
}

void ThrowNativeFailure(JNIEnv* env, const char* operation,
                        const char* detail) noexcept {
  const char* core_detail = ho_last_error();
  if (core_detail == nullptr || core_detail[0] == '\0') {
    core_detail = "<empty>";
  }
  if (detail == nullptr || detail[0] == '\0') {
    detail = "<unknown>";
  }
  char message[1800];
  std::snprintf(message, sizeof(message),
                "%s failed (%s; ho_last_error: %s)", operation, detail,
                core_detail);
  ThrowJava(env, "java/lang/RuntimeException", message);
}

jobject NewResult(JNIEnv* env, const char* message,
                  jint checked_rpc_count) noexcept {
  jclass result_class = env->FindClass(kResultClass);
  if (result_class == nullptr) {
    return nullptr;
  }
  jmethodID constructor =
      env->GetMethodID(result_class, "<init>", "(ZLjava/lang/String;I)V");
  if (constructor == nullptr) {
    env->DeleteLocalRef(result_class);
    return nullptr;
  }
  jstring result_message = env->NewStringUTF(message);
  if (result_message == nullptr) {
    env->DeleteLocalRef(result_class);
    return nullptr;
  }
  jobject result = env->NewObject(result_class, constructor, JNI_TRUE,
                                  result_message, checked_rpc_count);
  env->DeleteLocalRef(result_message);
  env->DeleteLocalRef(result_class);
  return result;
}

bool RequireText(JNIEnv* env, const UtfChars& text,
                 const char* parameter_name) noexcept {
  if (text.get() != nullptr && !text.empty()) {
    return true;
  }
  if (!env->ExceptionCheck()) {
    char message[160];
    std::snprintf(message, sizeof(message), "%s must not be empty",
                  parameter_name);
    ThrowJava(env, "java/lang/IllegalArgumentException", message);
  }
  return false;
}

}  // namespace

extern "C" JNIEXPORT jobject JNICALL
Java_local_swgoh_heroesoffline_source_NativeCore_nativeInitialize(
    JNIEnv* env, jclass, jstring storage_dir, jstring cache_dir,
    jstring pack_path, jstring bundle_dir) {
  try {
    if (storage_dir == nullptr || cache_dir == nullptr ||
        pack_path == nullptr || bundle_dir == nullptr) {
      ThrowJava(env, "java/lang/IllegalArgumentException",
                "storageDir, cacheDir, packPath, and bundleDir are required");
      return nullptr;
    }

    UtfChars storage(env, storage_dir);
    if (!RequireText(env, storage, "storageDir")) {
      return nullptr;
    }
    UtfChars cache(env, cache_dir);
    if (!RequireText(env, cache, "cacheDir")) {
      return nullptr;
    }
    UtfChars pack(env, pack_path);
    if (!RequireText(env, pack, "packPath")) {
      return nullptr;
    }
    UtfChars bundle(env, bundle_dir);
    if (!RequireText(env, bundle, "bundleDir")) {
      return nullptr;
    }

    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    const ho_init_options options{
        .storage_directory = storage.get(),
        .cache_directory = cache.get(),
        .pack_path = pack.get(),
        .bundle_directory = bundle.get(),
        .content_version = kContentVersion,
        .initial_utc_seconds = static_cast<std::int64_t>(now),
        .ai_seed = kDeterministicSeed,
    };
    const ho_status status = ho_initialize(&options);
    if (status != HO_STATUS_OK) {
      ThrowCoreFailure(env, "initialize", status);
      return nullptr;
    }
    return NewResult(env, "offline core initialized", 0);
  } catch (const std::exception& exception) {
    ThrowNativeFailure(env, "initialize", exception.what());
    return nullptr;
  } catch (...) {
    ThrowNativeFailure(env, "initialize", "unknown native exception");
    return nullptr;
  }
}

extern "C" JNIEXPORT jobject JNICALL
Java_local_swgoh_heroesoffline_source_NativeCore_nativeSelfTest(
    JNIEnv* env, jclass) {
  try {
    struct Rpc {
      const char* service;
      const char* method;
    };
    constexpr std::array<Rpc, 4> kRpcs{{
        {"AuthRpc", "DoAuthGuest"},
        {"PlayerRpc", "GetInitialData"},
        {"ContentRpc", "GetMetadata"},
        {"BattleRpc", "BattleStart"},
    }};

    for (const Rpc& rpc : kRpcs) {
      OwnedResponse response;
      const ho_status status =
          ho_dispatch_rpc(rpc.service, rpc.method, nullptr, 0, &response.value);
      if (status != HO_STATUS_OK) {
        char operation[192];
        std::snprintf(operation, sizeof(operation), "selfTest %s/%s",
                      rpc.service, rpc.method);
        ThrowCoreFailure(env, operation, status);
        return nullptr;
      }
      if (response.value.data == nullptr || response.value.size == 0) {
        char detail[256];
        std::snprintf(detail, sizeof(detail),
                      "%s/%s returned an empty response envelope", rpc.service,
                      rpc.method);
        ThrowNativeFailure(env, "selfTest", detail);
        return nullptr;
      }
    }

    return NewResult(env, "4 offline RPC envelopes validated", 4);
  } catch (const std::exception& exception) {
    ThrowNativeFailure(env, "selfTest", exception.what());
    return nullptr;
  } catch (...) {
    ThrowNativeFailure(env, "selfTest", "unknown native exception");
    return nullptr;
  }
}

extern "C" JNIEXPORT void JNICALL
Java_local_swgoh_heroesoffline_source_NativeCore_nativeShutdown(
    JNIEnv* env, jclass) {
  try {
    const ho_status status = ho_shutdown();
    if (status != HO_STATUS_OK) {
      ThrowCoreFailure(env, "shutdown", status);
    }
  } catch (const std::exception& exception) {
    ThrowNativeFailure(env, "shutdown", exception.what());
  } catch (...) {
    ThrowNativeFailure(env, "shutdown", "unknown native exception");
  }
}

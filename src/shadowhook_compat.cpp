#include <android/log.h>
#include <dlfcn.h>
#include <link.h>
#include <pthread.h>
#include <unistd.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "dobby.h"

namespace {

constexpr char kLogTag[] = "HeroesOfflineHook";
constexpr char kTargetLibrary[] = "libil2cpp.so";
constexpr std::size_t kMaxCallbacks = 8;

enum ErrorCode {
  kOk = 0,
  kInvalidArgument = 1,
  kHookFailed = 2,
  kCallbackTableFull = 3,
};

using DlCallback = void (*)(dl_phdr_info* info, std::size_t size, void* data);
using AndroidDlopenExt = void* (*)(const char* filename, int flags, const void* extinfo);
using Dlopen = void* (*)(const char* filename, int flags);

struct CallbackRegistration {
  DlCallback pre;
  DlCallback post;
  void* data;
};

pthread_mutex_t g_callbacks_mutex = PTHREAD_MUTEX_INITIALIZER;
CallbackRegistration g_callbacks[kMaxCallbacks]{};
std::size_t g_callback_count = 0;

std::atomic<int> g_init_result{kInvalidArgument};
std::atomic<bool> g_initialized{false};
std::atomic<bool> g_target_notified{false};
std::atomic<bool> g_worker_started{false};
thread_local int g_last_error = kOk;
thread_local bool g_inside_loader_proxy = false;

AndroidDlopenExt g_original_android_dlopen_ext = nullptr;
Dlopen g_original_dlopen = nullptr;

bool is_target_library(const char* path) {
  if (path == nullptr) {
    return false;
  }
  const char* basename = std::strrchr(path, '/');
  basename = basename == nullptr ? path : basename + 1;
  return std::strcmp(basename, kTargetLibrary) == 0;
}

void notify_callbacks(dl_phdr_info* info, std::size_t size) {
  CallbackRegistration callbacks[kMaxCallbacks]{};
  std::size_t callback_count = 0;

  pthread_mutex_lock(&g_callbacks_mutex);
  callback_count = g_callback_count;
  if (callback_count > 0) {
    std::memcpy(callbacks, g_callbacks, callback_count * sizeof(CallbackRegistration));
  }
  pthread_mutex_unlock(&g_callbacks_mutex);

  // Do not consume the one notification before OfflineCore has registered.
  if (callback_count == 0 || g_target_notified.exchange(true)) {
    return;
  }

  g_inside_loader_proxy = true;
  for (std::size_t i = 0; i < callback_count; ++i) {
    if (callbacks[i].pre != nullptr) {
      callbacks[i].pre(info, size, callbacks[i].data);
    }
    if (callbacks[i].post != nullptr) {
      callbacks[i].post(info, size, callbacks[i].data);
    }
  }
  g_inside_loader_proxy = false;
}

int find_and_notify_target(dl_phdr_info* info, std::size_t size, void*) {
  if (is_target_library(info->dlpi_name)) {
    notify_callbacks(info, size);
    return 1;
  }
  return 0;
}

void scan_loaded_libraries() {
  if (!g_target_notified.load()) {
    dl_iterate_phdr(find_and_notify_target, nullptr);
  }
}

void* callback_worker(void*) {
  // Loader interception is the primary path. Polling is a bounded-cost fallback
  // for Android builds that bypass public libdl entry points.
  for (unsigned int attempts = 0; !g_target_notified.load(); ++attempts) {
    scan_loaded_libraries();
    usleep(attempts < 30000 ? 1000 : 100000);
  }
  return nullptr;
}

void start_callback_worker() {
  if (g_worker_started.exchange(true)) {
    return;
  }

  pthread_t thread{};
  if (pthread_create(&thread, nullptr, callback_worker, nullptr) == 0) {
    pthread_detach(thread);
  } else {
    g_worker_started.store(false);
    __android_log_print(ANDROID_LOG_WARN, kLogTag, "Could not start loader fallback worker");
  }
}

void scan_after_load(void* handle) {
  if (handle != nullptr && !g_inside_loader_proxy) {
    g_inside_loader_proxy = true;
    scan_loaded_libraries();
    g_inside_loader_proxy = false;
  }
}

void* android_dlopen_ext_proxy(const char* filename, int flags, const void* extinfo) {
  void* handle = g_original_android_dlopen_ext(filename, flags, extinfo);
  scan_after_load(handle);
  return handle;
}

void* dlopen_proxy(const char* filename, int flags) {
  void* handle = g_original_dlopen(filename, flags);
  scan_after_load(handle);
  return handle;
}

void install_loader_monitors() {
  void* android_dlopen_ext_address = dlsym(RTLD_DEFAULT, "android_dlopen_ext");
  void* dlopen_address = dlsym(RTLD_DEFAULT, "dlopen");

  if (android_dlopen_ext_address != nullptr &&
      DobbyHook(android_dlopen_ext_address, reinterpret_cast<void*>(android_dlopen_ext_proxy),
                reinterpret_cast<void**>(&g_original_android_dlopen_ext)) != 0) {
    g_original_android_dlopen_ext = nullptr;
  }

  if (dlopen_address != nullptr &&
      DobbyHook(dlopen_address, reinterpret_cast<void*>(dlopen_proxy),
                reinterpret_cast<void**>(&g_original_dlopen)) != 0) {
    g_original_dlopen = nullptr;
  }

  if (g_original_android_dlopen_ext == nullptr && g_original_dlopen == nullptr) {
    __android_log_print(ANDROID_LOG_WARN, kLogTag,
                        "Public loader hooks unavailable; using phdr polling fallback");
  }
}

}  // namespace

extern "C" __attribute__((visibility("default"))) int shadowhook_init(int, bool) {
  bool expected = false;
  if (g_initialized.compare_exchange_strong(expected, true)) {
    install_loader_monitors();
    g_init_result.store(kOk);
  }
  g_last_error = g_init_result.load();
  return g_last_error;
}

extern "C" __attribute__((visibility("default"))) int shadowhook_get_errno() {
  return g_last_error;
}

extern "C" __attribute__((visibility("default"))) const char* shadowhook_to_errmsg(
    int error_number) {
  switch (error_number) {
    case kOk:
      return "OK";
    case kInvalidArgument:
      return "Invalid argument";
    case kHookFailed:
      return "Dobby inline hook failed";
    case kCallbackTableFull:
      return "Dynamic-loader callback table is full";
    default:
      return "Unknown compatibility-shim error";
  }
}

extern "C" __attribute__((visibility("default"))) void* shadowhook_hook_func_addr(
    void* function_address, void* replacement_address, void** original_address) {
  if (function_address == nullptr || replacement_address == nullptr || original_address == nullptr) {
    g_last_error = kInvalidArgument;
    return nullptr;
  }

  if (DobbyHook(function_address, replacement_address, original_address) != 0) {
    g_last_error = kHookFailed;
    return nullptr;
  }

  g_last_error = kOk;
  return function_address;
}

extern "C" __attribute__((visibility("default"))) int shadowhook_register_dl_init_callback(
    DlCallback pre, DlCallback post, void* data) {
  if (pre == nullptr && post == nullptr) {
    g_last_error = kInvalidArgument;
    return -1;
  }

  pthread_mutex_lock(&g_callbacks_mutex);
  if (g_callback_count == kMaxCallbacks) {
    pthread_mutex_unlock(&g_callbacks_mutex);
    g_last_error = kCallbackTableFull;
    return -1;
  }
  g_callbacks[g_callback_count++] = {pre, post, data};
  pthread_mutex_unlock(&g_callbacks_mutex);

  g_last_error = kOk;
  scan_loaded_libraries();
  start_callback_worker();
  return 0;
}

extern "C" __attribute__((visibility("default"))) void* shadowhook_dlopen(
    const char* library_name) {
  if (library_name == nullptr) {
    g_last_error = kInvalidArgument;
    return nullptr;
  }

  void* handle = dlopen(library_name, RTLD_NOW | RTLD_NOLOAD);
  if (handle == nullptr) {
    handle = dlopen(library_name, RTLD_NOW);
  }
  g_last_error = handle == nullptr ? kInvalidArgument : kOk;
  return handle;
}

extern "C" __attribute__((visibility("default"))) void* shadowhook_dlsym(
    void* handle, const char* symbol_name) {
  if (handle == nullptr || symbol_name == nullptr) {
    g_last_error = kInvalidArgument;
    return nullptr;
  }

  void* address = dlsym(handle, symbol_name);
  if (address == nullptr) {
    address = DobbySymbolResolver(nullptr, symbol_name);
  }
  g_last_error = address == nullptr ? kInvalidArgument : kOk;
  return address;
}

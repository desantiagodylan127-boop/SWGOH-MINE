#include "heroes/offline/offline_core.h"

#include "heroes/offline/content_pack.h"
#include "heroes/offline/rpc_registry.h"
#include "heroes/offline/state.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace {

using heroes::offline::ContentPack;
using heroes::offline::PlayerState;
using heroes::offline::RpcRegistry;
using heroes::offline::StateStore;

constexpr std::size_t kErrorCapacity = 1024;
thread_local std::array<char, kErrorCapacity> last_error{};

void clear_error() noexcept { last_error[0] = '\0'; }

void set_error(const char* message) noexcept {
  if (message == nullptr) {
    clear_error();
    return;
  }
  std::size_t index = 0;
  while (index + 1 < last_error.size() && message[index] != '\0') {
    last_error[index] = message[index];
    ++index;
  }
  last_error[index] = '\0';
}

ho_status fail(const ho_status status, const char* message) noexcept {
  set_error(message);
  return status;
}

struct Runtime {
  Runtime(const std::filesystem::path& storage_directory,
          PlayerState initial_state)
      : store(storage_directory), state(std::move(initial_state)) {}

  StateStore store;
  PlayerState state;
  std::optional<ContentPack> pack;
  RpcRegistry registry;
};

std::mutex runtime_mutex;
std::unique_ptr<Runtime> runtime;

bool is_valid_content_version(const char* value) noexcept {
  if (value == nullptr || value[0] == '\0') {
    return false;
  }
  for (const auto* cursor = reinterpret_cast<const unsigned char*>(value);
       *cursor != '\0'; ++cursor) {
    if (*cursor < 0x20U || *cursor == 0x7fU) {
      return false;
    }
  }
  return true;
}

bool is_mutating_rpc(const std::string_view service,
                     const std::string_view method) noexcept {
  if (service == "BattleRpc") {
    return method == "BattleFinish";
  }
  if (service != "PlayerRpc") {
    return false;
  }
  return method == "TrackFtueStep" || method == "SkipTutorial" ||
         method == "UpdatePlayerPrefs" || method == "SetPlayerPref";
}

std::span<const std::uint8_t> request_span(const std::uint8_t* request,
                                           const std::size_t size) {
  if (size == 0) {
    return {};
  }
  return {request, size};
}

std::uint8_t* copy_response(const std::span<const std::uint8_t> bytes) {
  if (bytes.empty()) {
    return nullptr;
  }
  void* const allocation = std::malloc(bytes.size());
  if (allocation == nullptr) {
    throw std::bad_alloc();
  }
  std::memcpy(allocation, bytes.data(), bytes.size());
  return static_cast<std::uint8_t*>(allocation);
}

}  // namespace

extern "C" ho_status ho_initialize(const ho_init_options* options) {
  try {
    if (options == nullptr || options->storage_directory == nullptr ||
        options->storage_directory[0] == '\0' ||
        !is_valid_content_version(options->content_version)) {
      return fail(HO_STATUS_INVALID_ARGUMENT,
                  "storage_directory and content_version are required");
    }

    // These locations are part of the stable ABI for hosts that use them, but
    // the portable core does not need to materialize either directory.
    (void)options->cache_directory;
    (void)options->bundle_directory;

    const std::filesystem::path storage(options->storage_directory);
    std::error_code error;
    const bool save_exists =
        std::filesystem::exists(storage / "profile.hos", error);
    if (error) {
      return fail(HO_STATUS_INTERNAL_ERROR, error.message().c_str());
    }

    PlayerState initial_state;
    try {
      StateStore store(storage);
      initial_state = store.load_or_create(
          options->content_version, options->initial_utc_seconds,
          options->ai_seed);
    } catch (const std::exception& exception) {
      return fail(save_exists ? HO_STATUS_INVALID_SAVE
                              : HO_STATUS_INTERNAL_ERROR,
                  exception.what());
    }
    auto candidate =
        std::make_unique<Runtime>(storage, std::move(initial_state));

    if (options->pack_path != nullptr && options->pack_path[0] != '\0') {
      const std::filesystem::path pack_path(options->pack_path);
      const bool pack_exists = std::filesystem::exists(pack_path, error);
      if (error) {
        return fail(HO_STATUS_INTERNAL_ERROR, error.message().c_str());
      }
      if (pack_exists) {
        candidate->pack.emplace(pack_path);
      }
    }

    {
      const std::lock_guard lock(runtime_mutex);
      runtime = std::move(candidate);
    }
    clear_error();
    return HO_STATUS_OK;
  } catch (const std::invalid_argument& exception) {
    return fail(HO_STATUS_INVALID_ARGUMENT, exception.what());
  } catch (const std::exception& exception) {
    return fail(HO_STATUS_INTERNAL_ERROR, exception.what());
  } catch (...) {
    return fail(HO_STATUS_INTERNAL_ERROR, "unknown initialization error");
  }
}

extern "C" ho_status ho_shutdown(void) {
  try {
    const std::lock_guard lock(runtime_mutex);
    runtime.reset();
    clear_error();
    return HO_STATUS_OK;
  } catch (const std::exception& exception) {
    return fail(HO_STATUS_INTERNAL_ERROR, exception.what());
  } catch (...) {
    return fail(HO_STATUS_INTERNAL_ERROR, "unknown shutdown error");
  }
}

extern "C" ho_status ho_dispatch_rpc(const char* service, const char* method,
                                      const std::uint8_t* request,
                                      const std::size_t request_size,
                                      ho_owned_bytes* response) {
  if (response != nullptr) {
    response->data = nullptr;
    response->size = 0;
  }
  try {
    if (service == nullptr || service[0] == '\0' || method == nullptr ||
        method[0] == '\0' || response == nullptr ||
        (request == nullptr && request_size != 0)) {
      return fail(HO_STATUS_INVALID_ARGUMENT,
                  "service, method, request, or response is invalid");
    }

    const std::lock_guard lock(runtime_mutex);
    if (!runtime) {
      return fail(HO_STATUS_NOT_INITIALIZED,
                  "offline core is not initialized");
    }
    if (!runtime->registry.supports(service, method)) {
      return fail(HO_STATUS_UNSUPPORTED_RPC, "unsupported offline RPC");
    }

    const bool mutating = is_mutating_rpc(service, method);
    PlayerState working = runtime->state;
    const auto payload = runtime->registry.dispatch(
        service, method, request_span(request, request_size), working);
    const auto envelope = heroes::offline::make_response_envelope(
        payload, working.clock.utc_seconds);
    std::uint8_t* allocation = copy_response(envelope);
    if (mutating) {
      try {
        runtime->store.save(working);
      } catch (...) {
        std::free(allocation);
        throw;
      }
      runtime->state = std::move(working);
    }
    response->data = allocation;
    response->size = envelope.size();
    clear_error();
    return HO_STATUS_OK;
  } catch (const std::invalid_argument& exception) {
    return fail(HO_STATUS_INVALID_ARGUMENT, exception.what());
  } catch (const std::exception& exception) {
    return fail(HO_STATUS_INTERNAL_ERROR, exception.what());
  } catch (...) {
    return fail(HO_STATUS_INTERNAL_ERROR, "unknown RPC dispatch error");
  }
}

extern "C" ho_status ho_find_content(const char* relative_path,
                                      ho_content_location* location) {
  if (location != nullptr) {
    location->fd = -1;
    location->offset = 0;
    location->size = 0;
  }
  try {
    if (relative_path == nullptr || relative_path[0] == '\0' ||
        location == nullptr) {
      return fail(HO_STATUS_INVALID_ARGUMENT,
                  "relative_path and location are required");
    }

    const std::lock_guard lock(runtime_mutex);
    if (!runtime) {
      return fail(HO_STATUS_NOT_INITIALIZED,
                  "offline core is not initialized");
    }
    if (!runtime->pack) {
      return fail(HO_STATUS_INVALID_ARGUMENT, "no content pack is available");
    }
    const auto found = runtime->pack->find(relative_path);
    if (!found) {
      return fail(HO_STATUS_INVALID_ARGUMENT, "content path was not found");
    }
    location->fd = found->fd;
    location->offset = found->offset;
    location->size = found->size;
    clear_error();
    return HO_STATUS_OK;
  } catch (const std::exception& exception) {
    return fail(HO_STATUS_INTERNAL_ERROR, exception.what());
  } catch (...) {
    return fail(HO_STATUS_INTERNAL_ERROR, "unknown content lookup error");
  }
}

extern "C" ho_status ho_reload_content(const char* pack_path) {
  try {
    if (pack_path == nullptr || pack_path[0] == '\0') {
      return fail(HO_STATUS_INVALID_ARGUMENT, "pack_path is required");
    }
    ContentPack replacement(pack_path);
    const std::lock_guard lock(runtime_mutex);
    if (!runtime) {
      return fail(HO_STATUS_NOT_INITIALIZED,
                  "offline core is not initialized");
    }
    runtime->pack = std::move(replacement);
    clear_error();
    return HO_STATUS_OK;
  } catch (const std::invalid_argument& exception) {
    return fail(HO_STATUS_INVALID_ARGUMENT, exception.what());
  } catch (const std::exception& exception) {
    return fail(HO_STATUS_INTERNAL_ERROR, exception.what());
  } catch (...) {
    return fail(HO_STATUS_INTERNAL_ERROR, "unknown content reload error");
  }
}

extern "C" void ho_free(void* allocation) { std::free(allocation); }

extern "C" const char* ho_last_error(void) { return last_error.data(); }

#ifndef HEROES_OFFLINE_OFFLINE_CORE_H_
#define HEROES_OFFLINE_OFFLINE_CORE_H_

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#define HO_API __declspec(dllexport)
#else
#define HO_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ho_status {
  HO_STATUS_OK = 0,
  HO_STATUS_INVALID_ARGUMENT = 1,
  HO_STATUS_NOT_INITIALIZED = 2,
  HO_STATUS_UNSUPPORTED_RPC = 3,
  HO_STATUS_INVALID_SAVE = 4,
  HO_STATUS_INTERNAL_ERROR = 5
} ho_status;

typedef struct ho_owned_bytes {
  uint8_t* data;
  size_t size;
} ho_owned_bytes;

typedef struct ho_content_location {
  int fd;
  int64_t offset;
  int64_t size;
} ho_content_location;

typedef struct ho_init_options {
  const char* storage_directory;
  const char* cache_directory;
  const char* pack_path;
  const char* bundle_directory;
  const char* content_version;
  int64_t initial_utc_seconds;
  uint64_t ai_seed;
} ho_init_options;

HO_API ho_status ho_initialize(const ho_init_options* options);
HO_API ho_status ho_shutdown(void);

HO_API ho_status ho_dispatch_rpc(const char* service, const char* method,
                                 const uint8_t* request, size_t request_size,
                                 ho_owned_bytes* response);

/* The returned descriptor is borrowed and remains valid until shutdown or a
 * successful subsequent initialization. */
HO_API ho_status ho_find_content(const char* relative_path,
                                 ho_content_location* location);
HO_API ho_status ho_reload_content(const char* pack_path);

HO_API void ho_free(void* allocation);

/* The returned string is local to the calling thread and remains valid until
 * that thread makes another offline-core call. */
HO_API const char* ho_last_error(void);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#undef HO_API

#endif  /* HEROES_OFFLINE_OFFLINE_CORE_H_ */

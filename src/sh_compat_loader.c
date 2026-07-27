#include <dlfcn.h>
#include <link.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>

#include "shadowhook.h"

#define SH_COMPAT_MAX_CALLBACKS 8

typedef struct {
  shadowhook_dl_info_t pre;
  shadowhook_dl_info_t post;
  void *data;
} sh_compat_callback_t;

static pthread_mutex_t callbacks_lock = PTHREAD_MUTEX_INITIALIZER;
static sh_compat_callback_t callbacks[SH_COMPAT_MAX_CALLBACKS];
static size_t callbacks_count = 0;
static bool worker_started = false;
static bool target_notified = false;

static bool sh_compat_is_il2cpp(const char *path) {
  if (NULL == path) return false;
  const char *name = strrchr(path, '/');
  name = NULL == name ? path : name + 1;
  return 0 == strcmp(name, "libil2cpp.so");
}

static void sh_compat_notify(struct dl_phdr_info *info, size_t size) {
  sh_compat_callback_t snapshot[SH_COMPAT_MAX_CALLBACKS];
  size_t count;

  pthread_mutex_lock(&callbacks_lock);
  if (target_notified || 0 == callbacks_count) {
    pthread_mutex_unlock(&callbacks_lock);
    return;
  }
  target_notified = true;
  count = callbacks_count;
  memcpy(snapshot, callbacks, count * sizeof(sh_compat_callback_t));
  pthread_mutex_unlock(&callbacks_lock);

  for (size_t i = 0; i < count; ++i) {
    if (NULL != snapshot[i].pre) snapshot[i].pre(info, size, snapshot[i].data);
    if (NULL != snapshot[i].post) snapshot[i].post(info, size, snapshot[i].data);
  }
}

static int sh_compat_find_il2cpp(struct dl_phdr_info *info, size_t size, void *data) {
  (void)data;
  if (!sh_compat_is_il2cpp(info->dlpi_name)) return 0;

  // dl_iterate_phdr can expose an ELF before the loading thread has completed
  // relocation and constructors. Acquiring a NOLOAD handle on this worker
  // waits for the linker lock without loading a second copy. Never patch code
  // until that barrier succeeds.
  void *handle = dlopen(info->dlpi_name, RTLD_NOW | RTLD_NOLOAD);
  if (NULL == handle) return 0;
  dlclose(handle);

  sh_compat_notify(info, size);
  return 1;
}

static void sh_compat_scan(void) {
  pthread_mutex_lock(&callbacks_lock);
  bool done = target_notified;
  pthread_mutex_unlock(&callbacks_lock);
  if (!done) dl_iterate_phdr(sh_compat_find_il2cpp, NULL);
}

static void *sh_compat_worker(void *arg) {
  (void)arg;
  unsigned int attempts = 0;
  for (;;) {
    pthread_mutex_lock(&callbacks_lock);
    bool done = target_notified;
    pthread_mutex_unlock(&callbacks_lock);
    if (done) return NULL;

    sh_compat_scan();
    usleep(attempts++ < 30000 ? 1000 : 100000);
  }
}

int sh_compat_register_dl_init_callback(shadowhook_dl_info_t pre, shadowhook_dl_info_t post,
                                       void *data) {
  pthread_mutex_lock(&callbacks_lock);
  if (callbacks_count >= SH_COMPAT_MAX_CALLBACKS) {
    pthread_mutex_unlock(&callbacks_lock);
    return SHADOWHOOK_ERRNO_OOM;
  }
  callbacks[callbacks_count++] = (sh_compat_callback_t){.pre = pre, .post = post, .data = data};

  bool start_worker = !worker_started;
  if (start_worker) worker_started = true;
  pthread_mutex_unlock(&callbacks_lock);

  sh_compat_scan();
  if (start_worker) {
    pthread_t thread;
    if (0 != pthread_create(&thread, NULL, sh_compat_worker, NULL)) return SHADOWHOOK_ERRNO_OOM;
    pthread_detach(thread);
  }
  return SHADOWHOOK_ERRNO_OK;
}

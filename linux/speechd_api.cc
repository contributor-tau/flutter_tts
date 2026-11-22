#include "speechd_api.h"

#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace flutter_tts {

namespace {

// Sonames to try, most specific first. libspeechd.so.2 is the ABI used by
// every Speech Dispatcher release since 0.8; the unversioned name is a
// fallback for unusual setups where only the development symlink exists.
constexpr const char* kLibraryNames[] = {"libspeechd.so.2", "libspeechd.so"};

struct LoadResult {
  SpeechdApi api = {};
  bool ok = false;
  std::string error;
};

template <typename T>
bool LoadSymbol(void* handle, const char* name, T* out, std::string* missing) {
  void* symbol = dlsym(handle, name);
  *out = reinterpret_cast<T>(symbol);
  if (symbol == nullptr && missing != nullptr) {
    if (!missing->empty())
      missing->append(", ");
    missing->append(name);
  }
  return symbol != nullptr;
}

LoadResult Load() {
  LoadResult result;

  void* handle = nullptr;
  std::string dl_errors;
  for (const char* name : kLibraryNames) {
    handle = dlopen(name, RTLD_NOW | RTLD_LOCAL);
    if (handle != nullptr)
      break;
    const char* err = dlerror();
    if (!dl_errors.empty())
      dl_errors.append("; ");
    dl_errors.append(err != nullptr ? err : name);
  }
  if (handle == nullptr) {
    result.error =
        "Speech Dispatcher client library (libspeechd.so.2) not found: " +
        dl_errors;
    return result;
  }

  SpeechdApi& api = result.api;
  std::string missing;
  LoadSymbol(handle, "spd_open2", &api.open2, &missing);
  LoadSymbol(handle, "spd_close", &api.close, &missing);
  LoadSymbol(handle, "spd_say", &api.say, &missing);
  LoadSymbol(handle, "spd_cancel", &api.cancel, &missing);
  LoadSymbol(handle, "spd_pause", &api.pause, &missing);
  LoadSymbol(handle, "spd_resume", &api.resume, &missing);
  LoadSymbol(handle, "spd_set_notification_on", &api.set_notification_on,
             &missing);
  LoadSymbol(handle, "spd_set_voice_rate", &api.set_voice_rate, &missing);
  LoadSymbol(handle, "spd_set_voice_pitch", &api.set_voice_pitch, &missing);
  LoadSymbol(handle, "spd_set_volume", &api.set_volume, &missing);
  LoadSymbol(handle, "spd_set_language", &api.set_language, &missing);
  LoadSymbol(handle, "spd_set_synthesis_voice", &api.set_synthesis_voice,
             &missing);
  LoadSymbol(handle, "spd_set_voice_type", &api.set_voice_type, &missing);
  LoadSymbol(handle, "spd_list_synthesis_voices", &api.list_synthesis_voices,
             &missing);
  LoadSymbol(handle, "spd_list_modules", &api.list_modules, &missing);
  LoadSymbol(handle, "spd_set_output_module", &api.set_output_module, &missing);

  // Optional symbols.
  LoadSymbol(handle, "spd_get_language", &api.get_language, nullptr);
  LoadSymbol(handle, "spd_get_output_module", &api.get_output_module, nullptr);
  LoadSymbol(handle, "spd_fd", &api.fd, nullptr);
  LoadSymbol(handle, "free_spd_voices", &api.free_voices, nullptr);
  LoadSymbol(handle, "free_spd_modules", &api.free_modules, nullptr);

  if (!missing.empty()) {
    result.error = "libspeechd is missing required symbols: " + missing;
    dlclose(handle);
    return result;
  }

  // The handle is intentionally never closed: libspeechd may own a callback
  // thread for as long as any connection is open.
  result.ok = true;
  return result;
}

}  // namespace

static_assert(sizeof(sigset_t) <= 128, "sigset_t storage too small");

ScopedSigpipeGuard::ScopedSigpipeGuard() {
  sigset_t pending;
  sigemptyset(&pending);
  if (sigpending(&pending) == 0) {
    was_pending_ = sigismember(&pending, SIGPIPE) == 1;
  }
  sigset_t block;
  sigemptyset(&block);
  sigaddset(&block, SIGPIPE);
  sigset_t old_mask;
  if (pthread_sigmask(SIG_BLOCK, &block, &old_mask) == 0) {
    memcpy(old_mask_, &old_mask, sizeof(old_mask));
    restore_mask_ = true;
  }
}

ScopedSigpipeGuard::~ScopedSigpipeGuard() {
  if (!restore_mask_)
    return;
  sigset_t old_mask;
  memcpy(&old_mask, old_mask_, sizeof(old_mask));
  if (!was_pending_ && !sigismember(&old_mask, SIGPIPE)) {
    // Consume a SIGPIPE raised while blocked, so it isn't delivered when the
    // mask is restored.
    sigset_t pending;
    sigemptyset(&pending);
    if (sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE) == 1) {
      sigset_t pipe_set;
      sigemptyset(&pipe_set);
      sigaddset(&pipe_set, SIGPIPE);
      const struct timespec zero = {0, 0};
      while (sigtimedwait(&pipe_set, nullptr, &zero) == -1 && errno == EINTR) {
      }
    }
  }
  pthread_sigmask(SIG_SETMASK, &old_mask, nullptr);
}

const SpeechdApi* LoadSpeechdApi(std::string* error) {
  static std::once_flag once;
  static LoadResult* result = nullptr;
  std::call_once(once, [] { result = new LoadResult(Load()); });
  if (!result->ok) {
    if (error != nullptr)
      *error = result->error;
    return nullptr;
  }
  return &result->api;
}

void FreeVoiceList(const SpeechdApi* api, SpdVoice** voices) {
  if (voices == nullptr)
    return;
  if (api != nullptr && api->free_voices != nullptr) {
    api->free_voices(voices);
    return;
  }
  // Fallback matching libspeechd's own implementation (malloc'd strings).
  for (SpdVoice** v = voices; *v != nullptr; ++v) {
    free((*v)->name);
    free((*v)->language);
    free((*v)->variant);
    free(*v);
  }
  free(voices);
}

void FreeStringList(const SpeechdApi* api, char** list) {
  if (list == nullptr)
    return;
  if (api != nullptr && api->free_modules != nullptr) {
    api->free_modules(list);
    return;
  }
  for (char** s = list; *s != nullptr; ++s)
    free(*s);
  free(list);
}

}  // namespace flutter_tts

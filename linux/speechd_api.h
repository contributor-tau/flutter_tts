// Runtime-loaded bindings for the Speech Dispatcher client library
// (libspeechd).
//
// The plugin never links against libspeechd. Instead it is opened with
// dlopen() the first time text-to-speech is used, so that:
//   * applications still start on systems (or Flatpak/Snap sandboxes) where
//     libspeechd is not installed, and simply report TTS as unavailable, and
//   * building the plugin does not require the libspeechd development
//     headers.
//
// Only the small, long-stable part of the libspeechd ABI that the plugin
// needs is declared here. These declarations mirror
// <speech-dispatcher/libspeechd.h> and <speech-dispatcher/speechd_types.h>
// and have been unchanged since Speech Dispatcher 0.8 (libspeechd.so.2).

#ifndef FLUTTER_TTS_SPEECHD_API_H_
#define FLUTTER_TTS_SPEECHD_API_H_

#include <cstddef>
#include <string>

namespace flutter_tts {

// --- Mirrors of libspeechd types -------------------------------------------

enum SpdConnectionMode { kSpdModeSingle = 0, kSpdModeThreaded = 1 };

enum SpdPriority {
  kSpdImportant = 1,
  kSpdMessage = 2,
  kSpdText = 3,
  kSpdNotification = 4,
  kSpdProgress = 5,
};

// SPDNotification: bit flags passed to spd_set_notification_on().
enum SpdNotification {
  kSpdBegin = 1,
  kSpdEnd = 2,
  kSpdIndexMarks = 4,
  kSpdCancel = 8,
  kSpdPause = 16,
  kSpdResume = 32,
};

// SPDNotificationType: the event type passed to callbacks.
enum SpdNotificationType {
  kSpdEventBegin = 0,
  kSpdEventEnd = 1,
  kSpdEventIndexMark = 2,
  kSpdEventCancel = 3,
  kSpdEventPause = 4,
  kSpdEventResume = 5,
};

enum SpdVoiceType { kSpdMale1 = 1 };

struct SpdVoice {
  char* name;
  char* language;
  char* variant;
};

using SpdCallback = void (*)(size_t msg_id, size_t client_id, int state);
using SpdCallbackIM = void (*)(size_t msg_id,
                               size_t client_id,
                               int state,
                               char* index_mark);

// Only the public head of SPDConnection. The callbacks are the first members
// of the struct and are documented as public; the rest is private to
// libspeechd and never touched here.
struct SpdConnection {
  SpdCallback callback_begin;
  SpdCallback callback_end;
  SpdCallback callback_cancel;
  SpdCallback callback_pause;
  SpdCallback callback_resume;
  SpdCallbackIM callback_im;
};

// --- Function table ---------------------------------------------------------

struct SpeechdApi {
  // Required.
  SpdConnection* (*open2)(const char* client_name,
                          const char* connection_name,
                          const char* user_name,
                          int mode,
                          const void* address,
                          int autospawn,
                          char** error_result);
  void (*close)(SpdConnection*);
  int (*say)(SpdConnection*, int priority, const char* text);
  int (*cancel)(SpdConnection*);
  int (*pause)(SpdConnection*);
  int (*resume)(SpdConnection*);
  int (*set_notification_on)(SpdConnection*, int notification);
  int (*set_voice_rate)(SpdConnection*, int rate);
  int (*set_voice_pitch)(SpdConnection*, int pitch);
  int (*set_volume)(SpdConnection*, int volume);
  int (*set_language)(SpdConnection*, const char* language);
  int (*set_synthesis_voice)(SpdConnection*, const char* voice_name);
  int (*set_voice_type)(SpdConnection*, int type);
  SpdVoice** (*list_synthesis_voices)(SpdConnection*);
  char** (*list_modules)(SpdConnection*);
  int (*set_output_module)(SpdConnection*, const char* module);

  // Optional: may be missing from very old libspeechd builds. Callers must
  // check for nullptr.
  char* (*get_language)(SpdConnection*);
  char* (*get_output_module)(SpdConnection*);
  int (*fd)(SpdConnection*);
  void (*free_voices)(SpdVoice**);
  void (*free_modules)(char**);
};

// Loads libspeechd on first call and returns its function table, or nullptr
// if the library (or a required symbol) is not available. In that case
// |error| (if non-null) receives a human-readable reason. The result is
// cached; this function is thread-safe.
const SpeechdApi* LoadSpeechdApi(std::string* error);

// libspeechd writes to its socket without MSG_NOSIGNAL, so a call made after
// speech-dispatcher exited (or was restarted) raises SIGPIPE, which would
// kill the whole application. While an instance of this class is alive,
// SIGPIPE is blocked on the current thread; a SIGPIPE raised in that time is
// consumed on destruction. The process-wide signal disposition is left
// untouched.
class ScopedSigpipeGuard {
 public:
  ScopedSigpipeGuard();
  ~ScopedSigpipeGuard();
  ScopedSigpipeGuard(const ScopedSigpipeGuard&) = delete;
  ScopedSigpipeGuard& operator=(const ScopedSigpipeGuard&) = delete;

 private:
  bool was_pending_ = false;
  bool restore_mask_ = false;
  unsigned char old_mask_[128];  // Storage for a sigset_t.
};

// Calls a libspeechd function with SIGPIPE guarded (see above).
template <typename Fn, typename... Args>
auto SpdCall(Fn fn, Args... args) -> decltype(fn(args...)) {
  ScopedSigpipeGuard guard;
  return fn(args...);
}

// Frees a voice list returned by list_synthesis_voices, using the library's
// own free function when available.
void FreeVoiceList(const SpeechdApi* api, SpdVoice** voices);

// Frees a string list returned by list_modules.
void FreeStringList(const SpeechdApi* api, char** list);

}  // namespace flutter_tts

#endif  // FLUTTER_TTS_SPEECHD_API_H_

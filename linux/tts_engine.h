// Speech Dispatcher based text-to-speech engine for the Linux plugin.
//
// This class has no Flutter dependencies so that it can be tested directly
// against a running speech-dispatcher. All methods must be called on the
// thread running the default GLib main context (the GTK main thread in a
// Flutter app), and listener callbacks are delivered on that thread too.
//
// Behaviour follows the Android implementation of flutter_tts:
//   * Text is spoken sentence by sentence (see SplitIntoChunks()).
//   * pause() stops speaking and remembers the current sentence; calling
//     speak() again with the same text resumes from that sentence, while a
//     different text starts over.
//   * In flush mode (the default) a new speak() interrupts the current one;
//     in add mode it is queued.
//   * With awaitSpeakCompletion, the speak() result is delivered when the
//     utterance completes (1) or is paused/stopped/cancelled (0).
//
// Speech Dispatcher's own PAUSE/RESUME commands are deliberately not used:
// they leave the client paused when nothing is speaking and a CANCEL does not
// discard a paused message, both of which made stop/pause unreliable.

#ifndef FLUTTER_TTS_TTS_ENGINE_H_
#define FLUTTER_TTS_TTS_ENGINE_H_

#include <glib.h>

#include <cstddef>
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "speechd_api.h"
#include "tts_util.h"

namespace flutter_tts {

class TtsEngine {
 public:
  class Listener {
   public:
    virtual ~Listener() = default;
    virtual void OnSpeakStart() = 0;
    virtual void OnSpeakComplete() = 0;
    virtual void OnSpeakCancel() = 0;
    virtual void OnSpeakPause() = 0;
    virtual void OnSpeakContinue() = 0;
    // |start|/|end| are UTF-16 offsets into |text| of the sentence being
    // spoken, and |word| is that sentence.
    virtual void OnSpeakProgress(const std::string& text,
                                 size_t start,
                                 size_t end,
                                 const std::string& word) = 0;
    virtual void OnSpeakError(const std::string& message) = 0;
  };

  struct Voice {
    std::string name;
    std::string locale;    // Normalised BCP-47, e.g. "en-US".
    std::string language;  // As reported by Speech Dispatcher.
    std::string variant;   // As reported by Speech Dispatcher ("none" if n/a).
  };

  // Called exactly once with the result of a speak() call (1 or 0).
  using SpeakCallback = std::function<void(int)>;

  enum QueueMode { kQueueFlush = 0, kQueueAdd = 1 };

  explicit TtsEngine(Listener* listener);
  ~TtsEngine();

  TtsEngine(const TtsEngine&) = delete;
  TtsEngine& operator=(const TtsEngine&) = delete;

  void Speak(const std::string& text, SpeakCallback on_result);
  void Pause();
  void Stop();

  void SetAwaitSpeakCompletion(bool await) { await_speak_completion_ = await; }
  void SetQueueMode(int mode);

  bool SetSpeechRate(double rate);
  bool SetVolume(double volume);
  bool SetPitch(double pitch);

  // Returns the voices of the current output module. espeak-ng style
  // "<voice>+<variant>" combinations are left out when "<voice>" itself is
  // listed, to keep the list usable (espeak-ng alone reports >10,000 such
  // combinations); they can still be selected by name with SetVoice().
  std::vector<Voice> GetVoices();
  std::vector<std::string> GetLanguages();
  bool IsLanguageAvailable(const std::string& language);
  bool SetLanguage(const std::string& language);
  // |locale| may be empty to match by name only.
  bool SetVoice(const std::string& name, const std::string& locale);
  bool ClearVoice();

  // Speech Dispatcher output modules, exposed through the engine API.
  std::vector<std::string> GetEngines();
  std::string GetDefaultEngine();
  bool SetEngine(const std::string& engine);

  // Whether a connection to speech-dispatcher could be made. If not, |error|
  // receives the reason.
  bool EnsureConnected(std::string* error = nullptr);

  // Dispatches a Speech Dispatcher notification. Public for the static
  // callback trampoline only.
  void HandleEvent(size_t msg_id, int type);

 private:
  enum class State { kIdle, kSpeaking, kPaused };

  struct Utterance {
    std::string text;
    std::vector<TextChunk> chunks;
    size_t chunk = 0;      // Index of the chunk being (or to be) spoken.
    bool started = false;  // Whether OnSpeakStart was sent.
    SpeakCallback on_result;
  };

  bool Connect(std::string* error);
  void Disconnect();
  // Whether the daemon closed the connection. libspeechd doesn't notice this
  // itself: its reader thread spins on EOF and calls waiting for a reply
  // block forever, so this must be checked before every call.
  bool ConnectionDead() const;
  // Drops a dead connection; an utterance being spoken fails with an error.
  void HandleConnectionLost();
  static gboolean OnConnectionHangup(gint fd,
                                     GIOCondition condition,
                                     gpointer user_data);
  void ApplySettings();
  bool LoadVoices();

  void StartNextIfIdle();
  void SubmitCurrentChunk();
  // Interrupts the current message without generating events for it.
  void CancelCurrentMessage();
  // Resolves pending results with 0 and empties the queue.
  void ClearQueue();
  void ResolveFront(int result);
  void ReportError(const std::string& message);

  Listener* listener_;
  const SpeechdApi* api_ = nullptr;
  SpdConnection* connection_ = nullptr;
  int connection_fd_ = -1;
  guint hangup_watch_ = 0;
  std::string last_error_;
  gint64 last_connect_attempt_us_ = 0;

  State state_ = State::kIdle;
  size_t current_msg_ = 0;
  bool resuming_ = false;
  std::deque<Utterance> queue_;

  bool await_speak_completion_ = false;
  QueueMode queue_mode_ = kQueueFlush;

  // Settings, re-applied after a reconnect.
  bool has_rate_ = false, has_volume_ = false, has_pitch_ = false;
  int rate_ = 0, volume_ = 0, pitch_ = 0;
  std::string language_;
  std::string voice_name_;
  std::string module_;

  // Values reported by speech-dispatcher on first connect.
  std::string default_language_;
  std::string default_module_;
  bool have_defaults_ = false;

  bool voices_loaded_ = false;
  std::vector<Voice> voices_;
};

}  // namespace flutter_tts

#endif  // FLUTTER_TTS_TTS_ENGINE_H_

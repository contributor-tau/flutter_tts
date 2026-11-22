#include "tts_engine.h"

#include <glib-unix.h>
#include <glib.h>
#include <poll.h>

#include <algorithm>
#include <cstdlib>
#include <set>
#include <unordered_set>
#include <utility>

namespace flutter_tts {

namespace {

// Minimum time between two connection attempts after a failure, so that a
// missing speech-dispatcher doesn't cost a connection attempt on every call.
constexpr gint64 kReconnectIntervalUs = 1000 * 1000;

// Live engines. Only accessed on the main thread.
std::vector<TtsEngine*>& Engines() {
  static std::vector<TtsEngine*>* engines = new std::vector<TtsEngine*>();
  return *engines;
}

struct PendingEvent {
  size_t msg_id;
  int type;
};

gboolean DispatchEvent(gpointer data) {
  const PendingEvent* event = static_cast<PendingEvent*>(data);
  // Message ids are unique per speech-dispatcher instance, so only the engine
  // that owns the message reacts to it. Copy the list: handlers may create or
  // destroy engines.
  std::vector<TtsEngine*> engines = Engines();
  for (TtsEngine* engine : engines) {
    if (std::find(Engines().begin(), Engines().end(), engine) !=
        Engines().end()) {
      engine->HandleEvent(event->msg_id, event->type);
    }
  }
  return G_SOURCE_REMOVE;
}

// Called on libspeechd's callback thread. Hands the event to the main loop
// via an idle source (never run inline, even if the main context happens to
// be free) so that all engine state is only touched on the main thread.
void OnSpeechdEvent(size_t msg_id, size_t /*client_id*/, int type) {
  PendingEvent* event = g_new(PendingEvent, 1);
  event->msg_id = msg_id;
  event->type = type;
  g_idle_add_full(G_PRIORITY_DEFAULT, DispatchEvent, event, g_free);
}

bool InFlatpak() {
  return g_file_test("/.flatpak-info", G_FILE_TEST_EXISTS);
}

bool InSnap() {
  return g_getenv("SNAP") != nullptr && g_getenv("SNAP_NAME") != nullptr;
}

// Extra advice appended to connection errors inside sandboxes.
std::string LibraryHint() {
  if (InFlatpak()) {
    return " (Flatpak: libspeechd is not part of the runtime; bundle it as a "
           "module in the app manifest, see the flutter_tts README)";
  }
  if (InSnap()) {
    return " (Snap: stage libspeechd2 in the snap, see the flutter_tts "
           "README)";
  }
  return " (install the speech-dispatcher package)";
}

std::string ConnectionHint() {
  if (InFlatpak()) {
    return " (Flatpak: the app needs the "
           "--filesystem=xdg-run/speech-dispatcher:ro permission and "
           "speech-dispatcher must be running on the host; see the flutter_tts "
           "README)";
  }
  if (InSnap()) {
    return " (Snap: snaps cannot reach the host's speech-dispatcher; bundle "
           "it in the snap, see the flutter_tts README)";
  }
  return "";
}

std::string TakeString(char* s) {
  if (s == nullptr)
    return std::string();
  std::string result(s);
  free(s);
  return result;
}

}  // namespace

TtsEngine::TtsEngine(Listener* listener) : listener_(listener) {
  Engines().push_back(this);
}

TtsEngine::~TtsEngine() {
  auto& engines = Engines();
  engines.erase(std::remove(engines.begin(), engines.end(), this),
                engines.end());
  // Pending results are dropped: the listener (and Flutter engine) is going
  // away too.
  queue_.clear();
  if (connection_ != nullptr && state_ == State::kSpeaking &&
      !ConnectionDead()) {
    SpdCall(api_->cancel, connection_);
  }
  Disconnect();
}

// --- Connection -------------------------------------------------------------

bool TtsEngine::EnsureConnected(std::string* error) {
  if (connection_ != nullptr && ConnectionDead())
    HandleConnectionLost();
  if (connection_ != nullptr)
    return true;
  const gint64 now = g_get_monotonic_time();
  if (last_connect_attempt_us_ != 0 &&
      now - last_connect_attempt_us_ < kReconnectIntervalUs) {
    if (error != nullptr)
      *error = last_error_;
    return false;
  }
  last_connect_attempt_us_ = now;
  std::string reason;
  if (Connect(&reason)) {
    last_connect_attempt_us_ = 0;
    last_error_.clear();
    return true;
  }
  if (reason != last_error_)
    g_warning("flutter_tts: %s", reason.c_str());
  last_error_ = reason;
  if (error != nullptr)
    *error = reason;
  return false;
}

bool TtsEngine::Connect(std::string* error) {
  std::string load_error;
  api_ = LoadSpeechdApi(&load_error);
  if (api_ == nullptr) {
    *error = "Text-to-speech is unavailable: " + load_error + LibraryHint();
    return false;
  }

  const char* prgname = g_get_prgname();
  const std::string client_name =
      (prgname != nullptr && *prgname != '\0') ? prgname : "flutter_tts";
  char* open_error = nullptr;
  // A null address makes libspeechd honour SPEECHD_ADDRESS and otherwise use
  // $XDG_RUNTIME_DIR/speech-dispatcher/speechd.sock. Autospawn lets it start
  // the daemon when it isn't socket-activated (it fails harmlessly inside
  // sandboxes that don't ship the daemon).
  connection_ =
      SpdCall(api_->open2, client_name.c_str(), "flutter_tts", nullptr,
              kSpdModeThreaded, nullptr, /*autospawn=*/1, &open_error);
  if (connection_ == nullptr) {
    std::string detail = TakeString(open_error);
    *error = "Could not connect to speech-dispatcher" +
             (detail.empty() ? std::string() : ": " + detail) +
             ConnectionHint();
    return false;
  }
  free(open_error);

  connection_->callback_begin = OnSpeechdEvent;
  connection_->callback_end = OnSpeechdEvent;
  connection_->callback_cancel = OnSpeechdEvent;
  SpdCall(api_->set_notification_on, connection_, kSpdBegin);
  SpdCall(api_->set_notification_on, connection_, kSpdEnd);
  SpdCall(api_->set_notification_on, connection_, kSpdCancel);

  connection_fd_ = api_->fd != nullptr ? api_->fd(connection_) : -1;
  if (connection_fd_ >= 0) {
    hangup_watch_ = g_unix_fd_add(
        connection_fd_, static_cast<GIOCondition>(G_IO_HUP | G_IO_ERR),
        &TtsEngine::OnConnectionHangup, this);
  }

  if (!have_defaults_) {
    if (api_->get_language != nullptr) {
      default_language_ = TakeString(SpdCall(api_->get_language, connection_));
    }
    if (api_->get_output_module != nullptr) {
      default_module_ =
          TakeString(SpdCall(api_->get_output_module, connection_));
    }
    have_defaults_ = true;
  }
  ApplySettings();
  return true;
}

void TtsEngine::Disconnect() {
  if (connection_ == nullptr)
    return;
  if (hangup_watch_ != 0) {
    g_source_remove(hangup_watch_);
    hangup_watch_ = 0;
  }
  // spd_close() stops libspeechd's callback thread; events it already queued
  // on the main loop are ignored because current_msg_ no longer matches.
  SpdCall(api_->close, connection_);
  connection_ = nullptr;
  connection_fd_ = -1;
  current_msg_ = 0;
}

bool TtsEngine::ConnectionDead() const {
  if (connection_ == nullptr || connection_fd_ < 0)
    return false;
  struct pollfd pfd = {};
  pfd.fd = connection_fd_;
  pfd.events = POLLRDHUP;
  if (poll(&pfd, 1, 0) <= 0)
    return false;
  return (pfd.revents & (POLLRDHUP | POLLHUP | POLLERR | POLLNVAL)) != 0;
}

void TtsEngine::HandleConnectionLost() {
  if (connection_ == nullptr)
    return;
  g_warning("flutter_tts: lost connection to speech-dispatcher");
  Disconnect();
  // Allow an immediate reconnect, e.g. when the daemon was restarted.
  last_connect_attempt_us_ = 0;
  // Settings are re-applied on reconnect; the voice list may have changed.
  voices_loaded_ = false;
  voices_.clear();
  if (state_ == State::kSpeaking) {
    state_ = State::kIdle;
    resuming_ = false;
    ClearQueue();
    ReportError("Lost connection to speech-dispatcher");
  }
  // A paused utterance stays paused and resumes on a new connection.
}

gboolean TtsEngine::OnConnectionHangup(gint /*fd*/,
                                       GIOCondition /*condition*/,
                                       gpointer user_data) {
  TtsEngine* self = static_cast<TtsEngine*>(user_data);
  // Returning G_SOURCE_REMOVE destroys the source; don't remove it twice.
  self->hangup_watch_ = 0;
  self->HandleConnectionLost();
  return G_SOURCE_REMOVE;
}

void TtsEngine::ApplySettings() {
  if (!module_.empty())
    SpdCall(api_->set_output_module, connection_, module_.c_str());
  if (has_rate_)
    SpdCall(api_->set_voice_rate, connection_, rate_);
  if (has_volume_)
    SpdCall(api_->set_volume, connection_, volume_);
  if (has_pitch_)
    SpdCall(api_->set_voice_pitch, connection_, pitch_);
  if (!language_.empty())
    SpdCall(api_->set_language, connection_, language_.c_str());
  if (!voice_name_.empty()) {
    SpdCall(api_->set_synthesis_voice, connection_, voice_name_.c_str());
  }
}

// --- Speaking ---------------------------------------------------------------

void TtsEngine::Speak(const std::string& text, SpeakCallback on_result) {
  std::string error;
  if (!EnsureConnected(&error)) {
    ReportError(error);
    if (on_result)
      on_result(0);
    return;
  }

  if (state_ == State::kPaused) {
    if (!queue_.empty() && queue_.front().text == text) {
      // Resume the paused utterance at the interrupted sentence.
      Utterance& u = queue_.front();
      if (await_speak_completion_) {
        u.on_result = std::move(on_result);
      } else if (on_result) {
        on_result(1);
      }
      state_ = State::kIdle;
      resuming_ = true;
      SubmitCurrentChunk();
      return;
    }
    // A different text replaces the paused utterance.
    ResolveFront(0);
    queue_.pop_front();
    state_ = State::kIdle;
    resuming_ = false;
  }

  if (queue_mode_ == kQueueFlush) {
    // Like Android: while an awaited utterance is in progress, further
    // speak() calls are rejected rather than interrupting it.
    if (await_speak_completion_ && !queue_.empty() &&
        queue_.front().on_result) {
      if (on_result)
        on_result(0);
      return;
    }
    if (state_ == State::kSpeaking || !queue_.empty()) {
      CancelCurrentMessage();
      ClearQueue();
      state_ = State::kIdle;
      resuming_ = false;
      listener_->OnSpeakCancel();
    }
  }

  Utterance u;
  u.text = text;
  u.chunks = SplitIntoChunks(text);
  if (u.chunks.empty()) {
    // Nothing to say.
    if (on_result)
      on_result(1);
    return;
  }
  if (await_speak_completion_) {
    u.on_result = std::move(on_result);
  } else if (on_result) {
    on_result(1);
  }
  queue_.push_back(std::move(u));
  StartNextIfIdle();
}

void TtsEngine::Pause() {
  if (state_ != State::kSpeaking)
    return;
  CancelCurrentMessage();
  state_ = State::kPaused;
  resuming_ = false;
  ResolveFront(0);
  listener_->OnSpeakPause();
}

void TtsEngine::Stop() {
  const bool was_speaking = state_ == State::kSpeaking;
  CancelCurrentMessage();
  ClearQueue();
  state_ = State::kIdle;
  resuming_ = false;
  if (was_speaking)
    listener_->OnSpeakCancel();
}

void TtsEngine::SetQueueMode(int mode) {
  queue_mode_ = mode == kQueueAdd ? kQueueAdd : kQueueFlush;
}

void TtsEngine::StartNextIfIdle() {
  if (state_ != State::kIdle || queue_.empty())
    return;
  SubmitCurrentChunk();
}

void TtsEngine::SubmitCurrentChunk() {
  // The previous message has finished (or none was active); a dead
  // connection found here must not fail the utterance we are about to send.
  current_msg_ = 0;
  state_ = State::kIdle;

  std::string error;
  bool connected = EnsureConnected(&error);
  int msg_id = -1;
  if (connected && !queue_.empty()) {
    const Utterance& u = queue_.front();
    const TextChunk& chunk = u.chunks[u.chunk];
    const std::string piece =
        u.text.substr(chunk.start, chunk.end - chunk.start);
    msg_id = SpdCall(api_->say, connection_, kSpdText, piece.c_str());
    if (msg_id < 0) {
      // The daemon may have exited or been restarted: reconnect once.
      Disconnect();
      last_connect_attempt_us_ = 0;
      connected = EnsureConnected(&error);
      if (connected) {
        msg_id = SpdCall(api_->say, connection_, kSpdText, piece.c_str());
      }
    }
  }
  if (queue_.empty())
    return;
  if (msg_id < 0) {
    ReportError(!error.empty() ? error
                               : "speech-dispatcher rejected the message");
    ClearQueue();
    resuming_ = false;
    return;
  }
  current_msg_ = static_cast<size_t>(msg_id);
  state_ = State::kSpeaking;
}

void TtsEngine::CancelCurrentMessage() {
  if (state_ == State::kSpeaking && connection_ != nullptr &&
      !ConnectionDead()) {
    SpdCall(api_->cancel, connection_);
  }
  // Events still in flight for the cancelled message are ignored.
  current_msg_ = 0;
}

void TtsEngine::ResolveFront(int result) {
  if (queue_.empty() || !queue_.front().on_result)
    return;
  SpeakCallback callback = std::move(queue_.front().on_result);
  queue_.front().on_result = nullptr;
  callback(result);
}

void TtsEngine::ClearQueue() {
  std::deque<Utterance> queue;
  queue.swap(queue_);
  for (Utterance& u : queue) {
    if (u.on_result)
      u.on_result(0);
  }
}

void TtsEngine::ReportError(const std::string& message) {
  listener_->OnSpeakError(message);
}

void TtsEngine::HandleEvent(size_t msg_id, int type) {
  if (msg_id == 0 || msg_id != current_msg_ || state_ != State::kSpeaking ||
      queue_.empty()) {
    return;
  }
  Utterance& u = queue_.front();
  switch (type) {
    case kSpdEventBegin: {
      // Copy what the progress event needs first: listeners may call back
      // into the engine and modify the queue.
      const TextChunk chunk = u.chunks[u.chunk];
      const std::string text = u.text;
      const bool was_resuming = resuming_;
      const bool first = !u.started;
      resuming_ = false;
      u.started = true;
      if (was_resuming) {
        listener_->OnSpeakContinue();
      } else if (first) {
        listener_->OnSpeakStart();
      }
      if (current_msg_ != msg_id)
        break;  // Stopped from the listener.
      listener_->OnSpeakProgress(
          text, Utf16Length(text, 0, chunk.start),
          Utf16Length(text, 0, chunk.end),
          text.substr(chunk.start, chunk.end - chunk.start));
      break;
    }
    case kSpdEventEnd: {
      current_msg_ = 0;
      if (u.chunk + 1 < u.chunks.size()) {
        ++u.chunk;
        SubmitCurrentChunk();
        break;
      }
      state_ = State::kIdle;
      // Take the utterance off the queue before calling out, so that
      // listeners may call speak() again.
      Utterance done = std::move(u);
      queue_.pop_front();
      listener_->OnSpeakComplete();
      if (done.on_result)
        done.on_result(1);
      StartNextIfIdle();
      break;
    }
    case kSpdEventCancel: {
      // Cancelled by someone else, e.g. a higher-priority message or another
      // client's text (Speech Dispatcher lets new text interrupt old text).
      current_msg_ = 0;
      state_ = State::kIdle;
      resuming_ = false;
      ClearQueue();
      listener_->OnSpeakCancel();
      break;
    }
    default:
      break;
  }
}

// --- Settings ---------------------------------------------------------------

bool TtsEngine::SetSpeechRate(double rate) {
  rate_ = RateToSpeechd(rate);
  has_rate_ = true;
  return EnsureConnected() &&
         SpdCall(api_->set_voice_rate, connection_, rate_) == 0;
}

bool TtsEngine::SetVolume(double volume) {
  volume_ = VolumeToSpeechd(volume);
  has_volume_ = true;
  return EnsureConnected() &&
         SpdCall(api_->set_volume, connection_, volume_) == 0;
}

bool TtsEngine::SetPitch(double pitch) {
  pitch_ = PitchToSpeechd(pitch);
  has_pitch_ = true;
  return EnsureConnected() &&
         SpdCall(api_->set_voice_pitch, connection_, pitch_) == 0;
}

// --- Voices and languages ---------------------------------------------------

bool TtsEngine::LoadVoices() {
  // Connect first even when the voices are cached: callers go on to use the
  // connection.
  if (!EnsureConnected())
    return false;
  if (voices_loaded_)
    return true;
  SpdVoice** list = SpdCall(api_->list_synthesis_voices, connection_);
  if (list == nullptr)
    return false;
  voices_.clear();
  for (SpdVoice** v = list; *v != nullptr; ++v) {
    if ((*v)->name == nullptr)
      continue;
    Voice voice;
    voice.name = (*v)->name;
    voice.language = (*v)->language != nullptr ? (*v)->language : "";
    voice.locale = NormalizeLocale(voice.language);
    voice.variant = (*v)->variant != nullptr ? (*v)->variant : "none";
    voices_.push_back(std::move(voice));
  }
  FreeVoiceList(api_, list);
  voices_loaded_ = true;
  return true;
}

std::vector<TtsEngine::Voice> TtsEngine::GetVoices() {
  std::vector<Voice> result;
  if (!LoadVoices())
    return result;
  std::unordered_set<std::string> names;
  for (const Voice& v : voices_)
    names.insert(v.name);
  for (const Voice& v : voices_) {
    const size_t plus = v.name.find('+');
    if (plus != std::string::npos && plus > 0 &&
        names.count(v.name.substr(0, plus)) > 0) {
      continue;
    }
    result.push_back(v);
  }
  return result;
}

std::vector<std::string> TtsEngine::GetLanguages() {
  std::set<std::string> languages;
  if (LoadVoices()) {
    for (const Voice& v : voices_) {
      if (!v.locale.empty())
        languages.insert(v.locale);
    }
  }
  return std::vector<std::string>(languages.begin(), languages.end());
}

bool TtsEngine::IsLanguageAvailable(const std::string& language) {
  if (!LoadVoices())
    return false;
  for (const Voice& v : voices_) {
    if (LanguageMatchScore(language, v.locale) > 0)
      return true;
  }
  return false;
}

bool TtsEngine::SetLanguage(const std::string& language) {
  if (!LoadVoices())
    return false;
  const Voice* best = nullptr;
  int best_score = 0;
  for (const Voice& v : voices_) {
    const int score = LanguageMatchScore(language, v.locale);
    if (score > best_score) {
      best = &v;
      best_score = score;
      if (score == 2)
        break;
    }
  }
  if (best == nullptr)
    return false;
  // An explicitly chosen voice would take precedence over the language, so
  // go back to the default voice for the new language.
  voice_name_.clear();
  SpdCall(api_->set_voice_type, connection_, kSpdMale1);
  language_ = best->language;
  return SpdCall(api_->set_language, connection_, language_.c_str()) == 0;
}

bool TtsEngine::SetVoice(const std::string& name, const std::string& locale) {
  if (!LoadVoices())
    return false;
  const Voice* match = nullptr;
  for (const Voice& v : voices_) {
    if (v.name != name)
      continue;
    if (!locale.empty() && LanguageMatchScore(locale, v.locale) == 0)
      continue;
    match = &v;
    break;
  }
  if (match == nullptr)
    return false;
  language_ = match->language;
  voice_name_ = match->name;
  if (!language_.empty())
    SpdCall(api_->set_language, connection_, language_.c_str());
  return SpdCall(api_->set_synthesis_voice, connection_, voice_name_.c_str()) ==
         0;
}

bool TtsEngine::ClearVoice() {
  voice_name_.clear();
  language_.clear();
  if (!EnsureConnected())
    return false;
  SpdCall(api_->set_voice_type, connection_, kSpdMale1);
  if (!default_language_.empty()) {
    SpdCall(api_->set_language, connection_, default_language_.c_str());
  }
  return true;
}

// --- Output modules ---------------------------------------------------------

std::vector<std::string> TtsEngine::GetEngines() {
  std::vector<std::string> result;
  if (!EnsureConnected())
    return result;
  char** modules = SpdCall(api_->list_modules, connection_);
  if (modules == nullptr)
    return result;
  for (char** m = modules; *m != nullptr; ++m)
    result.emplace_back(*m);
  FreeStringList(api_, modules);
  return result;
}

std::string TtsEngine::GetDefaultEngine() {
  if (!EnsureConnected())
    return std::string();
  if (!default_module_.empty())
    return default_module_;
  std::vector<std::string> engines = GetEngines();
  return engines.empty() ? std::string() : engines.front();
}

bool TtsEngine::SetEngine(const std::string& engine) {
  const std::vector<std::string> engines = GetEngines();
  if (std::find(engines.begin(), engines.end(), engine) == engines.end()) {
    return false;
  }
  if (SpdCall(api_->set_output_module, connection_, engine.c_str()) != 0)
    return false;
  module_ = engine;
  // Voices differ between modules.
  voice_name_.clear();
  voices_loaded_ = false;
  voices_.clear();
  return true;
}

}  // namespace flutter_tts

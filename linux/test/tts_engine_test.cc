// Integration tests for TtsEngine. These talk to a real speech-dispatcher
// (with any output module and audio sink, e.g. a PulseAudio null sink) and
// are skipped when none is reachable.

#include <glib.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "tts_engine.h"

namespace flutter_tts {
namespace {

class RecordingListener : public TtsEngine::Listener {
 public:
  void OnSpeakStart() override { events.push_back("start"); }
  void OnSpeakComplete() override { events.push_back("complete"); }
  void OnSpeakCancel() override { events.push_back("cancel"); }
  void OnSpeakPause() override { events.push_back("pause"); }
  void OnSpeakContinue() override { events.push_back("continue"); }
  void OnSpeakProgress(const std::string& text,
                       size_t start,
                       size_t end,
                       const std::string& word) override {
    events.push_back("progress:" + word);
    progress_ranges.emplace_back(start, end);
  }
  void OnSpeakError(const std::string& message) override {
    events.push_back("error");
    errors.push_back(message);
  }

  size_t Count(const std::string& event) const {
    return std::count(events.begin(), events.end(), event);
  }

  std::vector<std::string> events;
  std::vector<std::pair<size_t, size_t>> progress_ranges;
  std::vector<std::string> errors;
};

// Iterates the default main context until |done| returns true or the
// timeout elapses. Returns whether |done| became true.
bool RunUntil(const std::function<bool()>& done, int timeout_ms = 15000) {
  const gint64 deadline = g_get_monotonic_time() + timeout_ms * 1000LL;
  while (!done()) {
    if (g_get_monotonic_time() > deadline)
      return false;
    g_main_context_iteration(nullptr, FALSE);
    g_usleep(2000);
  }
  return true;
}

void RunFor(int ms) {
  RunUntil([] { return false; }, ms);
}

constexpr char kShort[] = "Hello world.";
constexpr char kLong[] =
    "The first sentence is right here. The second sentence follows it. "
    "A third sentence comes next. And the fourth sentence ends it all.";

class TtsEngineTest : public ::testing::Test {
 protected:
  void SetUp() override {
    engine_ = std::make_unique<TtsEngine>(&listener_);
    std::string error;
    if (!engine_->EnsureConnected(&error)) {
      GTEST_SKIP() << "speech-dispatcher not reachable: " << error;
    }
  }

  void TearDown() override {
    if (engine_)
      engine_->Stop();
    engine_.reset();
    RunFor(100);  // Drain events for the destroyed engine.
  }

  RecordingListener listener_;
  std::unique_ptr<TtsEngine> engine_;
};

TEST_F(TtsEngineTest, SpeakCompletes) {
  engine_->SetAwaitSpeakCompletion(true);
  int result = -1;
  engine_->Speak(kShort, [&](int r) { result = r; });
  EXPECT_EQ(result, -1) << "result must wait for completion";
  ASSERT_TRUE(RunUntil([&] { return result != -1; }));
  EXPECT_EQ(result, 1);
  EXPECT_EQ(
      listener_.events,
      (std::vector<std::string>{"start", "progress:Hello world.", "complete"}));
}

TEST_F(TtsEngineTest, WithoutAwaitResultIsImmediate) {
  int result = -1;
  engine_->Speak(kShort, [&](int r) { result = r; });
  EXPECT_EQ(result, 1);
  ASSERT_TRUE(RunUntil([&] { return listener_.Count("complete") == 1; }));
}

TEST_F(TtsEngineTest, SpeaksSentenceBySentenceWithProgress) {
  engine_->SetAwaitSpeakCompletion(true);
  int result = -1;
  engine_->Speak(kLong, [&](int r) { result = r; });
  ASSERT_TRUE(RunUntil([&] { return result != -1; }, 30000));
  EXPECT_EQ(result, 1);
  EXPECT_EQ(listener_.Count("start"), 1u);
  EXPECT_EQ(listener_.Count("complete"), 1u);
  ASSERT_EQ(listener_.progress_ranges.size(), 4u);
  const std::string text = kLong;
  EXPECT_EQ(listener_.progress_ranges[0].first, 0u);
  EXPECT_EQ(text.substr(listener_.progress_ranges[1].first,
                        listener_.progress_ranges[1].second -
                            listener_.progress_ranges[1].first),
            "The second sentence follows it.");
  EXPECT_EQ(listener_.progress_ranges[3].second, text.size());
}

TEST_F(TtsEngineTest, StopCancelsAndDaemonIsNotLeftPaused) {
  engine_->SetAwaitSpeakCompletion(true);
  int result = -1;
  engine_->Speak(kLong, [&](int r) { result = r; });
  ASSERT_TRUE(RunUntil([&] { return listener_.Count("start") == 1; }));
  engine_->Stop();
  EXPECT_EQ(result, 0);
  EXPECT_EQ(listener_.Count("cancel"), 1u);
  RunFor(1500);
  EXPECT_EQ(listener_.Count("complete"), 0u);
  EXPECT_EQ(listener_.Count("cancel"), 1u) << "no duplicate cancel events";

  // Speaking again works right away.
  result = -1;
  engine_->Speak(kShort, [&](int r) { result = r; });
  ASSERT_TRUE(RunUntil([&] { return result != -1; }));
  EXPECT_EQ(result, 1);
}

TEST_F(TtsEngineTest, StopWhenIdleIsQuiet) {
  engine_->Stop();
  engine_->Pause();
  RunFor(300);
  EXPECT_TRUE(listener_.events.empty());
}

TEST_F(TtsEngineTest, PauseAndResumeSameText) {
  engine_->SetAwaitSpeakCompletion(true);
  int first = -1;
  engine_->Speak(kLong, [&](int r) { first = r; });
  // Pause during the second sentence.
  ASSERT_TRUE(
      RunUntil([&] { return listener_.progress_ranges.size() == 2; }, 20000));
  engine_->Pause();
  EXPECT_EQ(first, 0) << "pause resolves a pending await with 0";
  EXPECT_EQ(listener_.Count("pause"), 1u);
  RunFor(1500);
  EXPECT_EQ(listener_.Count("complete"), 0u);
  EXPECT_EQ(listener_.progress_ranges.size(), 2u)
      << "nothing spoken while paused";

  int second = -1;
  engine_->Speak(kLong, [&](int r) { second = r; });
  ASSERT_TRUE(RunUntil([&] { return second != -1; }, 30000));
  EXPECT_EQ(second, 1);
  EXPECT_EQ(listener_.Count("continue"), 1u);
  EXPECT_EQ(listener_.Count("start"), 1u);
  EXPECT_EQ(listener_.Count("complete"), 1u);
  // Resumed at the interrupted (second) sentence: 2 + 3 progress events.
  ASSERT_EQ(listener_.progress_ranges.size(), 5u);
  EXPECT_EQ(listener_.progress_ranges[2], listener_.progress_ranges[1]);
}

TEST_F(TtsEngineTest, SpeakDifferentTextWhilePausedStartsOver) {
  engine_->Speak(kLong, nullptr);
  ASSERT_TRUE(RunUntil([&] { return listener_.Count("start") == 1; }));
  engine_->Pause();
  engine_->SetAwaitSpeakCompletion(true);
  int result = -1;
  engine_->Speak(kShort, [&](int r) { result = r; });
  ASSERT_TRUE(RunUntil([&] { return result != -1; }));
  EXPECT_EQ(result, 1);
  EXPECT_EQ(listener_.Count("continue"), 0u);
  EXPECT_EQ(listener_.Count("start"), 2u);
  EXPECT_EQ(listener_.events.back(), "complete");
}

TEST_F(TtsEngineTest, FlushModeInterrupts) {
  engine_->Speak(kLong, nullptr);
  ASSERT_TRUE(RunUntil([&] { return listener_.Count("start") == 1; }));
  engine_->Speak(kShort, nullptr);
  ASSERT_TRUE(RunUntil([&] { return listener_.Count("complete") == 1; }));
  RunFor(500);
  EXPECT_EQ(listener_.Count("cancel"), 1u);
  EXPECT_EQ(listener_.Count("start"), 2u);
  EXPECT_EQ(listener_.Count("complete"), 1u);
}

TEST_F(TtsEngineTest, FlushModeRejectsWhileAwaiting) {
  engine_->SetAwaitSpeakCompletion(true);
  int first = -1, second = -1;
  engine_->Speak(kLong, [&](int r) { first = r; });
  engine_->Speak(kShort, [&](int r) { second = r; });
  EXPECT_EQ(second, 0);
  engine_->Stop();
  EXPECT_EQ(first, 0);
}

TEST_F(TtsEngineTest, AddModeQueues) {
  engine_->SetQueueMode(TtsEngine::kQueueAdd);
  engine_->SetAwaitSpeakCompletion(true);
  std::vector<int> order;
  engine_->Speak("First utterance here.", [&](int r) { order.push_back(1); });
  engine_->Speak("Second utterance here.", [&](int r) { order.push_back(2); });
  ASSERT_TRUE(RunUntil([&] { return order.size() == 2; }, 20000));
  EXPECT_EQ(order, (std::vector<int>{1, 2}));
  EXPECT_EQ(listener_.Count("start"), 2u);
  EXPECT_EQ(listener_.Count("complete"), 2u);
  EXPECT_EQ(listener_.Count("cancel"), 0u);
}

TEST_F(TtsEngineTest, EmptyTextCompletesImmediately) {
  engine_->SetAwaitSpeakCompletion(true);
  int result = -1;
  engine_->Speak("  ", [&](int r) { result = r; });
  EXPECT_EQ(result, 1);
}

TEST_F(TtsEngineTest, SettingsAccepted) {
  EXPECT_TRUE(engine_->SetSpeechRate(0.7));
  EXPECT_TRUE(engine_->SetVolume(0.8));
  EXPECT_TRUE(engine_->SetPitch(1.2));
  int result = -1;
  engine_->SetAwaitSpeakCompletion(true);
  engine_->Speak(kShort, [&](int r) { result = r; });
  ASSERT_TRUE(RunUntil([&] { return result != -1; }));
  EXPECT_EQ(result, 1);
}

TEST_F(TtsEngineTest, VoicesAndLanguages) {
  const auto voices = engine_->GetVoices();
  ASSERT_FALSE(voices.empty());
  for (const auto& v : voices) {
    EXPECT_FALSE(v.name.empty());
  }
  const auto languages = engine_->GetLanguages();
  ASSERT_FALSE(languages.empty());
  EXPECT_TRUE(std::is_sorted(languages.begin(), languages.end()));

  // Use whatever the first voice's language is, so this works with any
  // output module.
  const std::string locale = voices.front().locale;
  EXPECT_TRUE(engine_->IsLanguageAvailable(locale));
  EXPECT_TRUE(engine_->SetLanguage(locale));
  EXPECT_FALSE(engine_->IsLanguageAvailable("xx-YY"));
  EXPECT_FALSE(engine_->SetLanguage("xx-YY"));

  EXPECT_TRUE(engine_->SetVoice(voices.front().name, ""));
  EXPECT_TRUE(engine_->SetVoice(voices.front().name, voices.front().locale));
  EXPECT_FALSE(engine_->SetVoice("no such voice", ""));
  EXPECT_TRUE(engine_->ClearVoice());

  int result = -1;
  engine_->SetAwaitSpeakCompletion(true);
  engine_->Speak(kShort, [&](int r) { result = r; });
  ASSERT_TRUE(RunUntil([&] { return result != -1; }));
  EXPECT_EQ(result, 1);
}

TEST_F(TtsEngineTest, VariantCombinationsAreCollapsed) {
  const auto voices = engine_->GetVoices();
  std::vector<std::string> names;
  for (const auto& v : voices)
    names.push_back(v.name);
  for (const auto& name : names) {
    const size_t plus = name.find('+');
    if (plus == std::string::npos)
      continue;
    EXPECT_EQ(std::find(names.begin(), names.end(), name.substr(0, plus)),
              names.end())
        << name << " should have been collapsed into its base voice";
  }
}

TEST_F(TtsEngineTest, Engines) {
  const auto engines = engine_->GetEngines();
  ASSERT_FALSE(engines.empty());
  const std::string def = engine_->GetDefaultEngine();
  EXPECT_NE(std::find(engines.begin(), engines.end(), def), engines.end());
  EXPECT_TRUE(engine_->SetEngine(engines.front()));
  EXPECT_FALSE(engine_->SetEngine("no-such-module"));
  EXPECT_FALSE(engine_->GetVoices().empty());
}

TEST_F(TtsEngineTest, TwoEnginesDoNotSeeEachOthersEvents) {
  RecordingListener other_listener;
  TtsEngine other(&other_listener);
  other.SetQueueMode(TtsEngine::kQueueAdd);
  engine_->Speak(kShort, nullptr);
  ASSERT_TRUE(RunUntil([&] { return listener_.Count("complete") == 1; }));
  EXPECT_TRUE(other_listener.events.empty());
}

// Not a fixture test: needs its own engine created with a bad address.
TEST(TtsEngineUnavailable, ReportsErrorWhenDaemonUnreachable) {
  g_setenv("SPEECHD_ADDRESS", "unix_socket:/nonexistent/speechd.sock", TRUE);
  RecordingListener listener;
  {
    TtsEngine engine(&listener);
    int result = -1;
    engine.Speak(kShort, [&](int r) { result = r; });
    EXPECT_EQ(result, 0);
    ASSERT_EQ(listener.errors.size(), 1u);
    EXPECT_NE(listener.errors[0].find("speech-dispatcher"), std::string::npos)
        << listener.errors[0];
    EXPECT_TRUE(engine.GetVoices().empty());
    EXPECT_TRUE(engine.GetLanguages().empty());
    EXPECT_FALSE(engine.SetLanguage("en-US"));
    EXPECT_FALSE(engine.SetSpeechRate(0.5));
    engine.Stop();
    engine.Pause();
  }
  g_unsetenv("SPEECHD_ADDRESS");
}

}  // namespace
}  // namespace flutter_tts

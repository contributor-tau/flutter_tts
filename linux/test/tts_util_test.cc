#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "tts_util.h"

namespace flutter_tts {
namespace {

std::vector<std::string> Chunks(const std::string& text) {
  std::vector<std::string> out;
  for (const TextChunk& c : SplitIntoChunks(text)) {
    out.push_back(text.substr(c.start, c.end - c.start));
  }
  return out;
}

TEST(TtsUtil, RateMapping) {
  EXPECT_EQ(RateToSpeechd(0.0), -100);
  EXPECT_EQ(RateToSpeechd(0.5), 0);
  EXPECT_EQ(RateToSpeechd(0.75), 50);
  EXPECT_EQ(RateToSpeechd(1.0), 100);
  EXPECT_EQ(RateToSpeechd(5.0), 100);
  EXPECT_EQ(RateToSpeechd(-1.0), -100);
}

TEST(TtsUtil, VolumeMapping) {
  EXPECT_EQ(VolumeToSpeechd(0.0), -100);
  EXPECT_EQ(VolumeToSpeechd(0.5), 0);
  EXPECT_EQ(VolumeToSpeechd(1.0), 100);
}

TEST(TtsUtil, PitchMapping) {
  EXPECT_EQ(PitchToSpeechd(0.5), -100);
  EXPECT_EQ(PitchToSpeechd(0.75), -50);
  EXPECT_EQ(PitchToSpeechd(1.0), 0);
  EXPECT_EQ(PitchToSpeechd(1.5), 50);
  EXPECT_EQ(PitchToSpeechd(2.0), 100);
  EXPECT_EQ(PitchToSpeechd(0.0), -100);
  EXPECT_EQ(PitchToSpeechd(3.0), 100);
}

TEST(TtsUtil, NormalizeLocale) {
  EXPECT_EQ(NormalizeLocale("en-us"), "en-US");
  EXPECT_EQ(NormalizeLocale("EN_gb"), "en-GB");
  EXPECT_EQ(NormalizeLocale("en"), "en");
  EXPECT_EQ(NormalizeLocale("cmn"), "cmn");
  EXPECT_EQ(NormalizeLocale("en-029"), "en-029");
  EXPECT_EQ(NormalizeLocale("zh-hans-cn"), "zh-Hans-CN");
  EXPECT_EQ(NormalizeLocale("en-GB-SCOTLAND"), "en-GB-scotland");
  EXPECT_EQ(NormalizeLocale("en-GB-X-GBCLAN"), "en-GB-x-gbclan");
  EXPECT_EQ(NormalizeLocale(""), "");
}

TEST(TtsUtil, LanguageMatchScore) {
  EXPECT_EQ(LanguageMatchScore("en-US", "en-us"), 2);
  EXPECT_EQ(LanguageMatchScore("en_US", "en-US"), 2);
  EXPECT_EQ(LanguageMatchScore("en", "en-US"), 1);
  EXPECT_EQ(LanguageMatchScore("en-US", "en"), 1);
  EXPECT_EQ(LanguageMatchScore("en-US", "en-GB"), 0);
  EXPECT_EQ(LanguageMatchScore("en", "de"), 0);
  EXPECT_EQ(LanguageMatchScore("", "en"), 0);
}

TEST(TtsUtil, SplitsSentences) {
  EXPECT_EQ(Chunks("Hello there, world. How are you today? I am fine!"),
            (std::vector<std::string>{"Hello there, world.",
                                      "How are you today?", "I am fine!"}));
}

TEST(TtsUtil, SingleSentenceAndEmpty) {
  EXPECT_EQ(Chunks("No terminator here"),
            (std::vector<std::string>{"No terminator here"}));
  EXPECT_TRUE(Chunks("").empty());
  EXPECT_TRUE(Chunks("   \n\t  ").empty());
}

TEST(TtsUtil, KeepsAbbreviationsAndNumbers) {
  // "Dr." is shorter than the minimum chunk length; 3.14 has no space.
  EXPECT_EQ(Chunks("Dr. Smith paid 3.14 dollars. Then he left."),
            (std::vector<std::string>{"Dr. Smith paid 3.14 dollars.",
                                      "Then he left."}));
}

TEST(TtsUtil, AbsorbsClosersAndRepeatedTerminators) {
  EXPECT_EQ(Chunks("He shouted \"Run away now!\" They ran. Really?! Yes..."),
            (std::vector<std::string>{"He shouted \"Run away now!\"",
                                      "They ran. Really?!", "Yes..."}));
}

TEST(TtsUtil, SplitsLines) {
  EXPECT_EQ(Chunks("Line one\n\nLine two\r\nok"),
            (std::vector<std::string>{"Line one", "Line two", "ok"}));
}

TEST(TtsUtil, SplitsCjkWithoutSpaces) {
  EXPECT_EQ(Chunks("今日は良い天気です。明日も晴れるでしょう。"),
            (std::vector<std::string>{"今日は良い天気です。",
                                      "明日も晴れるでしょう。"}));
}

TEST(TtsUtil, Utf16Length) {
  const std::string text = "a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80z";  // aé€😀z
  EXPECT_EQ(Utf16Length(text, 0, text.size()), 6u);
  EXPECT_EQ(Utf16Length(text, 0, 1), 1u);
  EXPECT_EQ(Utf16Length(text, 0, 3), 2u);
  EXPECT_EQ(Utf16Length(text, 6, 10), 2u);
  EXPECT_EQ(Utf16Length(text, 0, 1000), 6u);
}

}  // namespace
}  // namespace flutter_tts

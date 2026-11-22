// Pure helper functions used by the Linux implementation. Kept free of
// Flutter and Speech Dispatcher dependencies so they can be unit-tested.

#ifndef FLUTTER_TTS_TTS_UTIL_H_
#define FLUTTER_TTS_TTS_UTIL_H_

#include <cstddef>
#include <string>
#include <vector>

namespace flutter_tts {

// A byte range [start, end) of a UTF-8 string.
struct TextChunk {
  size_t start;
  size_t end;
};

// Splits UTF-8 text into sentence-sized chunks that are spoken as separate
// Speech Dispatcher messages. This is what makes pause/resume reliable (a
// paused utterance resumes at the start of the interrupted sentence) and
// provides sentence-level progress events.
//
// A chunk ends after a sentence terminator (. ! ? … 。 ！ ？) that is
// followed by optional closing quotes/brackets and then whitespace or the
// end of the text (CJK terminators need no following whitespace), or at a
// line break. To avoid splitting after abbreviations such as "Dr.", a
// sentence terminator only ends a chunk once the chunk is at least
// |kMinChunkBytes| long. Leading and trailing whitespace is excluded from
// chunks and whitespace-only chunks are dropped.
std::vector<TextChunk> SplitIntoChunks(const std::string& text);
constexpr size_t kMinChunkBytes = 12;

// Number of UTF-16 code units in text[byte_start, byte_end). Dart string
// indices are UTF-16 based, so this converts byte offsets for progress
// events.
size_t Utf16Length(const std::string& text, size_t byte_start, size_t byte_end);

// flutter_tts speech rate: 0.0 (slowest) .. 1.0 (fastest), 0.5 is normal.
// Speech Dispatcher rate: -100 .. 100, 0 is normal.
int RateToSpeechd(double rate);

// flutter_tts volume: 0.0 (silent) .. 1.0 (loudest).
// Speech Dispatcher volume: -100 .. 100, where 100 is its default (loudest).
int VolumeToSpeechd(double volume);

// flutter_tts pitch: 0.5 .. 2.0, 1.0 is normal.
// Speech Dispatcher pitch: -100 .. 100, 0 is normal. The mapping is
// piecewise linear so that 1.0 maps to exactly 0.
int PitchToSpeechd(double pitch);

// Converts a Speech Dispatcher language code ("en-us", "EN_gb", "pt-br",
// "zh-hans-cn") to BCP-47 casing ("en-US", "en-GB", "pt-BR", "zh-Hans-CN").
// Underscores are converted to hyphens.
std::string NormalizeLocale(const std::string& language);

// How well a voice locale satisfies a requested locale. Both are normalised
// before comparison.
//   2: exact match ("en-US" / "en-us")
//   1: same primary language where one side has no region
//      ("en" / "en-US", "en-US" / "en")
//   0: no match ("en-US" / "en-GB", "en" / "de")
int LanguageMatchScore(const std::string& requested,
                       const std::string& voice_locale);

}  // namespace flutter_tts

#endif  // FLUTTER_TTS_TTS_UTIL_H_

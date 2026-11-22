#include "tts_util.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <vector>

namespace flutter_tts {

namespace {

int ClampToSpeechd(double value) {
  if (std::isnan(value))
    return 0;
  const double rounded = std::round(value);
  return static_cast<int>(std::max(-100.0, std::min(100.0, rounded)));
}

std::vector<std::string> SplitSubtags(const std::string& language) {
  std::vector<std::string> parts;
  std::string current;
  for (char c : language) {
    if (c == '-' || c == '_') {
      if (!current.empty())
        parts.push_back(current);
      current.clear();
    } else if (!std::isspace(static_cast<unsigned char>(c))) {
      current.push_back(c);
    }
  }
  if (!current.empty())
    parts.push_back(current);
  return parts;
}

std::string Lower(std::string s) {
  for (char& c : s)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string Upper(std::string s) {
  for (char& c : s)
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

bool IsAlpha(const std::string& s) {
  return std::all_of(s.begin(), s.end(), [](char c) {
    return std::isalpha(static_cast<unsigned char>(c)) != 0;
  });
}

bool IsDigits(const std::string& s) {
  return std::all_of(s.begin(), s.end(), [](char c) {
    return std::isdigit(static_cast<unsigned char>(c)) != 0;
  });
}

bool IsRegion(const std::string& subtag) {
  return (subtag.size() == 2 && IsAlpha(subtag)) ||
         (subtag.size() == 3 && IsDigits(subtag));
}

bool IsSpaceByte(unsigned char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' ||
         c == '\v';
}

bool StartsWith(const std::string& text, size_t pos, const char* seq) {
  const size_t len = std::char_traits<char>::length(seq);
  return pos + len <= text.size() && text.compare(pos, len, seq) == 0;
}

// Multi-byte sentence terminators. |needs_space| is false for CJK
// punctuation, which is not followed by spaces.
struct Terminator {
  const char* utf8;
  bool needs_space;
};
constexpr Terminator kTerminators[] = {
    {".", true},
    {"!", true},
    {"?", true},
    {"\xE2\x80\xA6", true},   // … HORIZONTAL ELLIPSIS
    {"\xE3\x80\x82", false},  // 。 IDEOGRAPHIC FULL STOP
    {"\xEF\xBC\x81", false},  // ！ FULLWIDTH EXCLAMATION MARK
    {"\xEF\xBC\x9F", false},  // ？ FULLWIDTH QUESTION MARK
};

// Closing punctuation that may follow a terminator: " ' ) ] ” ’ » 」 』.
constexpr const char* kClosers[] = {
    "\"",           "'", ")", "]",
    "\xE2\x80\x9D",  // ”
    "\xE2\x80\x99",  // ’
    "\xC2\xBB",      // »
    "\xE3\x80\x8D",  // 」
    "\xE3\x80\x8F",  // 』
};

size_t MatchCloser(const std::string& text, size_t pos) {
  for (const char* closer : kClosers) {
    if (StartsWith(text, pos, closer)) {
      return std::char_traits<char>::length(closer);
    }
  }
  return 0;
}

void AddChunk(const std::string& text,
              size_t start,
              size_t end,
              std::vector<TextChunk>* chunks) {
  while (start < end && IsSpaceByte(static_cast<unsigned char>(text[start]))) {
    ++start;
  }
  while (end > start &&
         IsSpaceByte(static_cast<unsigned char>(text[end - 1]))) {
    --end;
  }
  if (start < end)
    chunks->push_back({start, end});
}

}  // namespace

std::vector<TextChunk> SplitIntoChunks(const std::string& text) {
  std::vector<TextChunk> chunks;
  size_t chunk_start = 0;
  size_t i = 0;
  while (i < text.size()) {
    if (text[i] == '\n') {
      AddChunk(text, chunk_start, i, &chunks);
      chunk_start = ++i;
      continue;
    }

    const Terminator* matched = nullptr;
    for (const Terminator& t : kTerminators) {
      if (StartsWith(text, i, t.utf8)) {
        matched = &t;
        break;
      }
    }
    if (matched == nullptr) {
      ++i;
      continue;
    }

    size_t end = i + std::char_traits<char>::length(matched->utf8);
    // Absorb repeated terminators ("?!", "...") and closing punctuation.
    for (;;) {
      size_t n = MatchCloser(text, end);
      if (n == 0) {
        for (const Terminator& t : kTerminators) {
          if (StartsWith(text, end, t.utf8)) {
            n = std::char_traits<char>::length(t.utf8);
            break;
          }
        }
      }
      if (n == 0)
        break;
      end += n;
    }

    const bool at_boundary =
        end >= text.size() ||
        IsSpaceByte(static_cast<unsigned char>(text[end])) ||
        !matched->needs_space;
    if (at_boundary && end - chunk_start >= kMinChunkBytes) {
      AddChunk(text, chunk_start, end, &chunks);
      chunk_start = end;
    }
    i = end;
  }
  AddChunk(text, chunk_start, text.size(), &chunks);
  return chunks;
}

size_t Utf16Length(const std::string& text,
                   size_t byte_start,
                   size_t byte_end) {
  byte_end = std::min(byte_end, text.size());
  size_t units = 0;
  for (size_t i = byte_start; i < byte_end; ++i) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if ((c & 0xC0) == 0x80)
      continue;                    // Continuation byte.
    units += (c >= 0xF0) ? 2 : 1;  // 4-byte sequences are surrogate pairs.
  }
  return units;
}

int RateToSpeechd(double rate) {
  return ClampToSpeechd((rate - 0.5) * 200.0);
}

int VolumeToSpeechd(double volume) {
  return ClampToSpeechd(volume * 200.0 - 100.0);
}

int PitchToSpeechd(double pitch) {
  if (pitch < 1.0)
    return ClampToSpeechd((pitch - 1.0) * 200.0);
  return ClampToSpeechd((pitch - 1.0) * 100.0);
}

std::string NormalizeLocale(const std::string& language) {
  const std::vector<std::string> parts = SplitSubtags(language);
  std::string out;
  for (size_t i = 0; i < parts.size(); ++i) {
    std::string part = parts[i];
    if (i == 0) {
      part = Lower(part);
    } else if (IsRegion(part)) {
      part = Upper(part);
    } else if (part.size() == 4 && IsAlpha(part)) {
      part = Lower(part);
      part[0] =
          static_cast<char>(std::toupper(static_cast<unsigned char>(part[0])));
    } else {
      part = Lower(part);
    }
    if (!out.empty())
      out.push_back('-');
    out.append(part);
  }
  return out;
}

int LanguageMatchScore(const std::string& requested,
                       const std::string& voice_locale) {
  const std::string a = Lower(NormalizeLocale(requested));
  const std::string b = Lower(NormalizeLocale(voice_locale));
  if (a.empty() || b.empty())
    return 0;
  if (a == b)
    return 2;

  const std::vector<std::string> pa = SplitSubtags(a);
  const std::vector<std::string> pb = SplitSubtags(b);
  if (pa[0] != pb[0])
    return 0;

  auto region_of = [](const std::vector<std::string>& parts) {
    for (size_t i = 1; i < parts.size(); ++i) {
      if (IsRegion(parts[i]))
        return parts[i];
    }
    return std::string();
  };
  const std::string ra = region_of(pa);
  const std::string rb = region_of(pb);
  if (ra.empty() || rb.empty())
    return 1;
  return 0;
}

}  // namespace flutter_tts

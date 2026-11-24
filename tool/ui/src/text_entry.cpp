// The tool's text entry (Tela has no text input).
#include "snd/tool/ui/text_entry.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "snd/tool/ui/control_math.hpp"

namespace snd::tool::ui {

void AppendUtf8(std::string* s, std::uint32_t c) {
  if (c < 0x80u) {
    s->push_back(static_cast<char>(c));
  } else if (c < 0x800u) {
    s->push_back(static_cast<char>(0xC0u | (c >> 6)));
    s->push_back(static_cast<char>(0x80u | (c & 0x3Fu)));
  } else if (c < 0x10000u) {
    if (c >= 0xD800u && c <= 0xDFFFu) return;  // surrogates are not characters
    s->push_back(static_cast<char>(0xE0u | (c >> 12)));
    s->push_back(static_cast<char>(0x80u | ((c >> 6) & 0x3Fu)));
    s->push_back(static_cast<char>(0x80u | (c & 0x3Fu)));
  } else if (c <= 0x10FFFFu) {
    s->push_back(static_cast<char>(0xF0u | (c >> 18)));
    s->push_back(static_cast<char>(0x80u | ((c >> 12) & 0x3Fu)));
    s->push_back(static_cast<char>(0x80u | ((c >> 6) & 0x3Fu)));
    s->push_back(static_cast<char>(0x80u | (c & 0x3Fu)));
  }
}

void PopUtf8(std::string* s) {
  while (!s->empty()) {
    const unsigned char c = static_cast<unsigned char>(s->back());
    s->pop_back();
    if ((c & 0xC0u) != 0x80u) return;  // removed the first byte of the character
  }
}

bool ParseNumber(const std::string& text, double* out) {
  // Trim spaces; the C locale's strtod ("." as the decimal point: the tool sets no locale).
  std::size_t a = 0, b = text.size();
  while (a < b && text[a] == ' ') ++a;
  while (b > a && text[b - 1] == ' ') --b;
  if (a == b || b - a > 64) return false;
  const std::string t = text.substr(a, b - a);
  for (char ch : t) {
    const bool ok = (ch >= '0' && ch <= '9') || ch == '-' || ch == '+' || ch == '.' || ch == 'e' || ch == 'E';
    if (!ok) return false;  // no "inf", "nan", hex
  }
  char* end = nullptr;
  const double v = std::strtod(t.c_str(), &end);
  if (end != t.c_str() + t.size() || !std::isfinite(v)) return false;
  *out = v;
  return true;
}

std::string FormatNumberText(double v, int decimals) {
  if (!std::isfinite(v)) return "0";
  if (decimals < 0) decimals = 0;
  if (decimals > 9) decimals = 9;
  char b[64];
  std::snprintf(b, sizeof b, "%.*f", decimals, v);
  std::string s = b;
  if (s.find('.') != std::string::npos) {
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
  }
  if (s == "-0") s = "0";
  return s;
}

void TextEntry::Begin(std::string field, std::string text, Commit commit, bool numeric) {
  active_ = true;
  numeric_ = numeric;
  field_ = std::move(field);
  text_ = text.size() > kMaxBytes ? text.substr(0, kMaxBytes) : std::move(text);
  commit_ = std::move(commit);
}

void TextEntry::Cancel() {
  active_ = false;
  field_.clear();
  text_.clear();
  commit_ = nullptr;
}

bool TextEntry::CommitNow() {
  if (!active_) return false;
  const Commit fn = commit_;  // the commit may start another entry
  const std::string text = text_;
  const std::string field = field_;
  if (fn && !fn(text)) return false;
  if (active_ && field_ == field) Cancel();
  return true;
}

bool TextEntry::Key(const KeyEvent& k, bool* refused) {
  if (!active_) return false;
  if (refused != nullptr) *refused = false;
  if (k.key == kKeyEnter) {
    if (!CommitNow() && refused != nullptr) *refused = true;
    return true;
  }
  if (k.key == kKeyEscape) {
    Cancel();
    return true;
  }
  if (k.key == kKeyBackspace) {
    PopUtf8(&text_);
    return true;
  }
  if ((k.modifiers & kModControl) != 0u) return true;  // shortcuts do nothing while typing (D-186)
  if (k.text >= 0x20u && k.text != 0x7Fu) {
    if (numeric_) {
      const bool ok = (k.text >= '0' && k.text <= '9') || k.text == '-' || k.text == '+' || k.text == '.' ||
                      k.text == 'e' || k.text == 'E';
      if (!ok) return true;  // swallowed: a number field takes no letters
    }
    std::string next = text_;
    AppendUtf8(&next, k.text);
    if (next.size() <= kMaxBytes) text_ = std::move(next);
    return true;
  }
  // Other keys (arrows, Delete, Tab, F-keys) while typing: used up so that they do not edit the
  // selection or change the screen behind the field (D-186).
  return true;
}

}  // namespace snd::tool::ui

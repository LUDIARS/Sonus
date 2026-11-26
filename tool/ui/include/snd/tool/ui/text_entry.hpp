// Keyboard input and the tool's own text entry (Tela has no text input; one workaround for
// every screen, tool/README.md "文字の入力").
//
// Hosts observe the keyboard (Windows: WM_KEYDOWN / WM_CHAR, macOS: the local NSEvent monitor)
// and hand every key to App::Key as a KeyEvent: `key` names the key (letters as their upper
// case ASCII code, digits as ASCII, the others kKey*), `text` the character it typed (a Unicode
// code point, 0 if none). One event may carry both (macOS) or each come alone (Windows sends
// WM_KEYDOWN with the key, then WM_CHAR with the text).
//
// TextEntry: a field that is being typed into. A click on a name or number field starts it
// with the current text; typed characters are appended (UTF-8), Backspace removes the last
// character, Enter commits (the field's commit function: the model's setter; refused -> the
// entry stays open with a message), Escape cancels. Starting another entry, changing the screen
// or a commit that succeeded ends it. Numeric entries take digits, sign, '.', 'e' only. While an
// entry is open it takes every key (D-186): Ctrl+Z / Ctrl+Y, Delete and the screen keys do nothing.
#ifndef SND_TOOL_UI_TEXT_ENTRY_HPP
#define SND_TOOL_UI_TEXT_ENTRY_HPP

#include <cstdint>
#include <functional>
#include <string>

namespace snd::tool::ui {

inline constexpr std::uint32_t kKeyEnter = 0x100;
inline constexpr std::uint32_t kKeyEscape = 0x101;
inline constexpr std::uint32_t kKeyBackspace = 0x102;
inline constexpr std::uint32_t kKeyDelete = 0x103;
inline constexpr std::uint32_t kKeyTab = 0x104;
inline constexpr std::uint32_t kKeyLeft = 0x105;
inline constexpr std::uint32_t kKeyRight = 0x106;
inline constexpr std::uint32_t kKeyUp = 0x107;
inline constexpr std::uint32_t kKeyDown = 0x108;
inline constexpr std::uint32_t kKeyHome = 0x109;
inline constexpr std::uint32_t kKeyEnd = 0x10A;
inline constexpr std::uint32_t kKeyPageUp = 0x10B;
inline constexpr std::uint32_t kKeyPageDown = 0x10C;
inline constexpr std::uint32_t kKeyF1 = 0x110;  // kKeyF1 + n - 1 for Fn (1..12)

struct KeyEvent {
  std::uint32_t key = 0;        // 'A'..'Z', '0'..'9', kKey*; 0 = only text
  std::uint32_t text = 0;       // typed character (code point), 0 = none
  std::uint32_t modifiers = 0;  // kModShift, kModControl (control_math.hpp; Cmd on macOS)
};

// Appends a code point as UTF-8 (invalid code points are ignored).
void AppendUtf8(std::string* s, std::uint32_t codepoint);
// Removes the last UTF-8 character.
void PopUtf8(std::string* s);
// A number as typed ("12", "-3.5", "1e3"): false if the whole text is not one finite number.
bool ParseNumber(const std::string& text, double* out);
// The shortest text that reads back as `v`, at most `decimals` after the point.
std::string FormatNumberText(double v, int decimals = 4);

class TextEntry {
 public:
  using Commit = std::function<bool(const std::string& text)>;
  static constexpr std::size_t kMaxBytes = 256;

  void Begin(std::string field, std::string text, Commit commit, bool numeric);
  void Cancel();
  bool active() const { return active_; }
  const std::string& field() const { return field_; }
  const std::string& text() const { return text_; }
  bool numeric() const { return numeric_; }
  // A key while active (every key is used: true). `refused` is set when Enter was refused by the
  // commit.
  bool Key(const KeyEvent& key, bool* refused = nullptr);
  // Commits as Enter does (true: committed and ended).
  bool CommitNow();

 private:
  bool active_ = false;
  bool numeric_ = false;
  std::string field_;
  std::string text_;
  Commit commit_;
};

}  // namespace snd::tool::ui

#endif  // SND_TOOL_UI_TEXT_ENTRY_HPP

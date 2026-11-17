// Command line of sonus-tool.
#ifndef SND_TOOL_APP_OPTIONS_HPP
#define SND_TOOL_APP_OPTIONS_HPP

#include <cstdint>
#include <string>

namespace snd::tool::app {

struct Options {
  std::string project;           // --project <dir>
  std::string buildDir;          // --build-dir <dir> (default <project>/.sonus-tool-build)
  std::string address = "127.0.0.1";
  std::uint16_t port = 27460;    // --connect host:port
  bool connect = true;           // --no-connect
  bool refuseMismatch = false;   // --refuse-mismatch
  bool headless = false;         // --headless
  std::string snapshot;          // --snapshot <file.ppm>
  std::string screen = "mixer";  // --screen mixer|profiler
  std::uint32_t frames = 30;     // --frames <n> (headless)
  std::uint32_t frameMs = 33;    // --frame-ms <ms>
  int width = 1280, height = 800;  // --size WxH
  float dpi = 1.0f;              // --dpi <scale>
  bool textPlaceholders = false; // --text-placeholders
  std::string font;              // --font <ttf> (window hosts; headless text when Tela has Pictor)
  std::string recording = "sonus-recording.snrec";  // --recording <path>
  bool help = false;
};

// false and `error` on a malformed command line.
bool ParseOptions(int argc, char** argv, Options* out, std::string* error);
const char* Usage();

}  // namespace snd::tool::app

#endif  // SND_TOOL_APP_OPTIONS_HPP

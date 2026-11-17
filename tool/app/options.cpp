// Command line of sonus-tool.
#include "options.hpp"

#include <charconv>
#include <cstring>

#include "snd/tool/ui/safe_drawing.hpp"

namespace snd::tool::app {
namespace {

template <class T>
bool Number(const char* s, T* out) {
  const char* end = s + std::strlen(s);
  auto r = std::from_chars(s, end, *out);
  return r.ec == std::errc() && r.ptr == end;
}

}  // namespace

const char* Usage() {
  return "usage: sonus-tool [options]\n"
         "  --project <dir>        project to edit (spec/interface/project-format.md)\n"
         "  --build-dir <dir>      build output for the project hash (default <project>/.sonus-tool-build)\n"
         "  --connect <host:port>  game to connect to (default 127.0.0.1:27460)\n"
         "  --no-connect           start offline\n"
         "  --refuse-mismatch      disconnect when the game's build differs (default: send the tool's values)\n"
         "  --font <file.ttf>      TrueType font for text (window hosts; required there)\n"
         "  --recording <path>     file used by Save / Load of the profiler's recording\n"
         "  --headless             no window: run frames and write a snapshot with Tela::Raster\n"
         "  --snapshot <file.ppm>  headless: image written after the last frame\n"
         "  --screen <mixer|profiler>  headless: screen to show\n"
         "  --frames <n>           headless: frames to run (default 30)\n"
         "  --frame-ms <ms>        headless: time between frames (default 33)\n"
         "  --size <W>x<H>         window / snapshot size in physical pixels (default 1280x800)\n"
         "  --dpi <scale>          headless DPI scale, 0.25 .. 4 (default 1)\n"
         "  --text-placeholders    headless without Pictor: draw bars where text goes\n";
}

bool ParseOptions(int argc, char** argv, Options* o, std::string* error) {
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto value = [&](const char** v) {
      if (i + 1 >= argc) {
        *error = "missing value after " + a;
        return false;
      }
      *v = argv[++i];
      return true;
    };
    const char* v = nullptr;
    if (a == "--help" || a == "-h") {
      o->help = true;
    } else if (a == "--project") {
      if (!value(&v)) return false;
      o->project = v;
    } else if (a == "--build-dir") {
      if (!value(&v)) return false;
      o->buildDir = v;
    } else if (a == "--connect") {
      if (!value(&v)) return false;
      const std::string s = v;
      const std::size_t colon = s.rfind(':');
      if (colon == std::string::npos || colon == 0 || !Number(s.c_str() + colon + 1, &o->port)) {
        *error = "--connect expects host:port";
        return false;
      }
      o->address = s.substr(0, colon);
      o->connect = true;
    } else if (a == "--no-connect") {
      o->connect = false;
    } else if (a == "--refuse-mismatch") {
      o->refuseMismatch = true;
    } else if (a == "--headless") {
      o->headless = true;
    } else if (a == "--snapshot") {
      if (!value(&v)) return false;
      o->snapshot = v;
    } else if (a == "--screen") {
      if (!value(&v)) return false;
      o->screen = v;
      if (o->screen != "mixer" && o->screen != "profiler") {
        *error = "--screen expects mixer or profiler";
        return false;
      }
    } else if (a == "--frames") {
      if (!value(&v) || !Number(v, &o->frames)) return *error = "--frames expects a number", false;
    } else if (a == "--frame-ms") {
      if (!value(&v) || !Number(v, &o->frameMs)) return *error = "--frame-ms expects a number", false;
    } else if (a == "--size") {
      if (!value(&v)) return false;
      const std::string s = v;
      const std::size_t x = s.find('x');
      if (x == std::string::npos || !Number(s.substr(0, x).c_str(), &o->width) ||
          !Number(s.substr(x + 1).c_str(), &o->height) || o->width < 320 || o->height < 240 || o->width > 8192 ||
          o->height > 8192) {
        *error = "--size expects WxH (320x240 .. 8192x8192)";
        return false;
      }
    } else if (a == "--dpi") {
      // The same range as App::SetViewport (safe_drawing.hpp kMinDpiScale .. kMaxDpiScale).
      if (!value(&v) || !Number(v, &o->dpi) || !(o->dpi >= ui::kMinDpiScale && o->dpi <= ui::kMaxDpiScale)) {
        *error = "--dpi expects 0.25 .. 4";
        return false;
      }
    } else if (a == "--text-placeholders") {
      o->textPlaceholders = true;
    } else if (a == "--font") {
      if (!value(&v)) return false;
      o->font = v;
    } else if (a == "--recording") {
      if (!value(&v)) return false;
      o->recording = v;
    } else {
      *error = "unknown option " + a;
      return false;
    }
  }
  if (o->buildDir.empty() && !o->project.empty()) o->buildDir = o->project + "/.sonus-tool-build";
  return true;
}

}  // namespace snd::tool::app

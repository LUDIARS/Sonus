// Command line of sonus-tool.
#include "options.hpp"

#include <charconv>
#include <cstring>

#include "snd/tool/ui/safe_drawing.hpp"
#include "snd/tool/ui/views.hpp"

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
         "  --refuse-mismatch      disconnect when the game's build differs\n"
         "  --send-diffs           send the tool's data when the game's build differs (default: ask; without a\n"
         "                         project: send the mixer's values)\n"
         "  --preview <silent|device>    run the preview engine (a runtime in the tool) and connect to it\n"
         "  --reload-dir <dir>     hot reloads write rebuilt banks there ...\n"
         "  --reload-prefix <p>    ... which the game reads as <p> (default: the directory as given)\n"
         "  --report-dir <dir>     the replay screen's bug report folder (default sonus-report)\n"
         "  --font <file.ttf>      TrueType font for text (window hosts; required there)\n"
         "  --recording <path>     file used by Save / Load of the profiler's recording\n"
         "  --headless             no window: run frames and write a snapshot with Tela::Raster\n"
         "  --snapshot <file.ppm>  headless: image written after the last frame\n"
         "  --screen <name>        headless: screen to show (mixer buses event params spatial music material\n"
         "                         patch profiler replay link problems)\n"
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
    } else if (a == "--send-diffs") {
      o->sendDiffs = true;
    } else if (a == "--preview") {
      if (!value(&v)) return false;
      o->preview = v;
      if (o->preview != "silent" && o->preview != "device") {
        *error = "--preview expects silent or device";
        return false;
      }
    } else if (a == "--reload-dir") {
      if (!value(&v)) return false;
      o->reloadDir = v;
    } else if (a == "--reload-prefix") {
      if (!value(&v)) return false;
      o->reloadPrefix = v;
    } else if (a == "--report-dir") {
      if (!value(&v)) return false;
      o->reportDir = v;
    } else if (a == "--headless") {
      o->headless = true;
    } else if (a == "--snapshot") {
      if (!value(&v)) return false;
      o->snapshot = v;
    } else if (a == "--screen") {
      if (!value(&v)) return false;
      o->screen = v;
      ui::Screen sc;
      if (!ui::ParseScreen(o->screen, &sc)) {
        *error = "--screen expects one of mixer buses event params spatial music material patch profiler replay "
                 "link problems";
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
  if (o->refuseMismatch && o->sendDiffs) {
    *error = "--refuse-mismatch and --send-diffs exclude each other";
    return false;
  }
  return true;
}

}  // namespace snd::tool::app

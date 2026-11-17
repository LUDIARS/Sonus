// PPM output of a Tela pixel surface.
#include "snd/tool/ui/image_file.hpp"

#include <cstdio>
#include <vector>

namespace snd::tool::ui {

bool PixelRgb(const tela::PixelSurface& s, int x, int y, unsigned char rgb[3]) {
  if (x < 0 || y < 0 || x >= s.width || y >= s.height) return false;
  const std::size_t i = (static_cast<std::size_t>(y) * static_cast<std::size_t>(s.width) + static_cast<std::size_t>(x)) * 4u;
  rgb[0] = s.pixels[i + 2];
  rgb[1] = s.pixels[i + 1];
  rgb[2] = s.pixels[i + 0];
  return true;
}

bool WritePpm(const tela::PixelSurface& s, const std::string& path) {
  if (s.width <= 0 || s.height <= 0 ||
      s.pixels.size() != static_cast<std::size_t>(s.width) * static_cast<std::size_t>(s.height) * 4u) {
    return false;
  }
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (f == nullptr) return false;
  std::fprintf(f, "P6\n%d %d\n255\n", s.width, s.height);
  std::vector<unsigned char> row(static_cast<std::size_t>(s.width) * 3u);
  bool ok = true;
  for (int y = 0; y < s.height && ok; ++y) {
    for (int x = 0; x < s.width; ++x) PixelRgb(s, x, y, &row[static_cast<std::size_t>(x) * 3u]);
    ok = std::fwrite(row.data(), 1, row.size(), f) == row.size();
  }
  return std::fclose(f) == 0 && ok;
}

}  // namespace snd::tool::ui

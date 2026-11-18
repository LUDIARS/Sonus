// Recording files (design.md 11.3).
#include "snd/tool/recording_file.hpp"

#include <cstdio>
#include <cstring>

#include "snd/livelink/record_format.hpp"

namespace snd::tool {
namespace {

void Put32(std::uint8_t* p, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}
void Put64(std::uint8_t* p, std::uint64_t v) {
  for (int i = 0; i < 8; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}
std::uint32_t Get32(const std::uint8_t* p) {
  std::uint32_t v = 0;
  for (int i = 3; i >= 0; --i) v = (v << 8) | p[i];
  return v;
}
std::uint64_t Get64(const std::uint8_t* p) {
  std::uint64_t v = 0;
  for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
  return v;
}

}  // namespace


namespace {
// FNV-1a 64 over the header without its checksum field, then the records.
std::uint64_t Checksum(const std::uint8_t* header, const std::uint8_t* records, std::uint64_t bytes) {
  std::uint64_t h = 0xcbf29ce484222325ull;
  auto feed = [&h](const std::uint8_t* p, std::uint64_t n) {
    for (std::uint64_t i = 0; i < n; ++i) {
      h ^= p[i];
      h *= 0x100000001b3ull;
    }
  };
  feed(header, 56);
  feed(records, bytes);
  return h;
}
}  // namespace

std::uint64_t Fnv1a64(const std::uint8_t* data, std::uint64_t bytes) {
  std::uint64_t h = 0xcbf29ce484222325ull;
  for (std::uint64_t i = 0; i < bytes; ++i) {
    h ^= data[i];
    h *= 0x100000001b3ull;
  }
  return h;
}

RecordingSummary Summarize(const std::uint8_t* data, std::uint64_t bytes) {
  RecordingSummary s;
  s.bytes = bytes;
  livelink::RecordReader reader(data, bytes);
  livelink::RecordItem item;
  bool first = true;
  auto block = [&](std::uint64_t b) {
    if (first || b < s.firstBlock) s.firstBlock = b;
    if (first || b > s.lastBlock) s.lastBlock = b;
    first = false;
  };
  while (reader.Next(&item)) {
    switch (item.kind) {
      case livelink::RecordKind::Arena:
        ++s.arenas;
        block(item.arena.appliedBlock);
        break;
      case livelink::RecordKind::Lod:
        ++s.lods;
        block(item.lod.block);
        break;
      case livelink::RecordKind::Stall:
        ++s.stalls;
        block(item.stall.block);
        break;
      case livelink::RecordKind::BankMap: ++s.bankMaps; break;
    }
  }
  if (reader.Error() != livelink::RecordError::None) {
    s.valid = false;
    s.error = livelink::RecordErrorName(reader.Error());
    s.errorOffset = reader.Offset();
  }
  return s;
}

std::vector<std::uint8_t> EncodeRecording(const Recording& r) {
  std::vector<std::uint8_t> out(kRecordingHeaderBytes + r.records.size());
  std::uint8_t* h = out.data();
  std::memcpy(h, "SNRC", 4);
  Put32(h + 4, kRecordingFileVersion);
  Put32(h + 8, kRecordingHeaderBytes);
  Put32(h + 12, r.info.flags);
  Put64(h + 16, r.info.projectBuildHash);
  Put64(h + 24, r.info.runtimeBuildHash);
  Put32(h + 32, r.info.sampleRate);
  Put32(h + 36, r.info.blockSize);
  Put32(h + 40, r.info.platform);
  Put32(h + 44, r.info.contractVersion);
  Put64(h + 48, r.records.size());
  if (!r.records.empty()) std::memcpy(h + kRecordingHeaderBytes, r.records.data(), r.records.size());
  Put64(h + 56, Checksum(out.data(), r.records.data(), r.records.size()));
  return out;
}

bool DecodeRecording(const std::uint8_t* data, std::uint64_t bytes, Recording* out, std::string* error) {
  auto fail = [&](const char* m) {
    if (error != nullptr) *error = m;
    return false;
  };
  if (bytes < kRecordingHeaderBytes) return fail("file shorter than the header");
  if (std::memcmp(data, "SNRC", 4) != 0) return fail("not a Sonus recording (magic)");
  if (Get32(data + 4) != kRecordingFileVersion) return fail("unsupported recording version");
  const std::uint32_t headerBytes = Get32(data + 8);
  if (headerBytes < kRecordingHeaderBytes || headerBytes > bytes) return fail("bad header size");
  const std::uint64_t recordBytes = Get64(data + 48);
  if (recordBytes != bytes - headerBytes) return fail("record size does not match the file size");
  const std::uint8_t* records = data + headerBytes;
  if (Checksum(data, records, recordBytes) != Get64(data + 56)) return fail("checksum mismatch");
  const RecordingSummary summary = Summarize(records, recordBytes);
  if (!summary.valid) return fail("unreadable records");
  Recording r;
  r.info.flags = Get32(data + 12);
  r.info.projectBuildHash = Get64(data + 16);
  r.info.runtimeBuildHash = Get64(data + 24);
  r.info.sampleRate = Get32(data + 32);
  r.info.blockSize = Get32(data + 36);
  r.info.platform = Get32(data + 40);
  r.info.contractVersion = Get32(data + 44);
  r.records.assign(records, records + recordBytes);
  *out = std::move(r);
  return true;
}

bool SaveRecording(const std::string& path, const Recording& r, std::string* error) {
  const std::vector<std::uint8_t> bytes = EncodeRecording(r);
  const std::string tmp = path + ".sonus-tmp";
  std::FILE* f = std::fopen(tmp.c_str(), "wb");
  if (f == nullptr) {
    if (error != nullptr) *error = "cannot create '" + tmp + "'";
    return false;
  }
  const bool wrote = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
  const bool closed = std::fclose(f) == 0;
  if (!wrote || !closed || std::rename(tmp.c_str(), path.c_str()) != 0) {
    std::remove(tmp.c_str());
    if (error != nullptr) *error = "cannot write '" + path + "'";
    return false;
  }
  return true;
}

bool LoadRecording(const std::string& path, Recording* out, std::string* error) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) {
    if (error != nullptr) *error = "cannot open '" + path + "'";
    return false;
  }
  std::vector<std::uint8_t> bytes;
  std::uint8_t buf[65536];
  for (;;) {
    const std::size_t n = std::fread(buf, 1, sizeof(buf), f);
    bytes.insert(bytes.end(), buf, buf + n);
    if (n < sizeof(buf)) break;
  }
  const bool ok = std::ferror(f) == 0;
  std::fclose(f);
  if (!ok) {
    if (error != nullptr) *error = "cannot read '" + path + "'";
    return false;
  }
  return DecodeRecording(bytes.data(), bytes.size(), out, error);
}

}  // namespace snd::tool

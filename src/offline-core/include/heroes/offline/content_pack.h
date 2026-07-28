#ifndef HEROES_OFFLINE_CONTENT_PACK_H_
#define HEROES_OFFLINE_CONTENT_PACK_H_

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>

namespace heroes::offline {

// A borrowed view of bytes in a ContentPack. The descriptor remains valid until
// the pack is destroyed or move-assigned.
struct ContentLocation {
  int fd = -1;
  std::int64_t offset = 0;
  std::int64_t size = 0;
};

class ContentPack {
 public:
  explicit ContentPack(const std::filesystem::path& pack_path);
  ~ContentPack();

  ContentPack(ContentPack&&) noexcept;
  ContentPack& operator=(ContentPack&&) noexcept;

  ContentPack(const ContentPack&) = delete;
  ContentPack& operator=(const ContentPack&) = delete;

  // Returns no value when path is not present. Invalid lookup paths are also
  // treated as absent; malformed pack paths are rejected by the constructor.
  [[nodiscard]] std::optional<ContentLocation> find(
      std::string_view path) const noexcept;

  // Materializes every entry beneath output_prefix. Existing regular files are
  // retained only when their size and SHA-256 digest both match the pack.
  // Throws std::runtime_error on malformed content or filesystem/I/O failure.
  void materialize(const std::filesystem::path& output_prefix) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace heroes::offline

#endif  // HEROES_OFFLINE_CONTENT_PACK_H_

#include "heroes/offline/content_pack.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace heroes::offline {
namespace {

constexpr std::size_t kIoChunkSize = 1024 * 1024;
constexpr std::int32_t kMaximumEntryCount = 100000;
constexpr std::array<unsigned char, 8> kMagic = {
    'H', 'O', 'P', 'A', 'C', 'K', '1', '\0'};

class UniqueFd {
 public:
  UniqueFd() = default;
  explicit UniqueFd(int fd) : fd_(fd) {}
  ~UniqueFd() {
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }

  UniqueFd(UniqueFd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
  UniqueFd& operator=(UniqueFd&& other) noexcept {
    if (this != &other) {
      if (fd_ >= 0) {
        ::close(fd_);
      }
      fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
  }

  UniqueFd(const UniqueFd&) = delete;
  UniqueFd& operator=(const UniqueFd&) = delete;

  [[nodiscard]] int get() const noexcept { return fd_; }
  [[nodiscard]] int release() noexcept { return std::exchange(fd_, -1); }

 private:
  int fd_ = -1;
};

[[noreturn]] void ThrowErrno(const std::string& operation) {
  throw std::runtime_error(operation + ": " + std::strerror(errno));
}

[[noreturn]] void ThrowFilesystem(const std::string& operation,
                                  const std::error_code& error) {
  throw std::runtime_error(operation + ": " + error.message());
}

bool CheckedAdd(std::uint64_t left, std::uint64_t right,
                std::uint64_t* result) {
  if (right > std::numeric_limits<std::uint64_t>::max() - left) {
    return false;
  }
  *result = left + right;
  return true;
}

void PreadExact(int fd, std::uint64_t offset, void* output, std::size_t size,
                const char* description) {
  auto* bytes = static_cast<unsigned char*>(output);
  std::size_t done = 0;
  while (done < size) {
    if (offset > static_cast<std::uint64_t>(
                     std::numeric_limits<off_t>::max()) ||
        done > static_cast<std::size_t>(
                   std::numeric_limits<off_t>::max() -
                   static_cast<off_t>(offset))) {
      throw std::runtime_error(std::string(description) +
                               ": offset is not representable");
    }
    const ssize_t result =
        ::pread(fd, bytes + done, size - done,
                static_cast<off_t>(offset) + static_cast<off_t>(done));
    if (result == 0) {
      throw std::runtime_error(std::string(description) +
                               ": unexpected end of file");
    }
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      ThrowErrno(description);
    }
    done += static_cast<std::size_t>(result);
  }
}

template <std::size_t Size>
std::uint64_t DecodeLittleEndian(const std::array<unsigned char, Size>& bytes) {
  static_assert(Size <= sizeof(std::uint64_t));
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < Size; ++i) {
    value |= static_cast<std::uint64_t>(bytes[i]) << (i * CHAR_BIT);
  }
  return value;
}

template <std::size_t Size>
std::uint64_t ReadUnsigned(int fd, std::uint64_t* cursor,
                           std::uint64_t file_size, const char* description) {
  std::uint64_t end = 0;
  if (!CheckedAdd(*cursor, Size, &end) || end > file_size) {
    throw std::runtime_error(std::string(description) + ": truncated pack");
  }
  std::array<unsigned char, Size> bytes{};
  PreadExact(fd, *cursor, bytes.data(), bytes.size(), description);
  *cursor = end;
  return DecodeLittleEndian(bytes);
}

bool IsSafeRelativePath(std::string_view path) {
  if (path.empty() || path.front() == '/' ||
      path.find('\\') != std::string_view::npos ||
      path.find('\0') != std::string_view::npos) {
    return false;
  }

  std::size_t start = 0;
  while (start < path.size()) {
    const std::size_t slash = path.find('/', start);
    const std::size_t end =
        slash == std::string_view::npos ? path.size() : slash;
    const std::string_view component = path.substr(start, end - start);
    if (component.empty() || component == "." || component == "..") {
      return false;
    }
    if (slash == std::string_view::npos) {
      break;
    }
    start = slash + 1;
  }
  return true;
}

class Sha256 {
 public:
  void Update(const unsigned char* data, std::size_t size) {
    if (size > std::numeric_limits<std::uint64_t>::max() - total_size_) {
      throw std::runtime_error("SHA-256 input length overflow");
    }
    total_size_ += size;
    while (size != 0) {
      const std::size_t copied = std::min(size, block_.size() - block_size_);
      std::memcpy(block_.data() + block_size_, data, copied);
      block_size_ += copied;
      data += copied;
      size -= copied;
      if (block_size_ == block_.size()) {
        Transform(block_.data());
        block_size_ = 0;
      }
    }
  }

  std::array<unsigned char, 32> Finish() {
    if (total_size_ > std::numeric_limits<std::uint64_t>::max() / 8) {
      throw std::runtime_error("SHA-256 input is too large");
    }
    const std::uint64_t bit_size = total_size_ * 8;
    block_[block_size_++] = 0x80;
    if (block_size_ > 56) {
      std::fill(block_.begin() + static_cast<std::ptrdiff_t>(block_size_),
                block_.end(), 0);
      Transform(block_.data());
      block_size_ = 0;
    }
    std::fill(block_.begin() + static_cast<std::ptrdiff_t>(block_size_),
              block_.begin() + 56, 0);
    for (std::size_t i = 0; i < 8; ++i) {
      block_[63 - i] =
          static_cast<unsigned char>(bit_size >> (i * CHAR_BIT));
    }
    Transform(block_.data());

    std::array<unsigned char, 32> digest{};
    for (std::size_t i = 0; i < state_.size(); ++i) {
      for (std::size_t byte = 0; byte < 4; ++byte) {
        digest[i * 4 + byte] = static_cast<unsigned char>(
            state_[i] >> ((3 - byte) * CHAR_BIT));
      }
    }
    return digest;
  }

 private:
  static constexpr std::array<std::uint32_t, 64> kConstants = {
      0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
      0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
      0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
      0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
      0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
      0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
      0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
      0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
      0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
      0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
      0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
      0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
      0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
      0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
      0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
      0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

  static std::uint32_t RotateRight(std::uint32_t value, unsigned count) {
    return (value >> count) | (value << (32 - count));
  }

  void Transform(const unsigned char* block) {
    std::array<std::uint32_t, 64> words{};
    for (std::size_t i = 0; i < 16; ++i) {
      words[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) |
                 (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
                 (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
                 static_cast<std::uint32_t>(block[i * 4 + 3]);
    }
    for (std::size_t i = 16; i < words.size(); ++i) {
      const std::uint32_t s0 = RotateRight(words[i - 15], 7) ^
                               RotateRight(words[i - 15], 18) ^
                               (words[i - 15] >> 3);
      const std::uint32_t s1 = RotateRight(words[i - 2], 17) ^
                               RotateRight(words[i - 2], 19) ^
                               (words[i - 2] >> 10);
      words[i] = words[i - 16] + s0 + words[i - 7] + s1;
    }

    auto working = state_;
    for (std::size_t i = 0; i < words.size(); ++i) {
      const std::uint32_t sigma1 = RotateRight(working[4], 6) ^
                                   RotateRight(working[4], 11) ^
                                   RotateRight(working[4], 25);
      const std::uint32_t choose =
          (working[4] & working[5]) ^ (~working[4] & working[6]);
      const std::uint32_t temp1 =
          working[7] + sigma1 + choose + kConstants[i] + words[i];
      const std::uint32_t sigma0 = RotateRight(working[0], 2) ^
                                   RotateRight(working[0], 13) ^
                                   RotateRight(working[0], 22);
      const std::uint32_t majority =
          (working[0] & working[1]) ^ (working[0] & working[2]) ^
          (working[1] & working[2]);
      const std::uint32_t temp2 = sigma0 + majority;
      for (std::size_t j = 7; j > 0; --j) {
        working[j] = working[j - 1];
      }
      working[4] += temp1;
      working[0] = temp1 + temp2;
    }
    for (std::size_t i = 0; i < state_.size(); ++i) {
      state_[i] += working[i];
    }
  }

  std::array<std::uint32_t, 8> state_ = {
      0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
      0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
  std::array<unsigned char, 64> block_{};
  std::size_t block_size_ = 0;
  std::uint64_t total_size_ = 0;
};

struct ParsedEntry {
  std::string path;
  std::int64_t offset;
  std::int64_t size;
  std::array<unsigned char, 32> digest;
};

bool PathIsWithin(const std::filesystem::path& root,
                  const std::filesystem::path& candidate) {
  auto root_part = root.begin();
  auto candidate_part = candidate.begin();
  for (; root_part != root.end(); ++root_part, ++candidate_part) {
    if (candidate_part == candidate.end() || *root_part != *candidate_part) {
      return false;
    }
  }
  return true;
}

std::filesystem::file_status SymlinkStatusAllowMissing(
    const std::filesystem::path& path) {
  std::error_code error;
  auto status = std::filesystem::symlink_status(path, error);
  if (error == std::errc::no_such_file_or_directory) {
    return std::filesystem::file_status(
        std::filesystem::file_type::not_found);
  }
  if (error) {
    ThrowFilesystem("inspect filesystem path", error);
  }
  return status;
}

std::filesystem::path EnsureContainedParent(
    const std::filesystem::path& root, const std::filesystem::path& relative) {
  std::filesystem::path current = root;
  for (const auto& component : relative.parent_path()) {
    current /= component;
    std::error_code error;
    const auto status = SymlinkStatusAllowMissing(current);
    if (status.type() == std::filesystem::file_type::not_found) {
      if (!std::filesystem::create_directory(current, error) && error) {
        ThrowFilesystem("create materialization directory", error);
      }
    } else if (status.type() != std::filesystem::file_type::directory &&
               status.type() != std::filesystem::file_type::symlink) {
      throw std::runtime_error(
          "materialization path contains a non-directory component");
    }

    const auto canonical = std::filesystem::canonical(current, error);
    if (error) {
      ThrowFilesystem("canonicalize materialization directory", error);
    }
    if (!PathIsWithin(root, canonical)) {
      throw std::runtime_error(
          "materialization path escapes the output prefix");
    }
    current = canonical;
  }
  return current;
}

std::array<unsigned char, 32> HashFile(int fd, std::uint64_t size,
                                       std::uint64_t offset) {
  Sha256 hash;
  std::vector<unsigned char> buffer(kIoChunkSize);
  std::uint64_t consumed = 0;
  while (consumed < size) {
    const std::size_t chunk = static_cast<std::size_t>(
        std::min<std::uint64_t>(buffer.size(), size - consumed));
    PreadExact(fd, offset + consumed, buffer.data(), chunk, "hash content");
    hash.Update(buffer.data(), chunk);
    consumed += chunk;
  }
  return hash.Finish();
}

void WriteAll(int fd, const unsigned char* data, std::size_t size) {
  std::size_t written = 0;
  while (written < size) {
    const ssize_t result = ::write(fd, data + written, size - written);
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      ThrowErrno("write pending content");
    }
    if (result == 0) {
      throw std::runtime_error("write pending content: no progress");
    }
    written += static_cast<std::size_t>(result);
  }
}

class PendingFile {
 public:
  PendingFile(std::filesystem::path path, UniqueFd fd)
      : path_(std::move(path)), fd_(std::move(fd)) {}
  ~PendingFile() {
    if (!committed_) {
      std::error_code ignored;
      std::filesystem::remove(path_, ignored);
    }
  }

  int fd() const { return fd_.get(); }
  void Close() { fd_ = UniqueFd(); }
  void RenameTo(const std::filesystem::path& target) {
    if (::rename(path_.c_str(), target.c_str()) != 0) {
      ThrowErrno("rename materialized content");
    }
    committed_ = true;
  }

 private:
  std::filesystem::path path_;
  UniqueFd fd_;
  bool committed_ = false;
};

PendingFile CreatePendingFile(const std::filesystem::path& target) {
  static std::atomic<std::uint64_t> sequence{0};
  for (unsigned attempt = 0; attempt < 1000; ++attempt) {
    const auto pending =
        target.string() + ".pending." + std::to_string(::getpid()) + "." +
        std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
    const int fd = ::open(pending.c_str(),
                          O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                          0600);
    if (fd >= 0) {
      return PendingFile(pending, UniqueFd(fd));
    }
    if (errno != EEXIST) {
      ThrowErrno("create pending content file");
    }
  }
  throw std::runtime_error("unable to choose a unique pending file name");
}

void FsyncDirectory(const std::filesystem::path& directory) {
  UniqueFd fd(::open(directory.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY));
  if (fd.get() < 0) {
    ThrowErrno("open materialization directory");
  }
  if (::fsync(fd.get()) != 0) {
    ThrowErrno("sync materialization directory");
  }
}

}  // namespace

struct ContentPack::Impl {
  UniqueFd fd;
  std::vector<ParsedEntry> entries;
  std::unordered_map<std::string, std::size_t> by_path;
};

ContentPack::ContentPack(const std::filesystem::path& pack_path)
    : impl_(std::make_unique<Impl>()) {
  UniqueFd fd(::open(pack_path.c_str(), O_RDONLY | O_CLOEXEC));
  if (fd.get() < 0) {
    ThrowErrno("open content pack");
  }

  struct stat file_info {};
  if (::fstat(fd.get(), &file_info) != 0) {
    ThrowErrno("stat content pack");
  }
  if (!S_ISREG(file_info.st_mode) || file_info.st_size < 0) {
    throw std::runtime_error("content pack is not a regular file");
  }
  const auto file_size = static_cast<std::uint64_t>(file_info.st_size);
  if (file_size < kMagic.size() + 4) {
    throw std::runtime_error("content pack header is truncated");
  }

  std::array<unsigned char, 8> magic{};
  PreadExact(fd.get(), 0, magic.data(), magic.size(), "read content pack magic");
  if (magic != kMagic) {
    throw std::runtime_error("invalid content pack magic");
  }

  std::uint64_t cursor = kMagic.size();
  const std::uint32_t raw_count =
      static_cast<std::uint32_t>(ReadUnsigned<4>(
          fd.get(), &cursor, file_size, "read content pack entry count"));
  if ((raw_count & 0x80000000U) != 0 ||
      raw_count > static_cast<std::uint32_t>(kMaximumEntryCount)) {
    throw std::runtime_error("content pack entry count is out of range");
  }
  const auto count = static_cast<std::int32_t>(raw_count);

  impl_->entries.reserve(static_cast<std::size_t>(count));
  impl_->by_path.reserve(static_cast<std::size_t>(count));
  for (std::int32_t i = 0; i < count; ++i) {
    const std::uint64_t path_size = ReadUnsigned<2>(
        fd.get(), &cursor, file_size, "read content pack path length");
    std::uint64_t path_end = 0;
    if (!CheckedAdd(cursor, path_size, &path_end) || path_end > file_size) {
      throw std::runtime_error("content pack path is truncated");
    }
    std::string path(static_cast<std::size_t>(path_size), '\0');
    if (!path.empty()) {
      PreadExact(fd.get(), cursor, path.data(), path.size(),
                 "read content pack path");
    }
    cursor = path_end;
    if (!IsSafeRelativePath(path)) {
      throw std::runtime_error("content pack contains an unsafe path");
    }

    const std::uint64_t raw_offset = ReadUnsigned<8>(
        fd.get(), &cursor, file_size, "read content pack data offset");
    const std::uint64_t raw_size = ReadUnsigned<8>(
        fd.get(), &cursor, file_size, "read content pack data size");
    if (raw_offset >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
        raw_size >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      throw std::runtime_error("content pack data range is negative");
    }

    ParsedEntry entry{std::move(path), static_cast<std::int64_t>(raw_offset),
                      static_cast<std::int64_t>(raw_size), {}};
    std::uint64_t digest_end = 0;
    if (!CheckedAdd(cursor, entry.digest.size(), &digest_end) ||
        digest_end > file_size) {
      throw std::runtime_error("content pack digest is truncated");
    }
    PreadExact(fd.get(), cursor, entry.digest.data(), entry.digest.size(),
               "read content pack digest");
    cursor = digest_end;

    const std::size_t index = impl_->entries.size();
    if (!impl_->by_path.emplace(entry.path, index).second) {
      throw std::runtime_error("content pack contains a duplicate path");
    }
    impl_->entries.push_back(std::move(entry));
  }

  struct Range {
    std::uint64_t begin;
    std::uint64_t end;
  };
  std::vector<Range> ranges;
  ranges.reserve(impl_->entries.size());
  for (const auto& entry : impl_->entries) {
    const auto begin = static_cast<std::uint64_t>(entry.offset);
    const auto size = static_cast<std::uint64_t>(entry.size);
    std::uint64_t end = 0;
    if (!CheckedAdd(begin, size, &end) || begin < cursor || end > file_size) {
      throw std::runtime_error("content pack data range is out of bounds");
    }
    if (size != 0) {
      ranges.push_back({begin, end});
    }
  }
  std::sort(ranges.begin(), ranges.end(),
            [](const Range& left, const Range& right) {
              return left.begin < right.begin;
            });
  for (std::size_t i = 1; i < ranges.size(); ++i) {
    if (ranges[i].begin < ranges[i - 1].end) {
      throw std::runtime_error("content pack data ranges overlap");
    }
  }

  impl_->fd = std::move(fd);
}

ContentPack::~ContentPack() = default;
ContentPack::ContentPack(ContentPack&&) noexcept = default;
ContentPack& ContentPack::operator=(ContentPack&&) noexcept = default;

std::optional<ContentLocation> ContentPack::find(
    std::string_view path) const noexcept {
  if (!impl_ || !IsSafeRelativePath(path)) {
    return std::nullopt;
  }
  const auto found = impl_->by_path.find(std::string(path));
  if (found == impl_->by_path.end()) {
    return std::nullopt;
  }
  const auto& entry = impl_->entries[found->second];
  return ContentLocation{impl_->fd.get(), entry.offset, entry.size};
}

void ContentPack::materialize(
    const std::filesystem::path& output_prefix) const {
  if (!impl_) {
    throw std::runtime_error("cannot materialize a moved-from content pack");
  }

  std::error_code error;
  if (!std::filesystem::create_directories(output_prefix, error) && error) {
    ThrowFilesystem("create output prefix", error);
  }
  const auto root = std::filesystem::canonical(output_prefix, error);
  if (error) {
    ThrowFilesystem("canonicalize output prefix", error);
  }
  if (!std::filesystem::is_directory(root, error)) {
    if (error) {
      ThrowFilesystem("inspect output prefix", error);
    }
    throw std::runtime_error("output prefix is not a directory");
  }

  std::vector<unsigned char> buffer(kIoChunkSize);
  for (const auto& entry : impl_->entries) {
    const std::filesystem::path relative(entry.path);
    const auto parent = EnsureContainedParent(root, relative);
    const auto target = parent / relative.filename();
    if (!PathIsWithin(root, target.lexically_normal())) {
      throw std::runtime_error("materialization target escapes output prefix");
    }

    const auto target_status = SymlinkStatusAllowMissing(target);
    if (target_status.type() == std::filesystem::file_type::symlink) {
      throw std::runtime_error("materialization target is a symbolic link");
    }

    bool matches = false;
    if (target_status.type() != std::filesystem::file_type::not_found) {
      UniqueFd existing(
          ::open(target.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
      if (existing.get() < 0) {
        ThrowErrno("open existing materialized file");
      }
      struct stat info {};
      if (::fstat(existing.get(), &info) != 0) {
        ThrowErrno("stat existing materialized file");
      }
      if (!S_ISREG(info.st_mode)) {
        throw std::runtime_error(
            "materialization target exists and is not a regular file");
      }
      if (info.st_size == entry.size) {
        matches =
            HashFile(existing.get(), static_cast<std::uint64_t>(entry.size), 0) ==
            entry.digest;
      }
    }
    if (matches) {
      continue;
    }

    auto pending = CreatePendingFile(target);
    Sha256 hash;
    std::uint64_t copied = 0;
    while (copied < static_cast<std::uint64_t>(entry.size)) {
      const std::size_t chunk = static_cast<std::size_t>(
          std::min<std::uint64_t>(
              buffer.size(), static_cast<std::uint64_t>(entry.size) - copied));
      PreadExact(impl_->fd.get(),
                 static_cast<std::uint64_t>(entry.offset) + copied,
                 buffer.data(), chunk, "read packed content");
      hash.Update(buffer.data(), chunk);
      WriteAll(pending.fd(), buffer.data(), chunk);
      copied += chunk;
    }
    if (hash.Finish() != entry.digest) {
      throw std::runtime_error("content pack SHA-256 digest mismatch for " +
                               entry.path);
    }
    if (::fsync(pending.fd()) != 0) {
      ThrowErrno("sync pending content file");
    }
    pending.Close();
    pending.RenameTo(target);
    FsyncDirectory(parent);
  }
}

}  // namespace heroes::offline

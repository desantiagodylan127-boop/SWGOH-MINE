#include "heroes/offline/content_pack.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <unistd.h>

namespace {

using heroes::offline::ContentPack;

struct FixtureEntry {
  std::string path;
  std::string data;
  std::string digest_hex;
};

std::array<unsigned char, 32> HexDigest(std::string_view hex) {
  if (hex.size() != 64) {
    throw std::runtime_error("bad test digest");
  }
  auto digit = [](char value) -> unsigned char {
    if (value >= '0' && value <= '9') {
      return static_cast<unsigned char>(value - '0');
    }
    if (value >= 'a' && value <= 'f') {
      return static_cast<unsigned char>(value - 'a' + 10);
    }
    throw std::runtime_error("bad test digest digit");
  };
  std::array<unsigned char, 32> result{};
  for (std::size_t i = 0; i < result.size(); ++i) {
    result[i] =
        static_cast<unsigned char>((digit(hex[i * 2]) << 4) | digit(hex[i * 2 + 1]));
  }
  return result;
}

void AppendLittle(std::vector<unsigned char>* bytes, std::uint64_t value,
                  std::size_t width) {
  for (std::size_t i = 0; i < width; ++i) {
    bytes->push_back(static_cast<unsigned char>(value >> (i * 8)));
  }
}

void SetLittle(std::vector<unsigned char>* bytes, std::size_t position,
               std::uint64_t value, std::size_t width) {
  for (std::size_t i = 0; i < width; ++i) {
    bytes->at(position + i) =
        static_cast<unsigned char>(value >> (i * 8));
  }
}

std::vector<unsigned char> MakePack(const std::vector<FixtureEntry>& entries) {
  std::uint64_t metadata_size = 12;
  for (const auto& entry : entries) {
    metadata_size += 2 + entry.path.size() + 8 + 8 + 32;
  }

  std::vector<unsigned char> bytes = {'H', 'O', 'P', 'A',
                                      'C', 'K', '1', '\0'};
  AppendLittle(&bytes, entries.size(), 4);
  std::uint64_t data_offset = metadata_size;
  for (const auto& entry : entries) {
    AppendLittle(&bytes, entry.path.size(), 2);
    bytes.insert(bytes.end(), entry.path.begin(), entry.path.end());
    AppendLittle(&bytes, data_offset, 8);
    AppendLittle(&bytes, entry.data.size(), 8);
    const auto digest = HexDigest(entry.digest_hex);
    bytes.insert(bytes.end(), digest.begin(), digest.end());
    data_offset += entry.data.size();
  }
  for (const auto& entry : entries) {
    bytes.insert(bytes.end(), entry.data.begin(), entry.data.end());
  }
  return bytes;
}

class TempDirectory {
 public:
  TempDirectory() {
    std::array<char, 64> pattern{};
    const std::string value = "/tmp/heroes-content-pack-test-XXXXXX";
    std::copy(value.begin(), value.end(), pattern.begin());
    if (::mkdtemp(pattern.data()) == nullptr) {
      throw std::runtime_error("mkdtemp failed");
    }
    path_ = pattern.data();
  }
  ~TempDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

void WriteBytes(const std::filesystem::path& path,
                const std::vector<unsigned char>& bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  if (!output) {
    throw std::runtime_error("write test fixture failed");
  }
}

std::string ReadText(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

void Require(bool condition) {
  if (!condition) {
    throw std::runtime_error("test requirement failed");
  }
}

template <typename Function>
void RequireThrows(Function&& function) {
  bool threw = false;
  try {
    std::forward<Function>(function)();
  } catch (const std::exception&) {
    threw = true;
  }
  Require(threw);
}

constexpr std::string_view kHelloDigest =
    "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824";
constexpr std::string_view kWorldDigest =
    "711e9609339e92b03ddc0a211827dba421f38f9ed8b9d806e1ffdd8c15ffa03d";

void ValidPackTest() {
  TempDirectory temporary;
  const auto pack_path = temporary.path() / "valid.hopack";
  WriteBytes(pack_path,
             MakePack({{"text/hello.txt", "hello", std::string(kHelloDigest)},
                       {"world.bin", "world!", std::string(kWorldDigest)}}));

  ContentPack pack(pack_path);
  const auto hello = pack.find("text/hello.txt");
  Require(hello.has_value());
  Require(hello->fd >= 0 && hello->size == 5);
  std::array<char, 5> content{};
  Require(::pread(hello->fd, content.data(), content.size(), hello->offset) ==
          static_cast<ssize_t>(content.size()));
  Require(std::string(content.data(), content.size()) == "hello");
  Require(!pack.find("missing").has_value());
  Require(!pack.find("../bad").has_value());

  const auto output = temporary.path() / "output";
  std::filesystem::create_directories(output / "text");
  {
    std::ofstream wrong(output / "text/hello.txt", std::ios::binary);
    wrong << "HELLO";  // Same size: materialize must hash, not merely stat.
  }
  pack.materialize(output);
  Require(ReadText(output / "text/hello.txt") == "hello");
  Require(ReadText(output / "world.bin") == "world!");
  pack.materialize(output);
}

void BadMagicTest() {
  TempDirectory temporary;
  auto bytes = MakePack({});
  bytes[0] = 'X';
  const auto path = temporary.path() / "bad-magic";
  WriteBytes(path, bytes);
  RequireThrows([&] { ContentPack pack(path); });
}

void BadCountTest() {
  TempDirectory temporary;
  auto bytes = MakePack({});
  SetLittle(&bytes, 8, 100001, 4);
  const auto too_many = temporary.path() / "too-many";
  WriteBytes(too_many, bytes);
  RequireThrows([&] { ContentPack pack(too_many); });

  SetLittle(&bytes, 8, 0xffffffffU, 4);
  const auto negative = temporary.path() / "negative";
  WriteBytes(negative, bytes);
  RequireThrows([&] { ContentPack pack(negative); });
}

void TruncationTest() {
  TempDirectory temporary;
  auto bytes =
      MakePack({{"hello", "hello", std::string(kHelloDigest)}});
  bytes.resize(20);
  const auto path = temporary.path() / "truncated";
  WriteBytes(path, bytes);
  RequireThrows([&] { ContentPack pack(path); });
}

void RangeTest() {
  TempDirectory temporary;
  auto bytes =
      MakePack({{"hello", "hello", std::string(kHelloDigest)}});
  const std::size_t offset_field = 12 + 2 + std::string("hello").size();
  SetLittle(&bytes, offset_field, bytes.size() + 1, 8);
  const auto path = temporary.path() / "bad-range";
  WriteBytes(path, bytes);
  RequireThrows([&] { ContentPack pack(path); });
}

void UnsafeAndDuplicatePathTest() {
  TempDirectory temporary;
  const auto traversal = temporary.path() / "traversal";
  WriteBytes(traversal,
             MakePack({{"../outside", "hello", std::string(kHelloDigest)}}));
  RequireThrows([&] { ContentPack pack(traversal); });

  const auto absolute = temporary.path() / "absolute";
  WriteBytes(absolute,
             MakePack({{"/outside", "hello", std::string(kHelloDigest)}}));
  RequireThrows([&] { ContentPack pack(absolute); });

  const auto backslash = temporary.path() / "backslash";
  WriteBytes(backslash,
             MakePack({{"bad\\path", "hello", std::string(kHelloDigest)}}));
  RequireThrows([&] { ContentPack pack(backslash); });

  const auto duplicate = temporary.path() / "duplicate";
  WriteBytes(duplicate,
             MakePack({{"same", "hello", std::string(kHelloDigest)},
                       {"same", "world!", std::string(kWorldDigest)}}));
  RequireThrows([&] { ContentPack pack(duplicate); });
}

void CorruptHashTest() {
  TempDirectory temporary;
  auto bytes =
      MakePack({{"hello", "hello", std::string(kHelloDigest)}});
  bytes.back() ^= 1;
  const auto path = temporary.path() / "corrupt";
  WriteBytes(path, bytes);
  ContentPack pack(path);
  RequireThrows([&] { pack.materialize(temporary.path() / "output"); });
  Require(!std::filesystem::exists(temporary.path() / "output/hello"));
}

}  // namespace

int run_content_pack_tests() {
  const std::array<std::pair<const char*, std::function<void()>>, 7> tests = {{
      {"valid pack", ValidPackTest},
      {"bad magic", BadMagicTest},
      {"bad count", BadCountTest},
      {"truncation", TruncationTest},
      {"range", RangeTest},
      {"unsafe and duplicate paths", UnsafeAndDuplicatePathTest},
      {"corrupt hash", CorruptHashTest},
  }};
  int failures = 0;
  for (const auto& [name, test] : tests) {
    try {
      test();
    } catch (const std::exception& error) {
      std::cerr << "content_pack_test: " << name << ": " << error.what()
                << '\n';
      ++failures;
    }
  }
  return failures;
}

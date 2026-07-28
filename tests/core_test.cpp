#include "heroes/offline/offline_core.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

namespace {

#define CHECK(condition)        \
  do {                          \
    if (!(condition)) {         \
      return __LINE__;          \
    }                           \
  } while (false)

class TempDirectory {
 public:
  TempDirectory() {
    std::array<char, 64> pattern{};
    const std::string source = "/tmp/heroes-offline-core-test-XXXXXX";
    std::copy(source.begin(), source.end(), pattern.begin());
    const char* const created = ::mkdtemp(pattern.data());
    if (created != nullptr) {
      path_ = created;
    }
  }

  ~TempDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  [[nodiscard]] const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

void append_little(std::vector<std::uint8_t>& output, std::uint64_t value,
                   std::size_t width) {
  for (std::size_t index = 0; index < width; ++index) {
    output.push_back(static_cast<std::uint8_t>(value >> (index * 8U)));
  }
}

std::vector<std::uint8_t> make_content_pack() {
  constexpr std::array<std::uint8_t, 32> digest{
      0x2c, 0xf2, 0x4d, 0xba, 0x5f, 0xb0, 0xa3, 0x0e, 0x26, 0xe8, 0x3b,
      0x2a, 0xc5, 0xb9, 0xe2, 0x9e, 0x1b, 0x16, 0x1e, 0x5c, 0x1f, 0xa7,
      0x42, 0x5e, 0x73, 0x04, 0x33, 0x62, 0x93, 0x8b, 0x98, 0x24};
  const std::string path = "text/hello.txt";
  const std::string payload = "hello";
  const std::uint64_t offset = 12U + 2U + path.size() + 8U + 8U +
                               digest.size();

  std::vector<std::uint8_t> output{'H', 'O', 'P', 'A', 'C', 'K', '1', 0};
  append_little(output, 1, 4);
  append_little(output, path.size(), 2);
  output.insert(output.end(), path.begin(), path.end());
  append_little(output, offset, 8);
  append_little(output, payload.size(), 8);
  output.insert(output.end(), digest.begin(), digest.end());
  output.insert(output.end(), payload.begin(), payload.end());
  return output;
}

bool write_bytes(const std::filesystem::path& path,
                 const std::vector<std::uint8_t>& bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  return static_cast<bool>(output);
}

}  // namespace

int run_core_tests() {
  CHECK(ho_shutdown() == HO_STATUS_OK);

  ho_owned_bytes response{reinterpret_cast<std::uint8_t*>(1), 1};
  CHECK(ho_dispatch_rpc("AuthRpc", "DoAuthGuest", nullptr, 0, &response) ==
        HO_STATUS_NOT_INITIALIZED);
  CHECK(response.data == nullptr && response.size == 0);
  CHECK(std::string(ho_last_error()).find("not initialized") !=
        std::string::npos);

  TempDirectory temporary;
  CHECK(!temporary.path().empty());
  const auto pack_path = temporary.path() / "content.hopack";
  CHECK(write_bytes(pack_path, make_content_pack()));
  const std::string storage = (temporary.path() / "state").string();
  const std::string pack = pack_path.string();
  const ho_init_options options{storage.c_str(),
                                nullptr,
                                pack.c_str(),
                                nullptr,
                                "test-content",
                                1'700'000'000,
                                0x1234U};

  CHECK(ho_initialize(&options) == HO_STATUS_OK);
  CHECK(ho_initialize(&options) == HO_STATUS_OK);
  CHECK(std::string(ho_last_error()).empty());

  CHECK(ho_dispatch_rpc("AuthRpc", "DoAuthGuest", nullptr, 0, &response) ==
        HO_STATUS_OK);
  CHECK(response.data != nullptr && response.size != 0);
  ho_free(response.data);

  CHECK(ho_dispatch_rpc("UnknownRpc", "Missing", nullptr, 0, &response) ==
        HO_STATUS_UNSUPPORTED_RPC);
  CHECK(response.data == nullptr && response.size == 0);
  const std::string calling_thread_error = ho_last_error();
  CHECK(!calling_thread_error.empty());

  std::string other_thread_error;
  ho_status other_thread_status = HO_STATUS_OK;
  std::thread worker([&] {
    ho_owned_bytes ignored{};
    other_thread_status =
        ho_dispatch_rpc(nullptr, "Missing", nullptr, 0, &ignored);
    other_thread_error = ho_last_error();
  });
  worker.join();
  CHECK(other_thread_status == HO_STATUS_INVALID_ARGUMENT);
  CHECK(!other_thread_error.empty());
  CHECK(std::string(ho_last_error()) == calling_thread_error);

  ho_content_location location{};
  CHECK(ho_find_content("text/hello.txt", &location) == HO_STATUS_OK);
  CHECK(location.fd >= 0 && location.size == 5);
  std::array<char, 5> content{};
  CHECK(::pread(location.fd, content.data(), content.size(), location.offset) ==
        static_cast<ssize_t>(content.size()));
  CHECK(std::string(content.data(), content.size()) == "hello");
  CHECK(ho_find_content("missing", &location) == HO_STATUS_INVALID_ARGUMENT);
  CHECK(location.fd == -1 && location.offset == 0 && location.size == 0);

  CHECK(ho_shutdown() == HO_STATUS_OK);
  CHECK(ho_shutdown() == HO_STATUS_OK);
  CHECK(ho_find_content("text/hello.txt", &location) ==
        HO_STATUS_NOT_INITIALIZED);
  return 0;
}

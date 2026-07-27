#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace heroes::offline::protobuf {

using Bytes = std::vector<std::uint8_t>;

enum class WireType : std::uint32_t {
  varint = 0,
  fixed64 = 1,
  length_delimited = 2,
  fixed32 = 5,
};

void write_varint(Bytes& output, std::uint64_t value);
void write_key(Bytes& output, std::uint32_t field, WireType wire_type);
void write_integer(Bytes& output, std::uint32_t field, std::uint64_t value);
void write_string(Bytes& output, std::uint32_t field, std::string_view value);
void write_bytes(Bytes& output, std::uint32_t field,
                 std::span<const std::uint8_t> value);
void write_message(Bytes& output, std::uint32_t field,
                   std::span<const std::uint8_t> value);

class Writer {
 public:
  Writer() = default;
  explicit Writer(Bytes bytes) : bytes_(std::move(bytes)) {}

  Writer& varint(std::uint64_t value);
  Writer& key(std::uint32_t field, WireType wire_type);
  Writer& integer(std::uint32_t field, std::uint64_t value);
  Writer& string(std::uint32_t field, std::string_view value);
  Writer& bytes(std::uint32_t field, std::span<const std::uint8_t> value);
  Writer& message(std::uint32_t field, std::span<const std::uint8_t> value);

  [[nodiscard]] const Bytes& data() const noexcept { return bytes_; }
  [[nodiscard]] Bytes take() && noexcept { return std::move(bytes_); }

 private:
  Bytes bytes_;
};

}  // namespace heroes::offline::protobuf

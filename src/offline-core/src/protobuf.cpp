#include "heroes/offline/protobuf.h"

#include <stdexcept>
#include <utility>

namespace heroes::offline::protobuf {
namespace {

constexpr std::uint32_t kMaximumFieldNumber = (1U << 29U) - 1U;

void validate_field(std::uint32_t field) {
  if (field == 0 || field > kMaximumFieldNumber) {
    throw std::invalid_argument("invalid protobuf field number");
  }
}

}  // namespace

void write_varint(Bytes& output, std::uint64_t value) {
  while (value >= 0x80U) {
    output.push_back(static_cast<std::uint8_t>((value & 0x7fU) | 0x80U));
    value >>= 7U;
  }
  output.push_back(static_cast<std::uint8_t>(value));
}

void write_key(Bytes& output, std::uint32_t field, WireType wire_type) {
  validate_field(field);
  const auto wire = static_cast<std::uint32_t>(wire_type);
  if (wire != 0 && wire != 1 && wire != 2 && wire != 5) {
    throw std::invalid_argument("invalid protobuf wire type");
  }
  write_varint(output, (static_cast<std::uint64_t>(field) << 3U) | wire);
}

void write_integer(Bytes& output, std::uint32_t field, std::uint64_t value) {
  write_key(output, field, WireType::varint);
  write_varint(output, value);
}

void write_string(Bytes& output, std::uint32_t field,
                  std::string_view value) {
  write_key(output, field, WireType::length_delimited);
  write_varint(output, value.size());
  output.insert(output.end(), value.begin(), value.end());
}

void write_bytes(Bytes& output, std::uint32_t field,
                 std::span<const std::uint8_t> value) {
  write_key(output, field, WireType::length_delimited);
  write_varint(output, value.size());
  output.insert(output.end(), value.begin(), value.end());
}

void write_message(Bytes& output, std::uint32_t field,
                   std::span<const std::uint8_t> value) {
  write_bytes(output, field, value);
}

Writer& Writer::varint(std::uint64_t value) {
  write_varint(bytes_, value);
  return *this;
}

Writer& Writer::key(std::uint32_t field, WireType wire_type) {
  write_key(bytes_, field, wire_type);
  return *this;
}

Writer& Writer::integer(std::uint32_t field, std::uint64_t value) {
  write_integer(bytes_, field, value);
  return *this;
}

Writer& Writer::string(std::uint32_t field, std::string_view value) {
  write_string(bytes_, field, value);
  return *this;
}

Writer& Writer::bytes(std::uint32_t field,
                      std::span<const std::uint8_t> value) {
  write_bytes(bytes_, field, value);
  return *this;
}

Writer& Writer::message(std::uint32_t field,
                        std::span<const std::uint8_t> value) {
  write_message(bytes_, field, value);
  return *this;
}

}  // namespace heroes::offline::protobuf

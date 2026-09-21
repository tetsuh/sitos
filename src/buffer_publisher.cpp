// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include "sitos/buffer_publisher.hpp"

#include <cctype>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "fence_internal.hpp"
#include "sitos/ack.hpp"
#include "sitos/param_value.hpp"

namespace sitos {

struct BufferPublisher::Impl {
  std::shared_ptr<Transport> transport;
  std::unique_ptr<fence_internal::FencePublisher> publisher;
  std::string prefix;
  std::string sid;
  BufferClass buffer_class;
};

namespace {

class SessionMetadataParser {
 public:
  explicit SessionMetadataParser(std::string_view text) : text_(text) {}

  bool Parse(FenceUuid* generation) {
    if (text_.size() > 16384) return false;
    SkipWhitespace();
    if (!Consume('{')) return false;
    bool state_seen = false;
    bool created_seen = false;
    bool generation_seen = false;
    std::string state;
    std::string created_at;
    std::string generation_text;
    SkipWhitespace();
    if (Consume('}')) return false;
    for (;;) {
      std::string key;
      if (!ParseString(&key)) return false;
      SkipWhitespace();
      if (!Consume(':')) return false;
      SkipWhitespace();
      if (key == "state" || key == "created_at" || key == "generation_uuid") {
        bool* seen = key == "state"        ? &state_seen
                     : key == "created_at" ? &created_seen
                                           : &generation_seen;
        if (*seen || !ParseString(key == "state"        ? &state
                                  : key == "created_at" ? &created_at
                                                        : &generation_text)) {
          return false;
        }
        *seen = true;
      } else if (!SkipValue(0)) {
        return false;
      }
      SkipWhitespace();
      if (Consume('}')) break;
      if (!Consume(',')) return false;
      SkipWhitespace();
    }
    SkipWhitespace();
    if (position_ != text_.size() || !state_seen || !created_seen || !generation_seen ||
        state != "active") {
      return false;
    }
    const auto parsed = fence_internal::ParseFenceUuid(generation_text);
    if (!parsed.has_value() || fence_internal::FormatFenceUuid(*parsed) != generation_text) {
      return false;
    }
    *generation = *parsed;
    return true;
  }

 private:
  static int HexValue(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
  }

  bool ReadHex4(std::uint16_t* value) {
    if (position_ + 4 > text_.size()) return false;
    std::uint16_t result = 0;
    for (std::size_t index = 0; index != 4; ++index) {
      const auto digit = HexValue(text_[position_ + index]);
      if (digit < 0) return false;
      result = static_cast<std::uint16_t>((result << 4) | digit);
    }
    position_ += 4;
    *value = result;
    return true;
  }

  static void AppendCodePoint(std::string* output, std::uint32_t code_point) {
    if (code_point <= 0x7F) {
      output->push_back(static_cast<char>(code_point));
    } else if (code_point <= 0x7FF) {
      output->push_back(static_cast<char>(0xC0 | (code_point >> 6)));
      output->push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else if (code_point <= 0xFFFF) {
      output->push_back(static_cast<char>(0xE0 | (code_point >> 12)));
      output->push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
      output->push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else {
      output->push_back(static_cast<char>(0xF0 | (code_point >> 18)));
      output->push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
      output->push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
      output->push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    }
  }

  bool ParseUnicodeEscape(std::string* output) {
    std::uint16_t first = 0;
    if (!ReadHex4(&first)) return false;
    if (first >= 0xDC00 && first <= 0xDFFF) return false;
    std::uint32_t code_point = first;
    if (first >= 0xD800 && first <= 0xDBFF) {
      if (position_ + 2 > text_.size() || text_[position_] != '\\' || text_[position_ + 1] != 'u')
        return false;
      position_ += 2;
      std::uint16_t second = 0;
      if (!ReadHex4(&second) || second < 0xDC00 || second > 0xDFFF) return false;
      code_point = 0x10000 + ((static_cast<std::uint32_t>(first) - 0xD800) << 10) +
                   (static_cast<std::uint32_t>(second) - 0xDC00);
    }
    AppendCodePoint(output, code_point);
    return true;
  }

  void SkipWhitespace() {
    while (position_ < text_.size()) {
      const auto value = static_cast<unsigned char>(text_[position_]);
      if (value != ' ' && value != '\t' && value != '\n' && value != '\r') break;
      ++position_;
    }
  }

  bool Consume(char expected) {
    if (position_ >= text_.size() || text_[position_] != expected) return false;
    ++position_;
    return true;
  }

  bool ParseString(std::string* output) {
    if (!Consume('"')) return false;
    output->clear();
    while (position_ < text_.size()) {
      const auto value = static_cast<unsigned char>(text_[position_++]);
      if (value == '"') return true;
      if (value < 0x20) return false;
      if (value != '\\') {
        if (value < 0x80) {
          output->push_back(static_cast<char>(value));
          continue;
        }
        const int width = value >= 0xF0 ? 4 : value >= 0xE0 ? 3 : value >= 0xC2 ? 2 : 0;
        if (width == 0 || position_ + static_cast<std::size_t>(width - 1) > text_.size()) {
          return false;
        }
        std::uint32_t code_point = value & ((1u << (7 - width)) - 1u);
        for (int index = 1; index < width; ++index) {
          const auto continuation = static_cast<unsigned char>(text_[position_++]);
          if ((continuation & 0xC0) != 0x80) return false;
          code_point = (code_point << 6) | (continuation & 0x3F);
        }
        const auto minimum = width == 2 ? 0x80u : width == 3 ? 0x800u : 0x10000u;
        if (code_point < minimum || code_point > 0x10FFFFu ||
            (code_point >= 0xD800u && code_point <= 0xDFFFu)) {
          return false;
        }
        AppendCodePoint(output, code_point);
        continue;
      }
      if (position_ >= text_.size()) return false;
      const char escaped = text_[position_++];
      switch (escaped) {
        case '"':
        case '\\':
        case '/':
          output->push_back(escaped);
          break;
        case 'b':
          output->push_back('\b');
          break;
        case 'f':
          output->push_back('\f');
          break;
        case 'n':
          output->push_back('\n');
          break;
        case 'r':
          output->push_back('\r');
          break;
        case 't':
          output->push_back('\t');
          break;
        case 'u':
          if (!ParseUnicodeEscape(output)) return false;
          break;
        default:
          return false;
      }
    }
    return false;
  }

  bool SkipValue(unsigned depth) {
    if (depth > 16 || position_ >= text_.size()) return false;
    if (text_[position_] == '"') {
      std::string ignored;
      return ParseString(&ignored);
    }
    if (text_[position_] == '{') {
      ++position_;
      SkipWhitespace();
      if (Consume('}')) return true;
      for (;;) {
        std::string ignored;
        if (!ParseString(&ignored)) return false;
        SkipWhitespace();
        if (!Consume(':')) return false;
        SkipWhitespace();
        if (!SkipValue(depth + 1)) return false;
        SkipWhitespace();
        if (Consume('}')) return true;
        if (!Consume(',')) return false;
        SkipWhitespace();
      }
    }
    if (text_[position_] == '[') {
      ++position_;
      SkipWhitespace();
      if (Consume(']')) return true;
      for (;;) {
        if (!SkipValue(depth + 1)) return false;
        SkipWhitespace();
        if (Consume(']')) return true;
        if (!Consume(',')) return false;
        SkipWhitespace();
      }
    }
    for (const auto literal :
         {std::string_view{"true"}, std::string_view{"false"}, std::string_view{"null"}}) {
      if (text_.substr(position_).starts_with(literal)) {
        position_ += literal.size();
        return true;
      }
    }
    if (text_[position_] != '-' && (text_[position_] < '0' || text_[position_] > '9')) {
      return false;
    }
    if (text_[position_] == '-') ++position_;
    if (position_ >= text_.size()) return false;
    if (text_[position_] == '0') {
      ++position_;
      if (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') {
        return false;
      }
    } else {
      if (text_[position_] < '1' || text_[position_] > '9') return false;
      while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') {
        ++position_;
      }
    }
    if (position_ < text_.size() && text_[position_] == '.') {
      ++position_;
      const auto fraction_begin = position_;
      while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') {
        ++position_;
      }
      if (position_ == fraction_begin) return false;
    }
    if (position_ < text_.size() && (text_[position_] == 'e' || text_[position_] == 'E')) {
      ++position_;
      if (position_ < text_.size() && (text_[position_] == '+' || text_[position_] == '-')) {
        ++position_;
      }
      const auto exponent_begin = position_;
      while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') {
        ++position_;
      }
      if (position_ == exponent_begin) return false;
    }
    return true;
  }

  std::string_view text_;
  std::size_t position_ = 0;
};

Result<FenceUuid> DiscoverSessionGeneration(Transport& transport, const ClientConfig& config,
                                            std::string_view sid) {
  const auto metadata_key = BuildMetaSessionKey(config.prefix, sid);
  if (!metadata_key.has_value()) {
    return Result<FenceUuid>::Err(Status::InvalidKey, "invalid session id");
  }

  std::optional<FenceUuid> generation;
  bool invalid_reply = false;
  auto query = transport.Get(
      *metadata_key,
      [&](std::string_view key, std::span<const std::byte> payload, Encoding encoding) {
        if (key != *metadata_key || encoding.id != Encoding::kSitosV1) {
          invalid_reply = true;
          return false;
        }
        auto value = ParamValue::Decode(payload);
        if (!value.has_value() || value->type() != ValueType::Str) {
          invalid_reply = true;
          return false;
        }
        const auto json_value = value->As<std::string>();
        if (!json_value.has_value()) {
          invalid_reply = true;
          return false;
        }
        FenceUuid parsed{};
        if (!SessionMetadataParser(*json_value).Parse(&parsed)) {
          invalid_reply = true;
          return false;
        }
        if (generation.has_value()) {
          invalid_reply = true;
          return true;
        }
        generation = parsed;
        return true;
      },
      config.query_timeout);
  if (!query.IsOk()) return Result<FenceUuid>::ErrFrom(query);
  if (invalid_reply) {
    return Result<FenceUuid>::Err(Status::TypeMismatch, "invalid session metadata");
  }
  if (!generation.has_value()) return Result<FenceUuid>::Err(Status::NotFound);
  return Result<FenceUuid>::Ok(*generation);
}

}  // namespace

Result<BufferPublisher> BufferPublisher::OpenWithTransport(std::shared_ptr<Transport> transport,
                                                           ClientConfig config,
                                                           std::string_view sid,
                                                           BufferClass buffer_class) {
  if (!transport) return Result<BufferPublisher>::Err(Status::InvalidArgument, "null transport");
  auto validated = ValidateClientConfig(config);
  if (!validated.IsOk()) return Result<BufferPublisher>::ErrFrom(validated);
  if (config.zenoh_config_json.has_value()) {
    return Result<BufferPublisher>::Err(Status::InvalidArgument,
                                        "injected transport cannot apply zenoh configuration");
  }
  if (!IsValidSessionId(sid))
    return Result<BufferPublisher>::Err(Status::InvalidKey, "invalid session id");
  if (buffer_class != BufferClass::Durable && buffer_class != BufferClass::Ephemeral) {
    return Result<BufferPublisher>::Err(Status::InvalidArgument, "invalid buffer class");
  }
  if (!transport->SupportsFenceProfile()) {
    return Result<BufferPublisher>::Err(Status::InvalidArgument,
                                        "Transport does not support Fence");
  }
  auto generation = DiscoverSessionGeneration(*transport, config, sid);
  if (!generation.IsOk()) return Result<BufferPublisher>::ErrFrom(generation);

  fence_internal::FencePublisherBinding binding;
  binding.target = fence_internal::FencePublisherTarget::Buffer;
  binding.prefix = config.prefix;
  binding.sid = std::string(sid);
  binding.receiver_generation = generation.Value();
  binding.buffer_class = buffer_class;
  binding.durability = AckDurability::Applied;
  auto internal = std::make_unique<fence_internal::FencePublisher>(
      *transport, fence_internal::GenerateFenceUuid(), std::move(binding));
  internal->AllowSynced();
  return Result<BufferPublisher>::Ok(BufferPublisher(std::make_unique<BufferPublisher::Impl>(
      BufferPublisher::Impl{std::move(transport), std::move(internal), std::string(config.prefix),
                            std::string(sid), buffer_class})));
}

BufferPublisher::BufferPublisher(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
BufferPublisher::BufferPublisher(BufferPublisher&& other) noexcept = default;
BufferPublisher& BufferPublisher::operator=(BufferPublisher&& other) noexcept = default;
BufferPublisher::~BufferPublisher() = default;

Result<BufferPublisher> BufferPublisher::Open(ClientConfig config, std::string_view session_id,
                                              BufferClass buffer_class) {
  auto validated = ValidateClientConfig(config);
  if (!validated.IsOk()) return Result<BufferPublisher>::ErrFrom(validated);
  std::optional<std::string_view> json;
  if (config.zenoh_config_json.has_value()) json = *config.zenoh_config_json;
  auto transport = OpenZenohTransport(json);
  if (!transport.IsOk()) return Result<BufferPublisher>::ErrFrom(transport);
  // The normal overload applies zenoh_config_json while opening the transport. The
  // injected overload must reject that field, so clear it only after transport creation.
  config.zenoh_config_json.reset();
  return OpenWithTransport(std::shared_ptr<Transport>(std::move(transport).Value()),
                           std::move(config), session_id, buffer_class);
}

Result<BufferPublisher> BufferPublisher::Open(std::shared_ptr<Transport> transport,
                                              ClientConfig config, std::string_view session_id,
                                              BufferClass buffer_class) {
  return BufferPublisher::OpenWithTransport(std::move(transport), std::move(config), session_id,
                                            buffer_class);
}

Result<void> BufferPublisher::Push(std::string_view key, std::span<const std::byte> value) {
  if (!impl_) return Result<void>::Err(Status::Disconnected);
  const auto full_key = BuildBufferKey(impl_->prefix, impl_->sid, impl_->buffer_class, key);
  if (!full_key.has_value()) return Result<void>::Err(Status::InvalidKey, "invalid buffer key");
  return impl_->publisher->SubmitData(*full_key, value, Encoding{"zenoh/bytes"});
}

Result<FenceReceipt> BufferPublisher::Fence(FenceDurability durability,
                                            std::chrono::milliseconds timeout) {
  if (!impl_) return Result<FenceReceipt>::Err(Status::Disconnected);
  if (durability != FenceDurability::kApplied && durability != FenceDurability::kSynced) {
    return Result<FenceReceipt>::Err(Status::InvalidArgument, "invalid fence durability");
  }
  if (durability == FenceDurability::kSynced && impl_->buffer_class == BufferClass::Ephemeral) {
    return Result<FenceReceipt>::Err(Status::InvalidArgument,
                                     "synchronized Fence requires a durable buffer");
  }
  impl_->publisher->SetDurability(durability == FenceDurability::kSynced ? AckDurability::Synced
                                                                         : AckDurability::Applied);
  auto handle = impl_->publisher->BeginFence(timeout);
  if (!handle.IsOk()) return Result<FenceReceipt>::ErrFrom(handle);
  auto result = impl_->publisher->Wait(handle.Value());
  if (!result.IsOk()) {
    impl_->publisher->Close();
    return Result<FenceReceipt>::ErrFrom(result);
  }
  const auto& acknowledgement = result.Value();
  const auto expected_durability =
      durability == FenceDurability::kSynced ? AckDurability::Synced : AckDurability::Applied;
  if (acknowledgement.operation_kind != AckOperationKind::Fence ||
      acknowledgement.durability != expected_durability ||
      acknowledgement.through_sequence != handle.Value().through_sequence) {
    impl_->publisher->Close();
    return Result<FenceReceipt>::Err(Status::TypeMismatch,
                                     "Fence acknowledgement fingerprint mismatch");
  }
  if (acknowledgement.status != Status::Ok) {
    impl_->publisher->Close();
    return Result<FenceReceipt>::Err(acknowledgement.status, acknowledgement.message);
  }
  return Result<FenceReceipt>::Ok(FenceReceipt{acknowledgement.through_sequence, durability});
}

}  // namespace sitos

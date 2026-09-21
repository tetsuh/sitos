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
        const std::string_view json = *json_value;
        constexpr std::string_view prefix = R"({"state":"active","created_at":")";
        constexpr std::string_view middle = R"(","generation_uuid":")";
        constexpr std::string_view suffix = R"("})";
        if (!json.starts_with(prefix) || !json.ends_with(suffix)) {
          invalid_reply = true;
          return false;
        }
        const auto middle_position = json.find(middle, prefix.size());
        if (middle_position == std::string_view::npos ||
            middle_position + middle.size() > json.size() - suffix.size()) {
          invalid_reply = true;
          return false;
        }
        const auto generation_text =
            json.substr(middle_position + middle.size(),
                        json.size() - suffix.size() - (middle_position + middle.size()));
        auto parsed = fence_internal::ParseFenceUuid(generation_text);
        if (!parsed.has_value()) {
          invalid_reply = true;
          return false;
        }
        generation = *parsed;
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
  if (handle.Value().submission_diagnostic.has_value()) {
    const auto error = *handle.Value().submission_diagnostic;
    impl_->publisher->Close();
    return Result<FenceReceipt>::Err(error.status, error.message, error.cause);
  }
  auto result = impl_->publisher->Wait(handle.Value());
  if (!result.IsOk()) {
    impl_->publisher->Close();
    return Result<FenceReceipt>::ErrFrom(result);
  }
  const auto& acknowledgement = result.Value();
  if (acknowledgement.status != Status::Ok) {
    impl_->publisher->Close();
    return Result<FenceReceipt>::Err(acknowledgement.status, acknowledgement.message);
  }
  return Result<FenceReceipt>::Ok(FenceReceipt{acknowledgement.through_sequence, durability});
}

}  // namespace sitos

// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <vector>

#include "sitos/ack.hpp"
#include "sitos/buffer_publisher.hpp"
#include "sitos/param_value.hpp"

namespace {

class MetadataTransport final : public sitos::Transport {
 public:
  bool SupportsFenceProfile() const noexcept override { return true; }
  std::uint64_t FenceGeneration() const noexcept override { return generation; }

  sitos::Result<void> Put(std::string_view key, std::span<const std::byte>,
                          sitos::Encoding encoding, sitos::PutOptions options) override {
    last_key = std::string(key);
    last_encoding = std::move(encoding.id);
    last_options = std::move(options);
    if (last_encoding == sitos::Encoding::kSitosV1Fence) {
      marker_count++;
      if (marker_status.has_value()) {
        return sitos::Result<void>::Err(*marker_status, marker_message);
      }
    } else if (data_status.has_value()) {
      return sitos::Result<void>::Err(*data_status, data_message);
    }
    return sitos::Result<void>::Ok();
  }
  sitos::Result<void> Delete(std::string_view, sitos::PutOptions) override {
    return sitos::Result<void>::Ok();
  }
  sitos::Result<void> Get(std::string_view keyexpr, const QueryResultSink& sink,
                          std::chrono::milliseconds timeout) override {
    last_get_key = std::string(keyexpr);
    last_timeout = timeout;
    if (metadata_status.has_value() && keyexpr.find("/meta/ack/") == std::string_view::npos) {
      return sitos::Result<void>::Err(*metadata_status, metadata_message, metadata_cause);
    }
    if (metadata_no_reply && keyexpr.find("/meta/ack/") == std::string_view::npos) {
      return sitos::Result<void>::Ok();
    }
    if (keyexpr.find("/meta/ack/") != std::string_view::npos) {
      if (ack_timeout) return sitos::Result<void>::Err(sitos::Status::Timeout);
      const bool is_put = ack_kind == sitos::AckOperationKind::Put;
      const auto payload = sitos::EncodeAckResult(sitos::AckResultV1{
          ack_kind, ack_status, ack_durability, is_put ? 1U : 0U, sitos::kAckNoFailedIndex,
          is_put ? 0U : ack_through_sequence, sitos::kAckNoFailedSequence, "fence result"});
      if (!payload.IsOk()) return sitos::Result<void>::ErrFrom(payload);
      sink(keyexpr, payload.Value(), sitos::Encoding{std::string(sitos::Encoding::kSitosV1Ack)});
      return sitos::Result<void>::Ok();
    }
    const auto json =
        metadata_json.empty()
            ? (metadata_malformed
                   ? R"({"state":"active"})"
                   : R"({"state":"active","created_at":"2026-09-21T00:00:00Z","generation_uuid":"6f1c2d3e-4a5b-4c6d-8e9f-0123456789ab"})")
            : metadata_json;
    const auto payload =
        metadata_raw_payload.empty() ? sitos::ParamValue(json).Encode() : metadata_raw_payload;
    sink(keyexpr, payload, sitos::Encoding{metadata_encoding});
    return sitos::Result<void>::Ok();
  }
  sitos::Result<sitos::Subscription> DeclareSubscriber(
      std::string_view, std::function<void(const sitos::TransportSample&)>) override {
    return sitos::Result<sitos::Subscription>::Ok(sitos::Subscription{});
  }
  sitos::Result<sitos::Queryable> DeclareQueryable(
      std::string_view, std::function<void(sitos::TransportQuery&)>) override {
    return sitos::Result<sitos::Queryable>::Ok(sitos::Queryable{});
  }

  std::string last_key;
  std::string last_get_key;
  std::string last_encoding;
  sitos::PutOptions last_options;
  std::size_t marker_count = 0;
  sitos::Status ack_status = sitos::Status::Ok;
  sitos::AckOperationKind ack_kind = sitos::AckOperationKind::Fence;
  sitos::AckDurability ack_durability = sitos::AckDurability::Applied;
  std::uint64_t ack_through_sequence = 1;
  bool ack_timeout = false;
  std::optional<sitos::Status> marker_status;
  std::string marker_message;
  std::optional<sitos::Status> data_status;
  std::string data_message;
  std::optional<sitos::Status> metadata_status;
  std::string metadata_message;
  std::error_code metadata_cause;
  bool metadata_no_reply = false;
  bool metadata_malformed = false;
  std::string metadata_json;
  std::vector<std::byte> metadata_raw_payload;
  std::string metadata_encoding = std::string(sitos::Encoding::kSitosV1);
  std::uint64_t generation = 1;
  std::chrono::milliseconds last_timeout{};
};

}  // namespace

namespace sitos {
namespace {

TEST(BufferPublisherApiTest, MapsMetadataDiscoveryOutcomes) {
  auto no_reply = std::make_shared<MetadataTransport>();
  no_reply->metadata_no_reply = true;
  auto missing = BufferPublisher::Open(no_reply, ClientConfig{}, "sid", BufferClass::Durable);
  EXPECT_EQ(missing.StatusCode(), Status::NotFound);

  auto malformed = std::make_shared<MetadataTransport>();
  malformed->metadata_malformed = true;
  auto type_mismatch =
      BufferPublisher::Open(malformed, ClientConfig{}, "sid", BufferClass::Durable);
  EXPECT_EQ(type_mismatch.StatusCode(), Status::TypeMismatch);

  auto timeout = std::make_shared<MetadataTransport>();
  timeout->metadata_status = Status::Timeout;
  timeout->metadata_message = "metadata timeout";
  timeout->metadata_cause = std::make_error_code(std::errc::timed_out);
  auto timed_out = BufferPublisher::Open(timeout, ClientConfig{}, "sid", BufferClass::Durable);
  EXPECT_EQ(timed_out.StatusCode(), Status::Timeout);
  EXPECT_EQ(timed_out.Message(), "metadata timeout");
  EXPECT_EQ(timed_out.Error(), timeout->metadata_cause);

  auto wrong_encoding = std::make_shared<MetadataTransport>();
  wrong_encoding->metadata_encoding = "zenoh/bytes";
  EXPECT_EQ(BufferPublisher::Open(wrong_encoding, ClientConfig{}, "sid", BufferClass::Durable)
                .StatusCode(),
            Status::TypeMismatch);

  auto malformed_payload = std::make_shared<MetadataTransport>();
  malformed_payload->metadata_raw_payload = {std::byte{0xFF}};
  EXPECT_EQ(BufferPublisher::Open(malformed_payload, ClientConfig{}, "sid", BufferClass::Durable)
                .StatusCode(),
            Status::TypeMismatch);

  auto reordered = std::make_shared<MetadataTransport>();
  reordered->metadata_json =
      R"({ "extra": {"nested": [true, 2]}, "generation_uuid": "6f1c2d3e-4a5b-4c6d-8e9f-0123456789ab", "state": "active", "created_at": "2026-09-21T00:00:00Z" })";
  EXPECT_TRUE(BufferPublisher::Open(reordered, ClientConfig{}, "sid", BufferClass::Durable).IsOk());

  auto duplicate = std::make_shared<MetadataTransport>();
  duplicate->metadata_json =
      R"({"state":"active","state":"active","created_at":"now","generation_uuid":"6f1c2d3e-4a5b-4c6d-8e9f-0123456789ab"})";
  EXPECT_EQ(
      BufferPublisher::Open(duplicate, ClientConfig{}, "sid", BufferClass::Durable).StatusCode(),
      Status::TypeMismatch);

  auto non_string = std::make_shared<MetadataTransport>();
  non_string->metadata_json =
      R"({"state":true,"created_at":"now","generation_uuid":"6f1c2d3e-4a5b-4c6d-8e9f-0123456789ab"})";
  EXPECT_EQ(
      BufferPublisher::Open(non_string, ClientConfig{}, "sid", BufferClass::Durable).StatusCode(),
      Status::TypeMismatch);

  auto trailing = std::make_shared<MetadataTransport>();
  trailing->metadata_json =
      R"({"state":"active","created_at":"now","generation_uuid":"6f1c2d3e-4a5b-4c6d-8e9f-0123456789ab"} trailing)";
  EXPECT_EQ(
      BufferPublisher::Open(trailing, ClientConfig{}, "sid", BufferClass::Durable).StatusCode(),
      Status::TypeMismatch);

  for (const auto number : {std::string_view{"+1"}, std::string_view{".1"}, std::string_view{"01"},
                            std::string_view{"1."}, std::string_view{"1e"}}) {
    auto invalid_number = std::make_shared<MetadataTransport>();
    invalid_number->metadata_json =
        std::string(
            R"({"state":"active","created_at":"now","generation_uuid":"6f1c2d3e-4a5b-4c6d-8e9f-0123456789ab","extra":)") +
        std::string(number) + "}";
    EXPECT_EQ(BufferPublisher::Open(invalid_number, ClientConfig{}, "sid", BufferClass::Durable)
                  .StatusCode(),
              Status::TypeMismatch);
  }

  auto valid_unicode = std::make_shared<MetadataTransport>();
  valid_unicode->metadata_json =
      R"({"state":"active","created_at":"\uD83D\uDE00","generation_uuid":"6f1c2d3e-4a5b-4c6d-8e9f-0123456789ab"})";
  EXPECT_TRUE(
      BufferPublisher::Open(valid_unicode, ClientConfig{}, "sid", BufferClass::Durable).IsOk());

  auto malformed_unicode = std::make_shared<MetadataTransport>();
  malformed_unicode->metadata_json =
      R"({"state":"active","created_at":"\uD83D\u0041","generation_uuid":"6f1c2d3e-4a5b-4c6d-8e9f-0123456789ab"})";
  EXPECT_EQ(BufferPublisher::Open(malformed_unicode, ClientConfig{}, "sid", BufferClass::Durable)
                .StatusCode(),
            Status::TypeMismatch);

  auto uppercase = std::make_shared<MetadataTransport>();
  uppercase->metadata_json =
      R"({"state":"active","created_at":"2026-09-21T00:00:00Z","generation_uuid":"6F1C2D3E-4A5B-4C6D-8E9F-0123456789AB"})";
  EXPECT_EQ(
      BufferPublisher::Open(uppercase, ClientConfig{}, "sid", BufferClass::Durable).StatusCode(),
      Status::TypeMismatch);

  auto invalid_version = std::make_shared<MetadataTransport>();
  invalid_version->metadata_json =
      R"({"state":"active","created_at":"2026-09-21T00:00:00Z","generation_uuid":"6f1c2d3e-4a5b-5c6d-8e9f-0123456789ab"})";
  EXPECT_EQ(BufferPublisher::Open(invalid_version, ClientConfig{}, "sid", BufferClass::Durable)
                .StatusCode(),
            Status::TypeMismatch);

  auto invalid_variant = std::make_shared<MetadataTransport>();
  invalid_variant->metadata_json =
      R"({"state":"active","created_at":"2026-09-21T00:00:00Z","generation_uuid":"6f1c2d3e-4a5b-4c6d-0e9f-0123456789ab"})";
  EXPECT_EQ(BufferPublisher::Open(invalid_variant, ClientConfig{}, "sid", BufferClass::Durable)
                .StatusCode(),
            Status::TypeMismatch);
}

TEST(BufferPublisherApiTest, DiscoversGenerationAndOwnsPushPayloadSubmission) {
  auto transport = std::make_shared<MetadataTransport>();
  ClientConfig config;
  config.query_timeout = std::chrono::milliseconds{1234};
  auto opened = BufferPublisher::Open(transport, config, "sid", BufferClass::Durable);
  ASSERT_TRUE(opened.IsOk()) << opened.Message();
  EXPECT_EQ(transport->last_get_key, "sitos/meta/session/sid");
  EXPECT_EQ(transport->last_timeout, std::chrono::milliseconds{1234});
  std::vector<std::byte> payload{std::byte{0x01}, std::byte{0x02}};
  auto publisher = std::move(opened).Value();
  ASSERT_TRUE(publisher.Push("value", payload).IsOk());
  EXPECT_EQ(transport->last_key, "sitos/buffers/sid/durable/value");
  EXPECT_EQ(transport->last_encoding, "zenoh/bytes");
  ASSERT_TRUE(transport->last_options.fence_lane.has_value());
  EXPECT_EQ(transport->last_options.fence_lane->sequence, 1U);

  payload[0] = std::byte{0xff};
  EXPECT_EQ(transport->last_options.fence_lane->sequence, 1U);
}

TEST(BufferPublisherApiTest, AppliedFenceReturnsReceiptAndSyncedEphemeralIsLocal) {
  auto transport = std::make_shared<MetadataTransport>();
  auto opened = BufferPublisher::Open(transport, ClientConfig{}, "sid", BufferClass::Durable);
  ASSERT_TRUE(opened.IsOk());
  auto publisher = std::move(opened).Value();
  ASSERT_TRUE(publisher.Push("value", std::vector<std::byte>{std::byte{1}}).IsOk());
  const auto receipt = publisher.Fence(FenceDurability::kApplied, std::chrono::milliseconds{100});
  ASSERT_TRUE(receipt.IsOk()) << receipt.Message();
  EXPECT_EQ(receipt.Value().through_publish_sequence, 1U);
  EXPECT_EQ(receipt.Value().durability, FenceDurability::kApplied);
  EXPECT_EQ(transport->marker_count, 1U);

  auto ephemeral_transport = std::make_shared<MetadataTransport>();
  auto ephemeral_open =
      BufferPublisher::Open(ephemeral_transport, ClientConfig{}, "sid", BufferClass::Ephemeral);
  ASSERT_TRUE(ephemeral_open.IsOk());
  auto ephemeral = std::move(ephemeral_open).Value();
  const auto invalid = ephemeral.Fence(FenceDurability::kSynced, std::chrono::milliseconds{100});
  EXPECT_EQ(invalid.StatusCode(), Status::InvalidArgument);
  EXPECT_EQ(ephemeral_transport->marker_count, 0U);
}

TEST(BufferPublisherApiTest, CoveredDataSubmissionFailureDoesNotPreemptFence) {
  auto transport = std::make_shared<MetadataTransport>();
  transport->data_status = Status::Error;
  auto opened = BufferPublisher::Open(transport, ClientConfig{}, "sid", BufferClass::Durable);
  ASSERT_TRUE(opened.IsOk());
  auto publisher = std::move(opened).Value();
  EXPECT_EQ(publisher.Push("value", std::vector<std::byte>{std::byte{1}}).StatusCode(),
            Status::Error);
  const auto receipt = publisher.Fence(FenceDurability::kApplied, std::chrono::milliseconds{100});
  ASSERT_TRUE(receipt.IsOk()) << receipt.Message();
  EXPECT_EQ(receipt.Value().through_publish_sequence, 1U);
}

TEST(BufferPublisherApiTest, DiagnosticsAreBoundedSanitizedAndPreserveFirstFailure) {
  auto transport = std::make_shared<MetadataTransport>();
  transport->data_status = Status::Error;
  transport->data_message =
      "bad" + std::string(1, static_cast<char>(0xFF)) + std::string(2000, 'y');
  auto opened = BufferPublisher::Open(transport, ClientConfig{}, "sid", BufferClass::Durable);
  ASSERT_TRUE(opened.IsOk());
  auto publisher = std::move(opened).Value();
  EXPECT_EQ(publisher.Push("first", std::vector<std::byte>{std::byte{1}}).StatusCode(),
            Status::Error);
  transport->data_message = "later failure";
  EXPECT_EQ(publisher.Push("second", std::vector<std::byte>{std::byte{2}}).StatusCode(),
            Status::Error);
  transport->ack_timeout = true;
  const auto failed = publisher.Fence(FenceDurability::kApplied, std::chrono::milliseconds{100});
  ASSERT_EQ(failed.StatusCode(), Status::Timeout);
  EXPECT_LE(failed.Message().size(), sitos::kAckResultMaxMessageLength);
  EXPECT_NE(failed.Message().find("later failure"), 0U);
  EXPECT_NE(failed.Message().find('?'), std::string_view::npos);
}

TEST(BufferPublisherApiTest, MarkerSubmissionFailureMayStillCompleteFence) {
  auto transport = std::make_shared<MetadataTransport>();
  transport->marker_status = Status::Error;
  transport->ack_through_sequence = 0;
  auto opened = BufferPublisher::Open(transport, ClientConfig{}, "sid", BufferClass::Durable);
  ASSERT_TRUE(opened.IsOk());
  auto publisher = std::move(opened).Value();
  const auto receipt = publisher.Fence(FenceDurability::kApplied, std::chrono::milliseconds{100});
  ASSERT_TRUE(receipt.IsOk()) << receipt.Message();
  EXPECT_EQ(receipt.Value().through_publish_sequence, 0U);
  EXPECT_TRUE(publisher.Push("later", std::vector<std::byte>{std::byte{2}}).IsOk());
}

TEST(BufferPublisherApiTest, MarkerSubmissionFailureWithoutAckTimesOutAndDisconnects) {
  auto transport = std::make_shared<MetadataTransport>();
  transport->marker_status = Status::Error;
  transport->marker_message = "marker";
  transport->ack_timeout = true;
  auto opened = BufferPublisher::Open(transport, ClientConfig{}, "sid", BufferClass::Durable);
  ASSERT_TRUE(opened.IsOk());
  auto publisher = std::move(opened).Value();
  const auto failed = publisher.Fence(FenceDurability::kApplied, std::chrono::milliseconds{10});
  EXPECT_EQ(failed.StatusCode(), Status::Timeout);
  EXPECT_NE(failed.Message().find("marker"), std::string_view::npos);
  EXPECT_LE(failed.Message().size(), sitos::kAckResultMaxMessageLength);
  EXPECT_TRUE(static_cast<bool>(failed.Error()));
  EXPECT_EQ(publisher.Push("later", std::vector<std::byte>{std::byte{2}}).StatusCode(),
            Status::Disconnected);
}

TEST(BufferPublisherApiTest, FenceRejectsMismatchedAcknowledgementFingerprint) {
  for (const auto& [kind, durability, through] :
       {std::tuple{sitos::AckOperationKind::Put, sitos::AckDurability::Applied, std::uint64_t{1}},
        std::tuple{sitos::AckOperationKind::Fence, sitos::AckDurability::Synced, std::uint64_t{1}},
        std::tuple{sitos::AckOperationKind::Fence, sitos::AckDurability::Applied,
                   std::uint64_t{2}}}) {
    auto transport = std::make_shared<MetadataTransport>();
    transport->ack_kind = kind;
    transport->ack_durability = durability;
    transport->ack_through_sequence = through;
    auto opened = BufferPublisher::Open(transport, ClientConfig{}, "sid", BufferClass::Durable);
    ASSERT_TRUE(opened.IsOk());
    auto publisher = std::move(opened).Value();
    ASSERT_TRUE(publisher.Push("value", std::vector<std::byte>{std::byte{1}}).IsOk());
    const auto failed = publisher.Fence(FenceDurability::kApplied, std::chrono::milliseconds{100});
    EXPECT_EQ(failed.StatusCode(), Status::TypeMismatch);
    EXPECT_EQ(publisher.Push("later", std::vector<std::byte>{std::byte{2}}).StatusCode(),
              Status::Disconnected);
  }
}

TEST(BufferPublisherApiTest, GenerationReplacementDisconnectsBoundPublisher) {
  auto transport = std::make_shared<MetadataTransport>();
  auto opened = BufferPublisher::Open(transport, ClientConfig{}, "sid", BufferClass::Durable);
  ASSERT_TRUE(opened.IsOk());
  auto publisher = std::move(opened).Value();
  ASSERT_TRUE(publisher.Push("value", std::vector<std::byte>{std::byte{1}}).IsOk());
  transport->generation = 2;
  const auto before = transport->last_key;
  EXPECT_EQ(publisher.Push("later", std::vector<std::byte>{std::byte{2}}).StatusCode(),
            Status::Disconnected);
  EXPECT_EQ(transport->last_key, before);
}

TEST(BufferPublisherApiTest, FenceFailureDisconnectsPublisherAndTimeoutPreservesStatus) {
  auto transport = std::make_shared<MetadataTransport>();
  transport->ack_status = Status::Error;
  auto opened = BufferPublisher::Open(transport, ClientConfig{}, "sid", BufferClass::Durable);
  ASSERT_TRUE(opened.IsOk());
  auto publisher = std::move(opened).Value();
  ASSERT_TRUE(publisher.Push("value", std::vector<std::byte>{std::byte{1}}).IsOk());
  const auto failed = publisher.Fence(FenceDurability::kApplied, std::chrono::milliseconds{100});
  EXPECT_EQ(failed.StatusCode(), Status::Error);
  const auto markers = transport->marker_count;
  EXPECT_EQ(publisher.Push("later", std::vector<std::byte>{std::byte{2}}).StatusCode(),
            Status::Disconnected);
  EXPECT_EQ(transport->marker_count, markers);

  auto timeout_transport = std::make_shared<MetadataTransport>();
  timeout_transport->ack_timeout = true;
  auto timeout_open =
      BufferPublisher::Open(timeout_transport, ClientConfig{}, "sid", BufferClass::Durable);
  ASSERT_TRUE(timeout_open.IsOk());
  auto timeout_publisher = std::move(timeout_open).Value();
  const auto timeout =
      timeout_publisher.Fence(FenceDurability::kApplied, std::chrono::milliseconds{1});
  EXPECT_EQ(timeout.StatusCode(), Status::Timeout);
}

TEST(BufferPublisherApiTest, MoveTransfersUsabilityAndDisconnectsMovedFrom) {
  auto transport = std::make_shared<MetadataTransport>();
  auto first_open = BufferPublisher::Open(transport, ClientConfig{}, "sid", BufferClass::Durable);
  ASSERT_TRUE(first_open.IsOk());
  auto first = std::move(first_open).Value();
  auto moved = std::move(first);
  EXPECT_EQ(first.Push("moved", std::vector<std::byte>{std::byte{1}}).StatusCode(),
            Status::Disconnected);
  EXPECT_TRUE(moved.Push("usable", std::vector<std::byte>{std::byte{2}}).IsOk());

  auto second_open = BufferPublisher::Open(transport, ClientConfig{}, "sid", BufferClass::Durable);
  ASSERT_TRUE(second_open.IsOk());
  auto second = std::move(second_open).Value();
  second = std::move(moved);
  EXPECT_EQ(moved.Fence(FenceDurability::kApplied, std::chrono::milliseconds{10}).StatusCode(),
            Status::Disconnected);
  EXPECT_TRUE(second.Push("assigned", std::vector<std::byte>{std::byte{3}}).IsOk());
}

TEST(BufferPublisherApiTest, ExposesFrozenApiAndDurabilityTypes) {
  static_cast<void>(
      static_cast<Result<BufferPublisher> (*)(ClientConfig, std::string_view, BufferClass)>(
          &BufferPublisher::Open));
  const std::vector<std::byte> bytes{std::byte{0x01}, std::byte{0x02}};
  auto result = BufferPublisher::Open(ClientConfig{}, "sid", BufferClass::Ephemeral);
  EXPECT_FALSE(result.IsOk());
  EXPECT_EQ(result.StatusCode(), Status::Error);
  static_cast<void>(bytes);
}

}  // namespace
}  // namespace sitos

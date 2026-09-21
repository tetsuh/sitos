// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
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
      if (marker_status.has_value()) return sitos::Result<void>::Err(*marker_status);
    } else if (data_status.has_value()) {
      return sitos::Result<void>::Err(*data_status);
    }
    return sitos::Result<void>::Ok();
  }
  sitos::Result<void> Delete(std::string_view, sitos::PutOptions) override {
    return sitos::Result<void>::Ok();
  }
  sitos::Result<void> Get(std::string_view keyexpr, const QueryResultSink& sink,
                          std::chrono::milliseconds) override {
    if (metadata_status.has_value() && keyexpr.find("/meta/ack/") == std::string_view::npos) {
      return sitos::Result<void>::Err(*metadata_status);
    }
    if (metadata_no_reply && keyexpr.find("/meta/ack/") == std::string_view::npos) {
      return sitos::Result<void>::Ok();
    }
    if (keyexpr.find("/meta/ack/") != std::string_view::npos) {
      if (ack_timeout) return sitos::Result<void>::Err(sitos::Status::Timeout);
      const auto payload = sitos::EncodeAckResult(sitos::AckResultV1{
          sitos::AckOperationKind::Fence, ack_status, sitos::AckDurability::Applied, 0,
          sitos::kAckNoFailedIndex, 1, sitos::kAckNoFailedSequence, "fence result"});
      if (!payload.IsOk()) return sitos::Result<void>::ErrFrom(payload);
      sink(keyexpr, payload.Value(), sitos::Encoding{std::string(sitos::Encoding::kSitosV1Ack)});
      return sitos::Result<void>::Ok();
    }
    const auto payload =
        sitos::ParamValue(
            metadata_malformed
                ? R"({"state":"active"})"
                : R"({"state":"active","created_at":"2026-09-21T00:00:00Z","generation_uuid":"6f1c2d3e-4a5b-4c6d-8e9f-0123456789ab"})")
            .Encode();
    sink(keyexpr, payload, sitos::Encoding{std::string(sitos::Encoding::kSitosV1)});
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
  std::string last_encoding;
  sitos::PutOptions last_options;
  std::size_t marker_count = 0;
  sitos::Status ack_status = sitos::Status::Ok;
  bool ack_timeout = false;
  std::optional<sitos::Status> marker_status;
  std::optional<sitos::Status> data_status;
  std::optional<sitos::Status> metadata_status;
  bool metadata_no_reply = false;
  bool metadata_malformed = false;
  std::uint64_t generation = 1;
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
  auto timed_out = BufferPublisher::Open(timeout, ClientConfig{}, "sid", BufferClass::Durable);
  EXPECT_EQ(timed_out.StatusCode(), Status::Timeout);
}

TEST(BufferPublisherApiTest, DiscoversGenerationAndOwnsPushPayloadSubmission) {
  auto transport = std::make_shared<MetadataTransport>();
  auto opened = BufferPublisher::Open(transport, ClientConfig{}, "sid", BufferClass::Durable);
  ASSERT_TRUE(opened.IsOk()) << opened.Message();
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

TEST(BufferPublisherApiTest, MarkerSubmissionFailureDisconnectsPublisher) {
  auto transport = std::make_shared<MetadataTransport>();
  transport->marker_status = Status::Error;
  auto opened = BufferPublisher::Open(transport, ClientConfig{}, "sid", BufferClass::Durable);
  ASSERT_TRUE(opened.IsOk());
  auto publisher = std::move(opened).Value();
  const auto failed = publisher.Fence(FenceDurability::kApplied, std::chrono::milliseconds{100});
  EXPECT_EQ(failed.StatusCode(), Status::Error);
  EXPECT_EQ(publisher.Push("later", std::vector<std::byte>{std::byte{2}}).StatusCode(),
            Status::Disconnected);
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

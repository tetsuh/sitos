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

#include "sitos/buffer_publisher.hpp"
#include "sitos/param_value.hpp"

namespace {

class MetadataTransport final : public sitos::Transport {
 public:
  bool SupportsFenceProfile() const noexcept override { return true; }
  std::uint64_t FenceGeneration() const noexcept override { return 1; }

  sitos::Result<void> Put(std::string_view key, std::span<const std::byte>,
                          sitos::Encoding encoding, sitos::PutOptions options) override {
    last_key = std::string(key);
    last_encoding = std::move(encoding.id);
    last_options = std::move(options);
    return sitos::Result<void>::Ok();
  }
  sitos::Result<void> Delete(std::string_view, sitos::PutOptions) override {
    return sitos::Result<void>::Ok();
  }
  sitos::Result<void> Get(std::string_view keyexpr, const QueryResultSink& sink,
                          std::chrono::milliseconds) override {
    const auto payload =
        sitos::ParamValue(
            R"({"state":"active","created_at":"2026-09-21T00:00:00Z","generation_uuid":"6f1c2d3e-4a5b-4c6d-8e9f-0123456789ab"})")
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
};

}  // namespace

namespace sitos {
namespace {

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

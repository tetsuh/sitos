// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "ack_client.hpp"
#include "fence_internal.hpp"
#include "fence_test_access.hpp"
#include "sitos/ack.hpp"
#include "sitos/in_memory_engine.hpp"
#include "sitos/param_cache.hpp"
#include "sitos/storage_node.hpp"
#include "sitos/transport.hpp"
#include "storage_node_test_access.hpp"
#include "transport/zenoh_transport_test_access.hpp"

namespace {

using namespace std::chrono_literals;

constexpr sitos::FenceUuid kPublisherA{{
    std::byte{0x8b},
    std::byte{0x8f},
    std::byte{0x3a},
    std::byte{0x62},
    std::byte{0x7d},
    std::byte{0xd5},
    std::byte{0x4c},
    std::byte{0x40},
    std::byte{0x8a},
    std::byte{0x2b},
    std::byte{0x28},
    std::byte{0xf7},
    std::byte{0x13},
    std::byte{0x31},
    std::byte{0xfe},
    std::byte{0x41},
}};
constexpr sitos::FenceUuid kPublisherB{{
    std::byte{0x70},
    std::byte{0xd9},
    std::byte{0x8f},
    std::byte{0x35},
    std::byte{0x92},
    std::byte{0x17},
    std::byte{0x42},
    std::byte{0x5b},
    std::byte{0x8c},
    std::byte{0x9e},
    std::byte{0x14},
    std::byte{0xf6},
    std::byte{0x64},
    std::byte{0x54},
    std::byte{0x1b},
    std::byte{0x9a},
}};

struct OwnedObservation {
  std::string key;
  std::vector<std::byte> payload;
  sitos::Encoding encoding;
  sitos::AckAttachmentObservation ack;
  sitos::FenceLaneObservation lane;
};

TEST(FenceZenohIntegrationTest, QualifiesTopologiesQosAndControlIsolation) {
  auto opened = sitos::OpenZenohTransport();
  ASSERT_TRUE(opened.IsOk());
  std::shared_ptr<sitos::Transport> transport(std::move(opened).Value());
  ASSERT_TRUE(transport->SupportsFenceProfile());
  ASSERT_NE(transport->FenceGeneration(), 0U);
  EXPECT_TRUE(sitos::transport_test_access::UsesFencePutProfile());

  const std::string prefix =
      "sitos/fence_integration_" + std::to_string(transport->FenceGeneration());
  std::mutex mutex;
  std::condition_variable condition;
  std::vector<OwnedObservation> observations;
  auto declared =
      transport->DeclareSubscriber(prefix + "/**", [&](const sitos::TransportSample& sample) {
        {
          std::scoped_lock lock(mutex);
          observations.push_back(
              {sample.key, std::vector<std::byte>(sample.payload.begin(), sample.payload.end()),
               sample.encoding, sample.ack, sample.fence_lane});
        }
        condition.notify_all();
      });
  ASSERT_TRUE(declared.IsOk());
  auto subscription = std::move(declared).Value();

  const std::array<std::byte, 3> payload{std::byte{0x11}, std::byte{0x22}, std::byte{0x33}};
  sitos::PutOptions first;
  first.fence_lane = sitos::FenceLaneMetadata{kPublisherA, 1};
  ASSERT_TRUE(
      transport->Put(prefix + "/data/a", payload, sitos::Encoding{"zenoh/bytes"}, first).IsOk());
  sitos::PutOptions second;
  second.fence_lane = sitos::FenceLaneMetadata{kPublisherB, 1};
  ASSERT_TRUE(
      transport->Put(prefix + "/data/b", payload, sitos::Encoding{"zenoh/bytes"}, second).IsOk());

  const auto marker_payload = sitos::fence_internal::EncodeFenceMarker();
  const auto token = sitos::GenerateAckToken();
  sitos::PutOptions marker;
  marker.ack_token = token;
  const std::string marker_key = prefix +
                                 "/meta/fence/cache/s1/123e4567-e89b-42d3-a456-426614174000/"
                                 "8b8f3a62-7dd5-4c40-8a2b-28f71331fe41/2";
  ASSERT_TRUE(transport
                  ->Put(marker_key, marker_payload,
                        sitos::Encoding{std::string(sitos::Encoding::kSitosV1Fence)}, marker)
                  .IsOk());

  std::vector<OwnedObservation> observed;
  {
    std::unique_lock lock(mutex);
    ASSERT_TRUE(condition.wait_for(lock, 5s, [&] { return observations.size() >= 3; }));
    observed = observations;
  }
  ASSERT_EQ(observed.size(), 3U);
  const auto find_observation = [&](const std::string& key) {
    return std::find_if(observed.begin(), observed.end(),
                        [&](const OwnedObservation& item) { return item.key == key; });
  };
  const auto first_observation = find_observation(prefix + "/data/a");
  ASSERT_NE(first_observation, observed.end());
  EXPECT_EQ(first_observation->payload, std::vector<std::byte>(payload.begin(), payload.end()));
  EXPECT_EQ(first_observation->encoding.id, "zenoh/bytes");
  ASSERT_TRUE(std::holds_alternative<sitos::FenceLaneMetadata>(first_observation->lane));
  EXPECT_EQ(std::get<sitos::FenceLaneMetadata>(first_observation->lane).publisher_uuid, kPublisherA);
  const auto second_observation = find_observation(prefix + "/data/b");
  ASSERT_NE(second_observation, observed.end());
  ASSERT_TRUE(std::holds_alternative<sitos::FenceLaneMetadata>(second_observation->lane));
  EXPECT_EQ(std::get<sitos::FenceLaneMetadata>(second_observation->lane).publisher_uuid, kPublisherB);
  const auto marker_observation = find_observation(marker_key);
  ASSERT_NE(marker_observation, observed.end());
  ASSERT_TRUE(std::holds_alternative<sitos::AckToken>(marker_observation->ack));
  EXPECT_EQ(std::get<sitos::AckToken>(marker_observation->ack), token);
  EXPECT_TRUE(std::holds_alternative<sitos::FenceLaneAbsent>(marker_observation->lane));
  EXPECT_EQ(marker_observation->encoding.id, sitos::Encoding::kSitosV1Fence);

  sitos::PutOptions invalid;
  invalid.ack_token = token;
  invalid.fence_lane = sitos::FenceLaneMetadata{kPublisherA, 2};
  std::size_t before = 0;
  {
    std::scoped_lock lock(mutex);
    before = observations.size();
  }
  const auto rejected =
      transport->Put(prefix + "/invalid", payload, sitos::Encoding{"zenoh/bytes"}, invalid);
  EXPECT_EQ(rejected.StatusCode(), sitos::Status::InvalidArgument);
  {
    std::scoped_lock lock(mutex);
    EXPECT_EQ(observations.size(), before);
  }

  const std::string node_prefix = prefix + "/node";
  sitos::StorageNode node(*transport);
  ASSERT_TRUE(node.Start(std::make_shared<sitos::InMemoryEngine>(),
                         sitos::StorageNodeConfig{
                             .prefix = node_prefix,
                             .log_sink = nullptr,
                             .durable_buffer_engine_factory =
                                 [](std::string_view) {
                                   return sitos::Result<std::unique_ptr<sitos::StorageEngine>>::Ok(
                                       std::make_unique<sitos::InMemoryEngine>());
                                 }})
                  .IsOk());
  ASSERT_TRUE(node.CreateSession("s1", sitos::SessionOptions{.durable_buffers = true,
                                                             .ephemeral_buffers = true})
                  .IsOk());
  const auto generation =
      sitos::storage_node_test_access::StorageNodeTestAccess::SessionGeneration(node, "s1");
  ASSERT_TRUE(generation.has_value());

  sitos::ClientConfig cache_config;
  cache_config.prefix = node_prefix;
  cache_config.query_timeout = 1s;
  cache_config.log_sink = nullptr;
  auto cache_opened = sitos::ParamCache::Open(transport, std::move(cache_config));
  ASSERT_TRUE(cache_opened.IsOk());
  auto cache = std::move(cache_opened).Value();
  ASSERT_TRUE(cache.Attach("s1").IsOk());
  ASSERT_TRUE(sitos::fence_test_access::FenceTestAccess::ConfigureCacheFenceReceiver(
      cache, kPublisherB, kPublisherA));
  auto malformed_cache_fence =
      sitos::fence_test_access::FenceTestAccess::PublishCacheWaiterForTesting(cache, 0, 5s);
  ASSERT_TRUE(malformed_cache_fence.IsOk());

  auto remote_opened = sitos::OpenZenohTransport();
  ASSERT_TRUE(remote_opened.IsOk());
  std::shared_ptr<sitos::Transport> remote(std::move(remote_opened).Value());
  std::string uppercase_receiver = sitos::fence_internal::FormatFenceUuid(kPublisherB);
  std::transform(
      uppercase_receiver.begin(), uppercase_receiver.end(), uppercase_receiver.begin(),
      [](unsigned char character) { return static_cast<char>(std::toupper(character)); });
  const std::string malformed_cache_route =
      node_prefix + "/meta/fence/cache/s1/" + uppercase_receiver + "/" +
      sitos::fence_internal::FormatFenceUuid(kPublisherA) + "/00";
  sitos::PutOptions malformed_cache_ack;
  malformed_cache_ack.ack_token = malformed_cache_fence.Value().token;
  ASSERT_TRUE(remote
                  ->Put(malformed_cache_route, marker_payload,
                        sitos::Encoding{std::string(sitos::Encoding::kSitosV1Fence)},
                        malformed_cache_ack)
                  .IsOk());
  {
    auto waiter = malformed_cache_fence.Value().waiter;
    std::unique_lock lock(waiter->mutex);
    ASSERT_TRUE(waiter->condition.wait_for(lock, 5s, [&waiter] { return waiter->terminal; }));
    ASSERT_TRUE(waiter->result.has_value());
    EXPECT_EQ(waiter->result->status, sitos::Status::Error);
    EXPECT_EQ(waiter->result->through_sequence, 0U);
  }

  sitos::fence_internal::FencePublisher publisher(
      *remote, kPublisherA,
      sitos::fence_internal::FencePublisherBinding{
          sitos::fence_internal::FencePublisherTarget::Buffer, node_prefix, "s1", *generation,
          sitos::BufferClass::Durable, sitos::AckDurability::Applied});
  const auto buffer_key =
      sitos::BuildBufferKey(node_prefix, "s1", sitos::BufferClass::Durable, "e2e");
  ASSERT_TRUE(buffer_key.has_value());
  ASSERT_TRUE(publisher.SubmitData(*buffer_key, payload, sitos::Encoding{"zenoh/bytes"}).IsOk());
  auto fence = publisher.BeginFence(5s);
  ASSERT_TRUE(fence.IsOk());
  auto fence_result = publisher.Wait(fence.Value());
  ASSERT_TRUE(fence_result.IsOk());
  EXPECT_EQ(fence_result.Value().status, sitos::Status::Ok);
  EXPECT_EQ(fence_result.Value().through_sequence, 1U);

  sitos::fence_internal::FencePublisher shared_session_publisher(
      *remote, kPublisherB,
      sitos::fence_internal::FencePublisherBinding{
          sitos::fence_internal::FencePublisherTarget::Buffer, node_prefix, "s1", *generation,
          sitos::BufferClass::Durable, sitos::AckDurability::Applied});
  const auto shared_key =
      sitos::BuildBufferKey(node_prefix, "s1", sitos::BufferClass::Durable, "shared");
  ASSERT_TRUE(shared_key.has_value());
  ASSERT_TRUE(
      shared_session_publisher.SubmitData(*shared_key, payload, sitos::Encoding{"zenoh/bytes"})
          .IsOk());
  auto shared_fence = shared_session_publisher.BeginFence(5s);
  ASSERT_TRUE(shared_fence.IsOk());
  auto shared_result = shared_session_publisher.Wait(shared_fence.Value());
  ASSERT_TRUE(shared_result.IsOk());
  EXPECT_EQ(shared_result.Value().status, sitos::Status::Ok);

  sitos::fence_internal::FencePublisher loopback_publisher(
      *transport, sitos::fence_internal::GenerateFenceUuid(),
      sitos::fence_internal::FencePublisherBinding{
          sitos::fence_internal::FencePublisherTarget::Buffer, node_prefix, "s1", *generation,
          sitos::BufferClass::Ephemeral, sitos::AckDurability::Applied});
  const auto loopback_key =
      sitos::BuildBufferKey(node_prefix, "s1", sitos::BufferClass::Ephemeral, "loopback");
  ASSERT_TRUE(loopback_key.has_value());
  ASSERT_TRUE(
      loopback_publisher.SubmitData(*loopback_key, payload, sitos::Encoding{"zenoh/bytes"}).IsOk());
  auto loopback_fence = loopback_publisher.BeginFence(5s);
  ASSERT_TRUE(loopback_fence.IsOk());
  auto loopback_result = loopback_publisher.Wait(loopback_fence.Value());
  ASSERT_TRUE(loopback_result.IsOk());
  EXPECT_EQ(loopback_result.Value().status, sitos::Status::Ok);

  sitos::PutOptions gap;
  gap.fence_lane = sitos::FenceLaneMetadata{kPublisherA, 3};
  ASSERT_TRUE(remote->Put(*buffer_key, payload, sitos::Encoding{"zenoh/bytes"}, gap).IsOk());
  const auto gap_token = sitos::GenerateAckToken();
  const auto gap_marker = sitos::fence_internal::BuildBufferFenceMarkerKey(
      node_prefix, "s1", *generation, sitos::BufferClass::Durable, kPublisherA,
      sitos::AckDurability::Applied, 3);
  ASSERT_TRUE(gap_marker.has_value());
  sitos::PutOptions gap_ack;
  gap_ack.ack_token = gap_token;
  ASSERT_TRUE(remote
                  ->Put(*gap_marker, marker_payload,
                        sitos::Encoding{std::string(sitos::Encoding::kSitosV1Fence)}, gap_ack)
                  .IsOk());
  auto gap_result = sitos::PollAcknowledgement(*remote, node_prefix, gap_token,
                                               std::chrono::steady_clock::now() + 5s);
  ASSERT_TRUE(gap_result.IsOk());
  EXPECT_EQ(gap_result.Value().status, sitos::Status::OutcomeUnknown);
  EXPECT_EQ(gap_result.Value().failed_sequence, 2U);

  node.Stop();
  const auto disappeared_token = sitos::GenerateAckToken();
  sitos::PutOptions disappeared_ack;
  disappeared_ack.ack_token = disappeared_token;
  ASSERT_TRUE(remote
                  ->Put(*gap_marker, marker_payload,
                        sitos::Encoding{std::string(sitos::Encoding::kSitosV1Fence)},
                        disappeared_ack)
                  .IsOk());
  const auto disappeared =
      sitos::PollAcknowledgement(*remote, node_prefix, disappeared_token,
                                 std::chrono::steady_clock::now() + std::chrono::milliseconds(200));
  EXPECT_EQ(disappeared.StatusCode(), sitos::Status::Timeout);

  auto custom = sitos::OpenZenohTransport("{}");
  ASSERT_TRUE(custom.IsOk());
  EXPECT_FALSE(custom.Value()->SupportsFenceProfile());
}

TEST(FenceZenohIntegrationTest, QualifiesPublicParamCacheLocalDelivery) {
  auto transport_owner = sitos::MakeZenohTransport();
  ASSERT_TRUE(transport_owner) << "Failed to open zenoh session";
  std::shared_ptr<sitos::Transport> transport(transport_owner.release());
  ASSERT_TRUE(transport->SupportsFenceProfile());
  const auto transport_generation = transport->FenceGeneration();
  ASSERT_NE(transport_generation, 0U);

  // A non-default, per-session prefix must govern the marker route and the wait (X03).
  const std::string prefix = "sitos/fence_public_wait_" + std::to_string(transport_generation);
  sitos::StorageNode node(*transport);
  ASSERT_TRUE(node.Start(std::make_shared<sitos::InMemoryEngine>(),
                         sitos::StorageNodeConfig{.prefix = prefix, .log_sink = nullptr})
                  .IsOk());
  ASSERT_TRUE(node.CreateSession("s1").IsOk());

  sitos::ClientConfig cache_config;
  cache_config.prefix = prefix;
  cache_config.query_timeout = 1s;
  cache_config.log_sink = nullptr;
  auto cache_opened = sitos::ParamCache::Open(transport, std::move(cache_config));
  ASSERT_TRUE(cache_opened.IsOk());
  auto cache = std::move(cache_opened).Value();
  ASSERT_TRUE(cache.Attach("s1").IsOk());

  // An empty prefix succeeds over a real session.
  ASSERT_TRUE(cache.WaitForLocalDelivery(5s).IsOk());

  // A covered write must have crossed the initiating cache subscriber before the
  // wait returns successfully.
  ASSERT_TRUE(cache.Put("covered", std::int64_t{7}).IsOk());
  const auto waited = cache.WaitForLocalDelivery(5s);
  ASSERT_TRUE(waited.IsOk()) << waited.Message();
  EXPECT_GE(sitos::fence_test_access::FenceTestAccess::CacheCompletedThrough(cache), 1U);
  const auto value = cache.Get<std::int64_t>("covered");
  ASSERT_TRUE(value.IsOk());
  EXPECT_EQ(value.Value(), 7);

  // A peer cache is never waited for: it may still be behind when the wait returns.
  sitos::ClientConfig peer_config;
  peer_config.prefix = prefix;
  peer_config.query_timeout = 1s;
  peer_config.log_sink = nullptr;
  auto peer_opened = sitos::ParamCache::Open(transport, std::move(peer_config));
  ASSERT_TRUE(peer_opened.IsOk());
  auto peer = std::move(peer_opened).Value();
  ASSERT_TRUE(peer.Attach("s1").IsOk());
  ASSERT_TRUE(cache.Put("peer-independent", std::int64_t{9}).IsOk());
  EXPECT_TRUE(cache.WaitForLocalDelivery(5s).IsOk());

  // Control data never appears as a cache value.
  EXPECT_FALSE(cache.Contains("meta").Value());

  peer.Detach();
  cache.Detach();
  node.Stop();
}

}  // namespace

// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <memory>
#include <thread>

#include "fence_test_support.hpp"

namespace {

TEST(FenceLifecycleTest, QuiescesCallbacksAndPreventsPostReturnAccess) {
  auto transport = sitos::fence_test::MakeTransport();
  auto cache_result = sitos::fence_test::OpenAttachedCache(transport);
  ASSERT_TRUE(cache_result.IsOk());
  auto cache = std::move(cache_result).Value();
  ASSERT_TRUE(sitos::fence_test_access::FenceTestAccess::ConfigureCacheFenceReceiver(
      cache, sitos::fence_test::kAttachGeneration, sitos::fence_test::kPublisherA));

  auto pending = sitos::fence_test_access::FenceTestAccess::BeginCacheFence(
      cache, sitos::fence_test::kDeadline);
  ASSERT_TRUE(pending.IsOk());
  sitos::fence_test_access::FenceTestAccess::GateCacheCallback(cache);
  auto cache_delivery = std::async(std::launch::async, [&transport] {
    transport->Deliver(sitos::fence_test_access::FenceTestAccess::MakeCoveredCachePut(
        "sitos/session/s1/blocked", sitos::fence_test::kPublisherA, 1));
  });
  ASSERT_TRUE(sitos::fence_test_access::FenceTestAccess::WaitForCacheCallbackBlocked(cache));
  auto detach = std::async(std::launch::async, [&cache] { cache.Detach(); });
  ASSERT_TRUE(sitos::fence_test_access::FenceTestAccess::WaitUntilCacheAdmissionClosed(cache));
  EXPECT_FALSE(sitos::fence_test_access::FenceTestAccess::LateCacheMarkerCanAccessState(
      cache, sitos::fence_test::kAttachGeneration, sitos::fence_test::kPublisherA,
      pending.Value().token));
  sitos::fence_test_access::FenceTestAccess::ReleaseCacheCallback(cache);
  cache_delivery.get();
  detach.get();
  EXPECT_TRUE(sitos::fence_test_access::FenceTestAccess::CacheCallbacksQuiesced(cache));
  const auto cancelled =
      sitos::fence_test_access::FenceTestAccess::FenceWaiterResult(pending.Value());
  ASSERT_TRUE(cancelled.has_value());
  EXPECT_EQ(cancelled->status, sitos::Status::Disconnected);

  ASSERT_TRUE(cache.Attach(sitos::fence_test::kSid).IsOk());
  ASSERT_TRUE(sitos::fence_test_access::FenceTestAccess::ConfigureCacheFenceReceiver(
      cache, sitos::fence_test_access::FenceTestAccess::CacheAttachGeneration(cache),
      sitos::fence_test::kPublisherA));
  auto source_pending = sitos::fence_test_access::FenceTestAccess::BeginCacheFence(
      cache, sitos::fence_test::kDeadline);
  ASSERT_TRUE(source_pending.IsOk());
  auto moved = std::move(cache);
  EXPECT_TRUE(sitos::fence_test_access::FenceTestAccess::CacheFencePending(
      moved, source_pending.Value().token));
  EXPECT_TRUE(sitos::fence_test_access::FenceTestAccess::ReceiveCacheMarker(
      moved, sitos::fence_test_access::FenceTestAccess::CacheAttachGeneration(moved),
      sitos::fence_test::kPublisherA, source_pending.Value().token));
  EXPECT_EQ(
      sitos::fence_test_access::FenceTestAccess::FenceWaiterResult(source_pending.Value())->status,
      sitos::Status::Ok);
  auto destination_result = sitos::fence_test::OpenAttachedCache(transport);
  ASSERT_TRUE(destination_result.IsOk());
  auto destination = std::move(destination_result).Value();
  auto destination_pending = sitos::fence_test_access::FenceTestAccess::BeginCacheFence(
      destination, sitos::fence_test::kDeadline);
  ASSERT_TRUE(destination_pending.IsOk());
  destination = std::move(moved);
  EXPECT_EQ(
      sitos::fence_test_access::FenceTestAccess::FenceWaiterResult(destination_pending.Value())
          ->status,
      sitos::Status::Disconnected);

  auto node_transport = sitos::fence_test::MakeTransport();
  auto node = sitos::fence_test::StartNode(node_transport);
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(node->CreateSession("s1", sitos::fence_test::DurableSessionOptions()).IsOk());
  ASSERT_TRUE(sitos::fence_test_access::FenceTestAccess::SetSessionGeneration(
      *node, "s1", sitos::fence_test::kSessionGeneration));

  // Marker lease first: CloseSession waits for the admitted marker to publish
  // its immutable result before releasing the Session generation.
  sitos::fence_test_access::FenceTestAccess::GateFenceAfterTokenClaim(*node);
  const auto lease_token = sitos::fence_test::Token(std::byte{0x60});
  auto lease_delivery = std::async(std::launch::async, [&node_transport, lease_token] {
    node_transport->Deliver(sitos::fence_test_access::FenceTestAccess::MakeBufferMarker(
        "sitos", "s1", sitos::fence_test::kSessionGeneration, sitos::BufferClass::Durable,
        sitos::fence_test::kPublisherA, sitos::AckDurability::Applied, 0, lease_token));
  });
  ASSERT_TRUE(
      sitos::fence_test_access::FenceTestAccess::WaitForFenceTokenClaim(*node, lease_token));
  auto close = std::async(std::launch::async, [&node] { return node->CloseSession("s1"); });
  sitos::fence_test_access::FenceTestAccess::ReleaseFenceAfterTokenClaim(*node);
  lease_delivery.get();
  ASSERT_TRUE(close.get().IsOk());
  const auto lease_result =
      sitos::fence_test_access::FenceTestAccess::FindAckResult(*node, lease_token);
  ASSERT_TRUE(lease_result.has_value());
  EXPECT_EQ(lease_result->status, sitos::Status::Ok);

  // Closing first: a callback that selects the Closing record and observes the
  // closed dispatch registration must complete InvalidArgument before
  // CloseSession can erase the record and return.
  ASSERT_TRUE(
      node->CreateSession("closing-race", sitos::fence_test::DurableSessionOptions()).IsOk());
  ASSERT_TRUE(sitos::fence_test_access::FenceTestAccess::SetSessionGeneration(
      *node, "closing-race", sitos::fence_test::kSessionGeneration));
  ASSERT_TRUE(sitos::fence_test_access::FenceTestAccess::GateCloseAfterFenceDispatch(*node));
  ASSERT_TRUE(sitos::fence_test_access::FenceTestAccess::GateClosedMarkerFallback(*node));
  auto closing_close =
      std::async(std::launch::async, [&node] { return node->CloseSession("closing-race"); });
  ASSERT_TRUE(sitos::fence_test_access::FenceTestAccess::WaitForCloseAfterFenceDispatch(*node));
  const auto closing_token = sitos::fence_test::Token(std::byte{0x64});
  auto closing_delivery = std::async(std::launch::async, [&node_transport, closing_token] {
    node_transport->Deliver(sitos::fence_test_access::FenceTestAccess::MakeBufferMarker(
        "sitos", "closing-race", sitos::fence_test::kSessionGeneration, sitos::BufferClass::Durable,
        sitos::fence_test::kPublisherA, sitos::AckDurability::Applied, 0, closing_token));
  });
  ASSERT_TRUE(sitos::fence_test_access::FenceTestAccess::WaitForClosedMarkerFallback(*node));
  ASSERT_TRUE(sitos::fence_test_access::FenceTestAccess::ReleaseCloseAfterFenceDispatch(*node));
  EXPECT_EQ(closing_close.wait_for(std::chrono::milliseconds(20)), std::future_status::timeout)
      << "CloseSession must quiesce the selected Closing-first marker";
  ASSERT_TRUE(sitos::fence_test_access::FenceTestAccess::ReleaseClosedMarkerFallback(*node));
  closing_delivery.get();
  ASSERT_TRUE(closing_close.get().IsOk());
  const auto closing_result =
      sitos::fence_test_access::FenceTestAccess::FindAckResult(*node, closing_token);
  ASSERT_TRUE(closing_result.has_value());
  EXPECT_EQ(closing_result->status, sitos::Status::InvalidArgument);

  // Stop before marker observation leaves no result; Stop after token claim
  // completes exactly once with the ADR-0028 boundary status and quiesces all callbacks.
  const auto before_token = sitos::fence_test::Token(std::byte{0x61});
  node->Stop();
  node_transport->Deliver(sitos::fence_test_access::FenceTestAccess::MakeBufferMarker(
      "sitos", "s1", sitos::fence_test::kSessionGeneration, sitos::BufferClass::Durable,
      sitos::fence_test::kPublisherA, sitos::AckDurability::Applied, 0, before_token));
  EXPECT_FALSE(sitos::fence_test_access::FenceTestAccess::FindAckResult(*node, before_token));

  node = sitos::fence_test::StartNode(node_transport);
  ASSERT_NE(node, nullptr);
  ASSERT_TRUE(node->CreateSession("s1", sitos::fence_test::DurableSessionOptions()).IsOk());
  ASSERT_TRUE(sitos::fence_test_access::FenceTestAccess::SetSessionGeneration(
      *node, "s1", sitos::fence_test::kSessionGeneration));
  sitos::fence_test_access::FenceTestAccess::GateFenceAfterTokenClaim(*node);
  const auto after_token = sitos::fence_test::Token(std::byte{0x62});
  auto node_delivery = std::async(std::launch::async, [&node_transport, after_token] {
    node_transport->Deliver(sitos::fence_test_access::FenceTestAccess::MakeBufferMarker(
        "sitos", "s1", sitos::fence_test::kSessionGeneration, sitos::BufferClass::Durable,
        sitos::fence_test::kPublisherA, sitos::AckDurability::Applied, 0, after_token));
  });
  ASSERT_TRUE(
      sitos::fence_test_access::FenceTestAccess::WaitForFenceTokenClaim(*node, after_token));
  auto stop = std::async(std::launch::async, [&node] { node->Stop(); });
  ASSERT_TRUE(
      sitos::fence_test_access::FenceTestAccess::WaitUntilStorageNodeAdmissionClosed(*node));
  sitos::fence_test_access::FenceTestAccess::ReleaseFenceAfterTokenClaim(*node);
  node_delivery.get();
  stop.get();
  EXPECT_TRUE(sitos::fence_test_access::FenceTestAccess::StorageNodeCallbacksQuiesced(*node));
  EXPECT_FALSE(sitos::fence_test_access::FenceTestAccess::LateNodeCallbackCanAccessState(*node));
  const auto stopped_marker =
      sitos::fence_test_access::FenceTestAccess::LastCompletedFenceResult(*node);
  ASSERT_TRUE(stopped_marker.has_value());
  EXPECT_EQ(stopped_marker->status, sitos::Status::Error);

  auto create_transport = sitos::fence_test::MakeTransport();
  auto create_node = sitos::fence_test::StartNode(create_transport);
  ASSERT_NE(create_node, nullptr);
  ASSERT_TRUE(
      sitos::fence_test_access::FenceTestAccess::GateSessionAfterFenceRegistration(*create_node));
  auto create = std::async(std::launch::async, [&create_node] {
    return create_node->CreateSession("late", sitos::fence_test::DurableSessionOptions());
  });
  ASSERT_TRUE(
      sitos::fence_test_access::FenceTestAccess::WaitForSessionFenceRegistration(*create_node));
  auto stop_during_create = std::async(std::launch::async, [&create_node] { create_node->Stop(); });
  sitos::fence_test_access::FenceTestAccess::ReleaseSessionFenceRegistration(*create_node);
  ASSERT_TRUE(create.get().IsOk());
  stop_during_create.get();
  EXPECT_TRUE(
      sitos::fence_test_access::FenceTestAccess::StorageNodeCallbacksQuiesced(*create_node));
  EXPECT_FALSE(
      sitos::fence_test_access::FenceTestAccess::LateNodeCallbackCanAccessState(*create_node));

  // A failed CreateSession unpublishes its Creating record, quiesces both
  // dispatch registrations, and erases receiver proof admitted during creation.
  auto rollback_transport = sitos::fence_test::MakeTransport();
  sitos::StorageNode rollback_node(*rollback_transport);
  std::promise<void> factory_entered;
  std::promise<void> release_factory;
  const auto release_factory_future = release_factory.get_future().share();
  ASSERT_TRUE(
      rollback_node
          .Start(std::make_shared<sitos::InMemoryEngine>(),
                 sitos::StorageNodeConfig{
                     .prefix = "sitos",
                     .durable_buffer_engine_factory =
                         [&factory_entered, release_factory_future](std::string_view) mutable {
                           factory_entered.set_value();
                           release_factory_future.wait();
                           return sitos::Result<std::unique_ptr<sitos::StorageEngine>>::Err(
                               sitos::Status::Error, "injected factory failure");
                         }})
          .IsOk());
  auto failed_create = std::async(std::launch::async, [&rollback_node] {
    return rollback_node.CreateSession("rollback", sitos::fence_test::DurableSessionOptions());
  });
  factory_entered.get_future().wait();
  const auto creating_generation =
      sitos::fence_test_access::FenceTestAccess::VisibleSessionGeneration(rollback_node,
                                                                          "rollback");
  ASSERT_TRUE(creating_generation.has_value());
  rollback_transport->Deliver(sitos::fence_test_access::FenceTestAccess::MakeCoveredBufferPut(
      "sitos/buffers/rollback/durable/value", sitos::fence_test::kPublisherA, 1));
  EXPECT_EQ(sitos::fence_test_access::FenceTestAccess::FenceReceiverRegistryEntries(rollback_node),
            1U);
  release_factory.set_value();
  EXPECT_FALSE(failed_create.get().IsOk());
  EXPECT_EQ(sitos::fence_test_access::FenceTestAccess::FenceReceiverRegistryEntries(rollback_node),
            0U);
  rollback_node.Stop();

  // A covered observation admitted while a Session is Creating must retain its
  // definite lifecycle failure when creation later succeeds. It cannot survive
  // as an incomplete reservation that degrades the next Fence to OutcomeUnknown.
  auto activating_transport = sitos::fence_test::MakeTransport();
  sitos::StorageNode activating_node(*activating_transport);
  std::promise<void> activating_factory_entered;
  std::promise<void> release_activating_factory;
  const auto activating_release = release_activating_factory.get_future().share();
  ASSERT_TRUE(activating_node
                  .Start(std::make_shared<sitos::InMemoryEngine>(),
                         sitos::StorageNodeConfig{
                             .prefix = "sitos",
                             .durable_buffer_engine_factory =
                                 [&activating_factory_entered,
                                  activating_release](std::string_view) mutable {
                                   activating_factory_entered.set_value();
                                   activating_release.wait();
                                   return sitos::Result<std::unique_ptr<sitos::StorageEngine>>::Ok(
                                       std::make_unique<sitos::InMemoryEngine>());
                                 }})
                  .IsOk());
  auto activating_create = std::async(std::launch::async, [&activating_node] {
    return activating_node.CreateSession("activating", sitos::fence_test::DurableSessionOptions());
  });
  activating_factory_entered.get_future().wait();
  const auto activating_generation =
      sitos::fence_test_access::FenceTestAccess::VisibleSessionGeneration(activating_node,
                                                                          "activating");
  ASSERT_TRUE(activating_generation.has_value());
  activating_transport->Deliver(sitos::fence_test_access::FenceTestAccess::MakeCoveredBufferPut(
      "sitos/buffers/activating/durable/value", sitos::fence_test::kPublisherA, 1));
  EXPECT_EQ(
      sitos::fence_test_access::FenceTestAccess::FenceReceiverRegistryEntries(activating_node), 1U);
  release_activating_factory.set_value();
  ASSERT_TRUE(activating_create.get().IsOk());
  const auto activating_token = sitos::fence_test::Token(std::byte{0x63});
  activating_transport->Deliver(sitos::fence_test_access::FenceTestAccess::MakeBufferMarker(
      "sitos", "activating", *activating_generation, sitos::BufferClass::Durable,
      sitos::fence_test::kPublisherA, sitos::AckDurability::Applied, 1, activating_token));
  const auto activating_result =
      sitos::fence_test_access::FenceTestAccess::FindAckResult(activating_node, activating_token);
  ASSERT_TRUE(activating_result.has_value());
  EXPECT_EQ(activating_result->status, sitos::Status::InvalidArgument);
  EXPECT_EQ(activating_result->failed_sequence, 1U);
  EXPECT_FALSE(sitos::fence_test_access::FenceTestAccess::BufferValueExists(activating_node,
                                                                            "activating", "value"));
  activating_node.Stop();
}

TEST(FenceLifecycleTest, PublicWaitDetachCancelsAndQuiesces) {
  // Detach must complete an admitted public wait with Disconnected instead of
  // letting it consume its timeout, and must not access waiter state afterwards.
  auto transport = sitos::fence_test::MakeTransport();
  auto cache_result = sitos::fence_test::OpenAttachedCache(transport);
  ASSERT_TRUE(cache_result.IsOk());
  auto cache = std::move(cache_result).Value();
  ASSERT_TRUE(sitos::fence_test_access::FenceTestAccess::ConfigureCacheFenceReceiver(
      cache, sitos::fence_test::kAttachGeneration, sitos::fence_test::kPublisherA));

  transport->GateMarkerCompletion();
  auto pending = std::async(std::launch::async,
                            [&cache] { return cache.WaitForLocalDelivery(std::chrono::hours{1}); });
  while (transport->MarkerCount() == 0) std::this_thread::sleep_for(std::chrono::milliseconds{1});

  const auto started = std::chrono::steady_clock::now();
  cache.Detach();
  ASSERT_EQ(pending.wait_for(std::chrono::seconds{5}), std::future_status::ready)
      << "Detach must release the admitted wait promptly";
  const auto elapsed = std::chrono::steady_clock::now() - started;
  EXPECT_LT(elapsed, std::chrono::seconds{5}) << "the wait must not consume its own timeout";

  const auto result = pending.get();
  ASSERT_FALSE(result.IsOk());
  EXPECT_EQ(result.StatusCode(), sitos::Status::Disconnected);

  // A detached cache is inert; a late marker replay must not revive the terminal waiter.
  EXPECT_NO_THROW(transport->ReleaseMarkerCompletion());
  const auto after_detach = cache.WaitForLocalDelivery(sitos::fence_test::kDeadline);
  ASSERT_FALSE(after_detach.IsOk());
  EXPECT_EQ(after_detach.StatusCode(), sitos::Status::InvalidArgument);

  // Move assignment and destruction keep the same lifecycle contract.
  auto move_transport = sitos::fence_test::MakeTransport();
  auto move_result = sitos::fence_test::OpenAttachedCache(move_transport);
  ASSERT_TRUE(move_result.IsOk());
  auto source = std::move(move_result).Value();
  ASSERT_TRUE(sitos::fence_test_access::FenceTestAccess::ConfigureCacheFenceReceiver(
      source, sitos::fence_test::kAttachGeneration, sitos::fence_test::kPublisherA));
  {
    auto destination = std::move(source);
    const auto moved_to = destination.WaitForLocalDelivery(std::chrono::milliseconds{20});
    ASSERT_FALSE(moved_to.IsOk());
    EXPECT_EQ(moved_to.StatusCode(), sitos::Status::Timeout);
  }  // destruction quiesces without deadlock or post-return access
  const auto moved_from = source.WaitForLocalDelivery(sitos::fence_test::kDeadline);
  ASSERT_FALSE(moved_from.IsOk());
  EXPECT_EQ(moved_from.StatusCode(), sitos::Status::InvalidArgument);
}

}  // namespace


TEST(RocksDBBufferLifecycleTest, CloseReleasesEngineBeforeReturn) {
  RocksDbTransport transport;
  const auto root = UniqueRoot("close");
  auto gates = std::make_shared<GateState>();
  auto destroyed = std::make_shared<bool>(false);
  sitos::StorageNode node{transport};
  ASSERT_TRUE(node.Start(
      std::make_shared<sitos::InMemoryEngine>(),
      {.prefix = "sitos", .durable_buffer_engine_factory = [&](std::string_view sid) {
         auto opened = sitos::RocksDBEngine::Open((root / std::string(sid)).string());
         if (!opened.IsOk())
           return sitos::Result<std::unique_ptr<sitos::StorageEngine>>::ErrFrom(opened);
         return sitos::Result<std::unique_ptr<sitos::StorageEngine>>::Ok(
             std::make_unique<BlockingRocksDbEngine>(std::move(opened).Value(), gates, destroyed));
       }}));
  ASSERT_TRUE(node.CreateSession("session", {.durable_buffers = true}).IsOk());
  gates->block_put = true;
  gates->block_get = true;
  gates->block_list = true;

  std::thread put(
      [&] { transport.PutSample("sitos/buffers/session/durable/key", {std::byte{7}}); });
  std::thread get([&] {
    transport.Get(
        "sitos/buffers/session/durable/key", [](auto, auto, auto) { return true; },
        std::chrono::seconds(1));
  });
  std::thread list([&] {
    transport.Get(
        "sitos/buffers/session/durable/**", [](auto, auto, auto) { return true; },
        std::chrono::seconds(1));
  });
  {
    std::unique_lock lock(gates->mutex);
    gates->condition.wait(lock, [&] {
      return gates->put_entered > 0 && gates->get_entered > 0 && gates->list_entered > 0;
    });
  }

  std::optional<sitos::Result<void>> close_result;
  std::atomic<bool> close_done = false;
  std::thread close([&] {
    close_result = node.CloseSession("session");
    close_done.store(true, std::memory_order_release);
  });
  ASSERT_TRUE(
      sitos::storage_node_test_access::StorageNodeTestAccess::WaitForClosing(node, "session"));
  EXPECT_FALSE(close_done.load(std::memory_order_acquire));
  {
    std::scoped_lock lock(gates->mutex);
    gates->block_put = false;
    gates->block_get = false;
    gates->block_list = false;
  }
  gates->condition.notify_all();
  put.join();
  get.join();
  list.join();
  close.join();
  ASSERT_TRUE(close_result.has_value());
  EXPECT_TRUE(close_result->IsOk());
  EXPECT_TRUE(*destroyed);
  EXPECT_GT(std::filesystem::remove_all(root), 0u);
  EXPECT_FALSE(std::filesystem::exists(root));
}

TEST(RocksDBBufferLifecycleTest, SameSidRecreationUsesFreshEngine) {
  RocksDbTransport transport;
  const auto root = UniqueRoot("recreate");
  const auto first_root = root / "first";
  const auto second_root = root / "second";
  auto base = std::make_shared<sitos::InMemoryEngine>();
  std::vector<std::string> factory_sids;
  int factory_calls = 0;
  sitos::StorageNode node{transport};
  ASSERT_TRUE(node.Start(
      base, {.prefix = "sitos", .durable_buffer_engine_factory = [&](std::string_view sid) {
               factory_sids.emplace_back(sid);
               ++factory_calls;
               const auto path = factory_calls == 1 ? first_root : second_root;
               auto opened = sitos::RocksDBEngine::Open((path / std::string(sid)).string());
               if (!opened.IsOk())
                 return sitos::Result<std::unique_ptr<sitos::StorageEngine>>::ErrFrom(opened);
               return sitos::Result<std::unique_ptr<sitos::StorageEngine>>::Ok(
                   std::make_unique<BlockingRocksDbEngine>(std::move(opened).Value(),
                                                           std::make_shared<GateState>(),
                                                           std::make_shared<bool>(false)));
             }}));
  ASSERT_TRUE(node.CreateSession("session", {.durable_buffers = true}).IsOk());
  transport.PutSample("sitos/buffers/session/durable/key", {std::byte{7}});
  ASSERT_TRUE(node.CloseSession("session").IsOk());
  EXPECT_GT(std::filesystem::remove_all(first_root), 0u);
  EXPECT_FALSE(std::filesystem::exists(first_root));

  ASSERT_TRUE(node.CreateSession("session", {.durable_buffers = true}).IsOk());
  int replies = 0;
  ASSERT_TRUE(transport
                  .Get(
                      "sitos/buffers/session/durable/key",
                      [&](std::string_view, std::span<const std::byte>, sitos::Encoding) {
                        ++replies;
                        return true;
                      },
                      std::chrono::milliseconds(500))
                  .IsOk());
  EXPECT_EQ(factory_calls, 2);
  EXPECT_EQ(factory_sids, (std::vector<std::string>{"session", "session"}));
  EXPECT_EQ(replies, 0);
  ASSERT_TRUE(node.CloseSession("session").IsOk());
  EXPECT_GT(std::filesystem::remove_all(second_root), 0u);
  EXPECT_FALSE(std::filesystem::exists(second_root));
}

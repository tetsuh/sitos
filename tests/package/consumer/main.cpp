#include <cstdint>
#include <string>

#include "sitos/list_sink.hpp"
#include "sitos/param_concepts.hpp"
#include "sitos/rocksdb_engine.hpp"
#include "sitos/session_view.hpp"
#include "sitos/sitos.hpp"

static_assert(sitos::ParamInput<std::int64_t>);
static_assert(sitos::SupportedParamType<std::string>);
static_assert(sitos::ParamSpanElement<std::uint32_t>);

template <typename T>
concept HasAttachBase = requires(T& value) {
  value.AttachBase();
};

static_assert(!HasAttachBase<sitos::ParamCache>);

template <typename T>
concept HasSessionViewPut = requires(T& value) {
  value.Put("key", sitos::ParamValue(std::int64_t{1}));
};
static_assert(!HasSessionViewPut<sitos::SessionView>);

int main(int argc, char** argv) {
  static_cast<void>(sitos::MakeZenohTransport());
#if defined(SITOS_PACKAGE_CONSUMER_WITH_ROCKSDB)
  const std::string path = argc > 0 ? argv[0] : "rocksdb-consumer";
  auto result = sitos::RocksDBEngine::Open(path);
  volatile int status = static_cast<int>(result.StatusCode());
  static_cast<void>(status);
#endif
  return 0;
}

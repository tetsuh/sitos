#include <cstdint>
#include <string>

#include "sitos/list_sink.hpp"
#include "sitos/param_concepts.hpp"
#include "sitos/sitos.hpp"

static_assert(sitos::ParamInput<std::int64_t>);
static_assert(sitos::SupportedParamType<std::string>);
static_assert(sitos::ParamSpanElement<std::uint32_t>);

template <typename T>
concept HasAttachBase = requires(T& value) {
  value.AttachBase();
};

static_assert(!HasAttachBase<sitos::ParamCache>);

int main() {
  static_cast<void>(sitos::MakeZenohTransport());
  return 0;
}

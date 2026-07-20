#include <cstdint>
#include <string>

#include "sitos/list_sink.hpp"
#include "sitos/param_concepts.hpp"
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

int main() {
  static_cast<void>(sitos::MakeZenohTransport());
  return 0;
}

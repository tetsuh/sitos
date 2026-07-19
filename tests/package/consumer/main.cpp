#include "sitos/sitos.hpp"

template <typename T>
concept HasAttachBase = requires(T& value) {
  value.AttachBase();
};

static_assert(!HasAttachBase<sitos::ParamCache>);

int main() {
  static_cast<void>(sitos::MakeZenohTransport());
  return 0;
}

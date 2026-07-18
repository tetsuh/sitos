#include <memory>

#include "sitos/sitos.hpp"

int main() {
  auto (*factory)() -> std::unique_ptr<sitos::Transport> = &sitos::MakeZenohTransport;
  return factory == nullptr ? 1 : 0;
}

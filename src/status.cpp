// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include "sitos/status.hpp"

#include <cassert>
#include <string>

namespace sitos {
namespace {

class StatusCategory final : public std::error_category {
 public:
  const char* name() const noexcept override { return "sitos.status"; }

  std::string message(int value) const override {
    switch (static_cast<Status>(value)) {
      case Status::Ok:
        return "ok";
      case Status::NotFound:
        return "not found";
      case Status::TypeMismatch:
        return "type mismatch";
      case Status::Timeout:
        return "timeout";
      case Status::Disconnected:
        return "disconnected";
      case Status::ReadOnly:
        return "read-only";
      case Status::InvalidKey:
        return "invalid key";
      case Status::InvalidArgument:
        return "invalid argument";
      case Status::Error:
        return "error";
    }
    return "error";
  }
};

}  // namespace

const std::error_category& StatusErrorCategory() noexcept {
  static const StatusCategory kCategory;
  return kCategory;
}

std::error_code MakeErrorCode(Status status) {
  if (status == Status::Ok) {
    assert(false && "Status::Ok has no error code");
    status = Status::Error;
  }
  return {static_cast<int>(status), StatusErrorCategory()};
}

}  // namespace sitos

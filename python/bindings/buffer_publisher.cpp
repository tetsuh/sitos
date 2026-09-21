// Copyright 2026 sitos contributors
// SPDX-License-Identifier: Apache-2.0

#include "sitos/buffer_publisher.hpp"

#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>

#include "client_binding.hpp"
#include "param_value_conversion.hpp"

namespace nb = nanobind;
using namespace nb::literals;

namespace sitos::python::detail {
namespace {

class PyBufferPublisher {
 public:
  PyBufferPublisher(const std::string& sid, BufferClass buffer_class, const std::string& prefix,
                    const nb::object& json, const nb::handle& query_timeout) {
    ClientConfig config;
    config.prefix = prefix;
    config.query_timeout = std::chrono::milliseconds(GetTimeout(query_timeout));
    if (!json.is_none()) config.zenoh_config_json = nb::cast<std::string>(json);
    auto opened = [&config, &sid, buffer_class] {
      nb::gil_scoped_release release;
      return BufferPublisher::Open(std::move(config), sid, buffer_class);
    }();
    native_ = std::make_shared<BufferPublisher>(Take(std::move(opened)));
  }

  void Push(const std::string& key, const nb::handle& value) {
    auto converted = ParamValueFromPython(value);
    if (converted.type() != ValueType::Bytes) {
      throw nb::type_error("push accepts bytes or supported contiguous numpy arrays");
    }
    const auto bytes = converted.As<std::vector<std::byte>>();
    if (!bytes.has_value()) throw nb::type_error("push value is not bytes");
    auto result = [&] {
      nb::gil_scoped_release release;
      return native_->Push(key, *bytes);
    }();
    Take(std::move(result));
  }

  FenceReceipt Fence(FenceDurability durability, const nb::handle& timeout) {
    if (!nb::isinstance<nb::float_>(timeout) && !nb::isinstance<nb::int_>(timeout)) {
      throw nb::type_error("timeout must be a positive number of seconds");
    }
    const double seconds = nb::cast<double>(timeout);
    if (!std::isfinite(seconds) || seconds <= 0.0) {
      throw nb::value_error("timeout must be positive");
    }
    const double milliseconds = seconds * 1000.0;
    if (milliseconds > static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
      throw nb::value_error("timeout is outside the C++ duration range");
    }
    auto result = [&] {
      nb::gil_scoped_release release;
      return native_->Fence(durability,
                            std::chrono::milliseconds(static_cast<std::int64_t>(milliseconds)));
    }();
    return Take(std::move(result));
  }

 private:
  std::shared_ptr<BufferPublisher> native_;
};

}  // namespace

void BindBufferPublisher(nb::module_& module) {
  nb::enum_<BufferClass>(module, "BufferClass")
      .value("DURABLE", BufferClass::Durable)
      .value("EPHEMERAL", BufferClass::Ephemeral);
  nb::enum_<FenceDurability>(module, "FenceDurability")
      .value("APPLIED", FenceDurability::kApplied)
      .value("SYNCED", FenceDurability::kSynced);
  nb::class_<FenceReceipt>(module, "FenceReceipt")
      .def_ro("through_publish_sequence", &FenceReceipt::through_publish_sequence)
      .def_ro("durability", &FenceReceipt::durability);
  nb::class_<PyBufferPublisher>(module, "BufferPublisher")
      .def(nb::init<const std::string&, BufferClass, const std::string&, const nb::object&,
                    const nb::handle&>(),
           "session_id"_a, "buffer_class"_a, "prefix"_a = "sitos",
           "zenoh_config_json"_a = nb::none(), "query_timeout_ms"_a = 5000)
      .def("push", &PyBufferPublisher::Push, "key"_a, "value"_a)
      .def("fence", &PyBufferPublisher::Fence, "durability"_a, "timeout"_a);
}

}  // namespace sitos::python::detail

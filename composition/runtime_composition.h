#pragma once

#include <array>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#include "quickapp/core/package/package_loader.h"

namespace quickapp::examples {

inline constexpr std::array<std::string_view, 10> kRuntimeHostComponents{
    "View",   "Text",  "Button", "Image", "Input",
    "Switch", "Slider", "Picker", "List",  "Scroll"};

inline core::package::RuntimeComposition makeRuntimeComposition() {
  std::set<std::string, std::less<>> components;
  for (const auto component : kRuntimeHostComponents) {
    components.emplace(component);
  }
  return {"quickapp-kit-runtime-v1", "quickapp-kit-js-engine-v1",
          std::move(components),
          {"system.prompt", "system.router", "system.fetch", "system.device",
           "system.shortcut"}};
}

}  // namespace quickapp::examples

#pragma once

#include <array>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#include "quickapp/core/package/package_loader.h"

namespace quickapp::examples {

inline constexpr std::array<std::string_view, 11> kRuntimeHostComponents{
    "View",   "Text",  "Button", "Image", "Input",
    "Switch", "Slider", "Picker", "List",  "Scroll", "Tabs"};

// This is the admission list, not a promise that every feature has a native
// implementation on every platform. Keep it limited to the providers and
// compatibility capabilities used by the current real RPK baseline.
inline constexpr std::array<std::string_view, 6> kRuntimeCapabilities{
    "system.prompt", "system.router", "system.fetch", "system.file",
    "system.device", "system.shortcut"};

inline core::package::RuntimeComposition makeRuntimeComposition() {
  std::set<std::string, std::less<>> components;
  for (const auto component : kRuntimeHostComponents) {
    components.emplace(component);
  }
  std::set<std::string, std::less<>> capabilities;
  for (const auto capability : kRuntimeCapabilities) {
    capabilities.emplace(capability);
  }
  return {"quickapp-kit-runtime-v1", "quickapp-kit-js-engine-v1",
          std::move(components), std::move(capabilities)};
}

}  // namespace quickapp::examples

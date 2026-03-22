// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: Apache-2.0

#include "display.hpp"

#include <vulkan/vulkan.h>

#include <dlfcn.h>

namespace drm::vulkan {

Display::~Display() {
  if (instance_) {
    auto vkDestroyInstance =
        reinterpret_cast<PFN_vkDestroyInstance>(dlsym(RTLD_DEFAULT, "vkDestroyInstance"));
    if (vkDestroyInstance) {
      vkDestroyInstance(static_cast<VkInstance>(instance_), nullptr);
    }
  }
}

Display::Display(Display&& other) noexcept
    : displays_(std::move(other.displays_)),
      planes_(std::move(other.planes_)),
      instance_(other.instance_),
      physical_device_(other.physical_device_) {
  other.instance_ = nullptr;
  other.physical_device_ = nullptr;
}

Display& Display::operator=(Display&& other) noexcept {
  if (this != &other) {
    if (instance_) {
      auto vkDestroyInstance =
          reinterpret_cast<PFN_vkDestroyInstance>(dlsym(RTLD_DEFAULT, "vkDestroyInstance"));
      if (vkDestroyInstance) {
        vkDestroyInstance(static_cast<VkInstance>(instance_), nullptr);
      }
    }

    displays_ = std::move(other.displays_);
    planes_ = std::move(other.planes_);
    instance_ = other.instance_;
    physical_device_ = other.physical_device_;

    other.instance_ = nullptr;
    other.physical_device_ = nullptr;
  }
  return *this;
}

std::expected<Display, std::error_code> Display::create() {
  // Dynamically load Vulkan
  auto vkCreateInstance =
      reinterpret_cast<PFN_vkCreateInstance>(dlsym(RTLD_DEFAULT, "vkCreateInstance"));
  if (!vkCreateInstance) {
    return std::unexpected(std::make_error_code(std::errc::not_supported));
  }

  auto vkEnumeratePhysicalDevices = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
      dlsym(RTLD_DEFAULT, "vkEnumeratePhysicalDevices"));
  auto vkGetPhysicalDeviceDisplayPropertiesKHR =
      reinterpret_cast<PFN_vkGetPhysicalDeviceDisplayPropertiesKHR>(
          dlsym(RTLD_DEFAULT, "vkGetPhysicalDeviceDisplayPropertiesKHR"));
  auto vkGetPhysicalDeviceDisplayPlanePropertiesKHR =
      reinterpret_cast<PFN_vkGetPhysicalDeviceDisplayPlanePropertiesKHR>(
          dlsym(RTLD_DEFAULT, "vkGetPhysicalDeviceDisplayPlanePropertiesKHR"));
  auto vkGetDisplayPlaneSupportedDisplaysKHR =
      reinterpret_cast<PFN_vkGetDisplayPlaneSupportedDisplaysKHR>(
          dlsym(RTLD_DEFAULT, "vkGetDisplayPlaneSupportedDisplaysKHR"));

  if (!vkEnumeratePhysicalDevices || !vkGetPhysicalDeviceDisplayPropertiesKHR ||
      !vkGetPhysicalDeviceDisplayPlanePropertiesKHR || !vkGetDisplayPlaneSupportedDisplaysKHR) {
    return std::unexpected(std::make_error_code(std::errc::not_supported));
  }

  // Create a minimal Vulkan instance
  VkApplicationInfo app_info{};
  app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app_info.pApplicationName = "drm-cxx";
  app_info.apiVersion = VK_API_VERSION_1_0;

  const char* extensions[] = {VK_KHR_DISPLAY_EXTENSION_NAME};

  VkInstanceCreateInfo create_info{};
  create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  create_info.pApplicationInfo = &app_info;
  create_info.enabledExtensionCount = 1;
  create_info.ppEnabledExtensionNames = extensions;

  Display display;
  VkInstance instance{};
  VkResult result = vkCreateInstance(&create_info, nullptr, &instance);
  if (result != VK_SUCCESS) {
    return std::unexpected(std::make_error_code(std::errc::not_supported));
  }
  display.instance_ = instance;

  // Find a physical device
  uint32_t dev_count = 0;
  vkEnumeratePhysicalDevices(instance, &dev_count, nullptr);
  if (dev_count == 0) {
    return display;  // No devices, but valid
  }

  std::vector<VkPhysicalDevice> devices(dev_count);
  vkEnumeratePhysicalDevices(instance, &dev_count, devices.data());
  VkPhysicalDevice phys_dev = devices[0];
  display.physical_device_ = phys_dev;

  // Enumerate displays
  uint32_t display_count = 0;
  vkGetPhysicalDeviceDisplayPropertiesKHR(phys_dev, &display_count, nullptr);
  if (display_count > 0) {
    std::vector<VkDisplayPropertiesKHR> display_props(display_count);
    vkGetPhysicalDeviceDisplayPropertiesKHR(phys_dev, &display_count, display_props.data());

    for (const auto& dp : display_props) {
      DisplayInfo info{};
      info.display_handle = reinterpret_cast<uint64_t>(dp.display);
      if (dp.displayName) info.name = dp.displayName;
      info.width = dp.physicalResolution.width;
      info.height = dp.physicalResolution.height;
      display.displays_.push_back(std::move(info));
    }
  }

  // Enumerate display planes
  uint32_t plane_count = 0;
  vkGetPhysicalDeviceDisplayPlanePropertiesKHR(phys_dev, &plane_count, nullptr);
  if (plane_count > 0) {
    std::vector<VkDisplayPlanePropertiesKHR> plane_props(plane_count);
    vkGetPhysicalDeviceDisplayPlanePropertiesKHR(phys_dev, &plane_count, plane_props.data());

    for (uint32_t i = 0; i < plane_count; ++i) {
      DisplayPlaneInfo pi{};
      pi.plane_index = i;
      pi.current_stack_index = plane_props[i].currentStackIndex;

      // Get supported displays for this plane
      uint32_t supported_count = 0;
      vkGetDisplayPlaneSupportedDisplaysKHR(phys_dev, i, &supported_count, nullptr);
      if (supported_count > 0) {
        std::vector<VkDisplayKHR> supported(supported_count);
        vkGetDisplayPlaneSupportedDisplaysKHR(phys_dev, i, &supported_count, supported.data());
        for (auto d : supported) {
          pi.supported_displays.push_back(reinterpret_cast<uint64_t>(d));
        }
      }

      display.planes_.push_back(std::move(pi));
    }
  }

  return display;
}

const std::vector<DisplayInfo>& Display::displays() const noexcept {
  return displays_;
}

const std::vector<DisplayPlaneInfo>& Display::planes() const noexcept {
  return planes_;
}

std::vector<const DisplayPlaneInfo*> Display::planes_for_display(uint64_t display_handle) const {
  std::vector<const DisplayPlaneInfo*> result;
  for (const auto& plane : planes_) {
    for (auto dh : plane.supported_displays) {
      if (dh == display_handle) {
        result.push_back(&plane);
        break;
      }
    }
  }
  return result;
}

}  // namespace drm::vulkan

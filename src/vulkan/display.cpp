// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "display.hpp"

#include <drm-cxx/detail/expected.hpp>

#include <vulkan/vulkan_core.h>

#include <cstdint>
#include <dlfcn.h>
#include <system_error>
#include <utility>
#include <vector>

namespace drm::vulkan {

Display::~Display() {
  if (instance_ != nullptr) {
    auto* vk_destroy_instance =
        reinterpret_cast<PFN_vkDestroyInstance>(dlsym(RTLD_DEFAULT, "vkDestroyInstance"));
    if (vk_destroy_instance != nullptr) {
      vk_destroy_instance(static_cast<VkInstance>(instance_), nullptr);
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
    if (instance_ != nullptr) {
      auto* vk_destroy_instance =
          reinterpret_cast<PFN_vkDestroyInstance>(dlsym(RTLD_DEFAULT, "vkDestroyInstance"));
      if (vk_destroy_instance != nullptr) {
        vk_destroy_instance(static_cast<VkInstance>(instance_), nullptr);
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

drm::expected<Display, std::error_code> Display::create() {
  // Dynamically load Vulkan
  auto* vk_create_instance =
      reinterpret_cast<PFN_vkCreateInstance>(dlsym(RTLD_DEFAULT, "vkCreateInstance"));
  if (vk_create_instance == nullptr) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::not_supported));
  }

  auto* vk_enumerate_physical_devices = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
      dlsym(RTLD_DEFAULT, "vkEnumeratePhysicalDevices"));
  auto* vk_get_physical_device_display_properties_khr =
      reinterpret_cast<PFN_vkGetPhysicalDeviceDisplayPropertiesKHR>(
          dlsym(RTLD_DEFAULT, "vkGetPhysicalDeviceDisplayPropertiesKHR"));
  auto* vk_get_physical_device_display_plane_properties_khr =
      reinterpret_cast<PFN_vkGetPhysicalDeviceDisplayPlanePropertiesKHR>(
          dlsym(RTLD_DEFAULT, "vkGetPhysicalDeviceDisplayPlanePropertiesKHR"));
  auto* vk_get_display_plane_supported_displays_khr =
      reinterpret_cast<PFN_vkGetDisplayPlaneSupportedDisplaysKHR>(
          dlsym(RTLD_DEFAULT, "vkGetDisplayPlaneSupportedDisplaysKHR"));

  if ((vk_enumerate_physical_devices == nullptr) ||
      (vk_get_physical_device_display_properties_khr == nullptr) ||
      (vk_get_physical_device_display_plane_properties_khr == nullptr) ||
      (vk_get_display_plane_supported_displays_khr == nullptr)) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::not_supported));
  }

  // Create a minimal Vulkan instance
  VkApplicationInfo app_info{};
  app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app_info.pApplicationName = "drm-cxx";
  app_info.apiVersion = VK_API_VERSION_1_0;

  const char* const extensions[] = {VK_KHR_DISPLAY_EXTENSION_NAME};

  VkInstanceCreateInfo create_info{};
  create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  create_info.pApplicationInfo = &app_info;
  create_info.enabledExtensionCount = 1;
  create_info.ppEnabledExtensionNames = extensions;

  Display display;
  VkInstance instance{};
  VkResult const result = vk_create_instance(&create_info, nullptr, &instance);
  if (result != VK_SUCCESS) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::not_supported));
  }
  display.instance_ = instance;

  // Find a physical device
  uint32_t dev_count = 0;
  if (vk_enumerate_physical_devices(instance, &dev_count, nullptr) != VK_SUCCESS) {
    return display;  // Enumeration failed, return with instance only
  }
  if (dev_count == 0) {
    return display;  // No devices, but valid
  }

  std::vector<VkPhysicalDevice> devices(dev_count);
  if (vk_enumerate_physical_devices(instance, &dev_count, devices.data()) != VK_SUCCESS ||
      dev_count == 0) {
    return display;
  }
  VkPhysicalDevice phys_dev = devices.front();
  display.physical_device_ = phys_dev;

  // Enumerate displays
  uint32_t display_count = 0;
  if (vk_get_physical_device_display_properties_khr(phys_dev, &display_count, nullptr) !=
      VK_SUCCESS) {
    return display;
  }
  if (display_count > 0) {
    std::vector<VkDisplayPropertiesKHR> display_props(display_count);
    if (vk_get_physical_device_display_properties_khr(phys_dev, &display_count,
                                                      display_props.data()) != VK_SUCCESS) {
      return display;
    }

    for (const auto& dp : display_props) {
      DisplayInfo info{};
      info.display_handle = reinterpret_cast<uint64_t>(dp.display);
      if (dp.displayName != nullptr) {
        info.name = dp.displayName;
      }
      info.width = dp.physicalResolution.width;
      info.height = dp.physicalResolution.height;
      display.displays_.push_back(std::move(info));
    }
  }

  // Enumerate display planes
  uint32_t plane_count = 0;
  if (vk_get_physical_device_display_plane_properties_khr(phys_dev, &plane_count, nullptr) !=
      VK_SUCCESS) {
    return display;
  }
  if (plane_count > 0) {
    std::vector<VkDisplayPlanePropertiesKHR> plane_props(plane_count);
    if (vk_get_physical_device_display_plane_properties_khr(phys_dev, &plane_count,
                                                            plane_props.data()) != VK_SUCCESS) {
      return display;
    }

    for (uint32_t i = 0; i < plane_count; ++i) {
      DisplayPlaneInfo pi{};
      pi.plane_index = i;
      pi.current_stack_index = plane_props.at(i).currentStackIndex;

      // Get supported displays for this plane
      uint32_t supported_count = 0;
      if (vk_get_display_plane_supported_displays_khr(phys_dev, i, &supported_count, nullptr) !=
          VK_SUCCESS) {
        continue;
      }
      if (supported_count > 0) {
        std::vector<VkDisplayKHR> supported(supported_count);
        if (vk_get_display_plane_supported_displays_khr(phys_dev, i, &supported_count,
                                                        supported.data()) != VK_SUCCESS) {
          continue;
        }
        for (auto* d : supported) {
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

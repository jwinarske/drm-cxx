// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// cluster_sim_vulkan — instrument-cluster sibling of `cluster_sim` that
// uses a Vulkan-rendered DMA-BUF for the background layer. Demonstrates
// that drm-cxx's scene layer-source contract takes a Vulkan-allocated
// `VkImage` (exported as a dma-buf, wrapped in `ExternalDmaBufSource`)
// exactly like any other producer.
//
// On the validation Jetson Orin: bg layer is GPU-cleared at vblank
// cadence (color cycles through a low-saturation "cluster mood" palette
// — slow drift mimicking a dashboard's amber-to-blue mood lighting),
// instruments layer is the same Blend2D dial/info/warn paint cluster_sim
// uses, both layers land on hardware planes via the scene's allocator
// (bg → PRIMARY via the Tegra zpos-affinity rule, inst → OVERLAY), and
// hardware composes them.
//
// What this is NOT: a full port of cluster_sim's instruments to Vulkan
// shaders. Doing that cleanly needs render passes, vertex/fragment
// pipelines, and a SPIR-V toolchain (`glslangValidator` or `glslc`),
// none of which are installed by default on Jetson L4T. The
// vkCmdClearColorImage path used here is what `vulkan_scene` proves at
// 60 fps locked, and it's enough to demonstrate the "Vulkan-owned scene
// layer alongside CPU-owned scene layer" architecture. A full
// shader-rendered cluster_sim_vulkan is a follow-up.
//
// CLI mirrors `cluster_sim`:
//   cluster_sim_vulkan [--seconds N] [--mode WxH[@Hz]] [/dev/dri/cardN]
//
// Build gate: this file is built only when `vulkan` is on AND `blend2d`
// is on (for the dial paint). Both gates are honored in meson.build.

#include "../../common/open_output.hpp"

#include <drm-cxx/core/resources.hpp>
#include <drm-cxx/detail/format.hpp>
#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/modeset/mode.hpp>
#include <drm-cxx/modeset/page_flip.hpp>
#include <drm-cxx/scene/buffer_source.hpp>
#include <drm-cxx/scene/dumb_buffer_source.hpp>
#include <drm-cxx/scene/external_dma_buf_source.hpp>
#include <drm-cxx/scene/layer_desc.hpp>
#include <drm-cxx/scene/layer_scene.hpp>

#include <drm_fourcc.h>
#include <vulkan/vulkan_core.h>

// Blend2D umbrella: <blend2d/blend2d.h> on most distros, <blend2d.h>
// in older drops; cover both like cluster_sim does.
#if __has_include(<blend2d/blend2d.h>)
#include <blend2d/blend2d.h>  // NOLINT(misc-include-cleaner)
#elif __has_include(<blend2d.h>)
#include <blend2d.h>          // NOLINT(misc-include-cleaner)
#else
#error "Blend2D headers not found; cluster_sim_vulkan needs Blend2D installed"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

constexpr int k_default_seconds = 30;
constexpr double k_pi = 3.141592653589793;
// Dial sweep matches cluster_sim's: 3π/4 at the bottom-left, 3π/2 wide.
constexpr double k_dial_start_angle = 3.0 * k_pi / 4.0;
constexpr double k_dial_sweep_angle = 3.0 * k_pi / 2.0;
constexpr double k_speedo_period_s = 6.0;

struct Args {
  int seconds{k_default_seconds};
  std::optional<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>> mode_override;
};

std::atomic<bool> g_quit{false};
void on_sigint(int /*signo*/) noexcept { g_quit.store(true, std::memory_order_relaxed); }

[[nodiscard]] Args parse_args(int& argc, char**& argv) {
  Args a;
  int write = 1;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg{argv[i]};
    if (arg == "--seconds" && (i + 1) < argc) {
      a.seconds = std::atoi(argv[++i]);
    } else if (arg == "--mode" && (i + 1) < argc) {
      const std::string spec{argv[++i]};
      std::uint32_t w = 0;
      std::uint32_t h = 0;
      std::uint32_t hz = 0;
      auto* end = spec.data() + spec.size();
      auto* p = spec.data();
      auto consume = [&](std::uint32_t& out) {
        std::uint32_t v = 0;
        bool any = false;
        while (p < end && *p >= '0' && *p <= '9') {
          v = (v * 10U) + static_cast<std::uint32_t>(*p++ - '0');
          any = true;
        }
        if (any) {
          out = v;
        }
        return any;
      };
      if (consume(w) && p < end) {
        ++p;  // 'x'
        if (consume(h)) {
          if (p < end && *p == '@') {
            ++p;
            (void)consume(hz);
          }
          a.mode_override.emplace(w, h, hz);
        }
      }
    } else {
      argv[write++] = argv[i];
    }
  }
  argc = write;
  return a;
}

// Find the VkPhysicalDevice whose DRM major/minor matches the
// rendered-from KMS device. Cross-device dma-buf import works on some
// drivers but is non-portable.
[[nodiscard]] VkPhysicalDevice pick_physical_device(VkInstance instance, int drm_fd) {
  struct stat st{};
  if (fstat(drm_fd, &st) != 0) {
    return VK_NULL_HANDLE;
  }
  const unsigned int want_major = major(st.st_rdev);
  const unsigned int want_minor = minor(st.st_rdev);

  std::uint32_t count = 0;
  vkEnumeratePhysicalDevices(instance, &count, nullptr);
  if (count == 0) {
    return VK_NULL_HANDLE;
  }
  std::vector<VkPhysicalDevice> devices(count);
  vkEnumeratePhysicalDevices(instance, &count, devices.data());

  auto get_drm_props = reinterpret_cast<
      PFN_vkGetPhysicalDeviceProperties2>(  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
      vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties2"));
  if (get_drm_props == nullptr) {
    return devices.front();
  }

  for (auto* pd : devices) {
    VkPhysicalDeviceDrmPropertiesEXT drm_props{};
    drm_props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT;
    VkPhysicalDeviceProperties2 props2{};
    props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props2.pNext = &drm_props;
    get_drm_props(pd, &props2);
    if ((drm_props.hasPrimary != VK_FALSE && drm_props.primaryMajor == want_major &&
         drm_props.primaryMinor == want_minor) ||
        (drm_props.hasRender != VK_FALSE && drm_props.renderMajor == want_major &&
         drm_props.renderMinor == want_minor)) {
      return pd;
    }
  }
  return devices.front();
}

[[nodiscard]] std::uint32_t find_memory_type(VkPhysicalDevice pd, std::uint32_t type_bits,
                                             VkMemoryPropertyFlags flags) {
  VkPhysicalDeviceMemoryProperties mp{};
  vkGetPhysicalDeviceMemoryProperties(pd, &mp);
  const drm::span<const VkMemoryType> types{mp.memoryTypes, mp.memoryTypeCount};
  for (std::uint32_t i = 0; i < types.size(); ++i) {
    if (((type_bits >> i) & 1U) == 0U) {
      continue;
    }
    if ((types[i].propertyFlags & flags) == flags) {
      return i;
    }
  }
  return 0xFFFFFFFFU;
}

[[nodiscard]] double dial_norm_from_phase(double phase01) noexcept {
  return 0.5 * (1.0 - std::cos(2.0 * k_pi * phase01));
}

// Paint a single dial face + needle into an XRGB8888 buffer. Shrunk
// from cluster_sim's full dial paint to keep the example focused on
// the Vulkan-side architecture. One dial centered on the buffer.
void paint_instruments(drm::BufferMapping& mapping, std::uint32_t width, std::uint32_t height,
                       double norm) noexcept {
  if (width == 0U || height == 0U) {
    return;
  }
  drm::span<std::uint8_t> const pixels = mapping.pixels();
  std::uint32_t const stride = mapping.stride();
  if (pixels.size() < static_cast<std::size_t>(height) * stride) {
    return;
  }
  BLImage canvas;
  if (canvas.create_from_data(static_cast<int>(width), static_cast<int>(height), BL_FORMAT_PRGB32,
                              pixels.data(), static_cast<intptr_t>(stride), BL_DATA_ACCESS_RW,
                              nullptr, nullptr) != BL_SUCCESS) {
    return;
  }
  BLContext ctx(canvas);
  // alpha=0 background so hw compose lets the Vulkan bg show through
  // outside the dial.
  ctx.set_comp_op(BL_COMP_OP_SRC_COPY);
  ctx.fill_all(BLRgba32(0x00000000U));
  ctx.set_comp_op(BL_COMP_OP_SRC_OVER);

  const double cx = static_cast<double>(width) / 2.0;
  const double cy = static_cast<double>(height) / 2.0;
  const double r = std::min(cx, cy) * 0.92;
  const double r_rim = r * 1.00;
  const double r_inner = r * 0.90;
  const double r_needle = r * 0.85;
  const double r_hub = r * 0.10;

  ctx.fill_circle(BLCircle(cx, cy, r_rim), BLRgba32(0xFF606870U));
  ctx.fill_circle(BLCircle(cx, cy, r_inner), BLRgba32(0xFF080C18U));

  // Needle.
  const double a = k_dial_start_angle + (std::clamp(norm, 0.0, 1.0) * k_dial_sweep_angle);
  const double nx = cx + (r_needle * std::cos(a));
  const double ny = cy + (r_needle * std::sin(a));
  ctx.set_stroke_width(std::max(3.0, r * 0.025));
  ctx.set_stroke_caps(BL_STROKE_CAP_ROUND);
  ctx.stroke_line(BLPoint(cx, cy), BLPoint(nx, ny), BLRgba32(0xFFFF3B30U));
  ctx.fill_circle(BLCircle(cx, cy, r_hub), BLRgba32(0xFF1A1F2CU));
  ctx.end();
}

}  // namespace

int main(int argc, char* argv[]) {
  std::signal(SIGINT, on_sigint);
  std::signal(SIGTERM, on_sigint);

  Args args = parse_args(argc, argv);

  auto out = drm::examples::open_and_pick_output(argc, argv);
  if (!out) {
    return EXIT_FAILURE;
  }
  auto& device = out->device;

  if (args.mode_override.has_value()) {
    const auto [tw, th, thz] = *args.mode_override;
    auto conn = drm::get_connector(device.fd(), out->connector_id);
    if (conn) {
      auto picked = drm::select_mode(
          drm::span<const drmModeModeInfo>(conn->modes, conn->count_modes), tw, th, thz);
      if (picked) {
        const auto& m = picked->drm_mode;
        if (m.hdisplay == tw && m.vdisplay == th && (thz == 0 || m.vrefresh == thz)) {
          out->mode = m;
        }
      }
    }
  }

  const std::uint32_t fb_w = out->mode.hdisplay;
  const std::uint32_t fb_h = out->mode.vdisplay;
  drm::println(stderr, "cluster_sim_vulkan: mode = {}x{}@{}Hz crtc={} connector={}", fb_w, fb_h,
               out->mode.vrefresh, out->crtc_id, out->connector_id);

  drm::scene::LayerScene::Config scene_cfg;
  scene_cfg.crtc_id = out->crtc_id;
  scene_cfg.connector_id = out->connector_id;
  scene_cfg.mode = out->mode;
  auto scene_r = drm::scene::LayerScene::create(device, scene_cfg);
  if (!scene_r) {
    drm::println(stderr, "LayerScene::create: {}", scene_r.error().message());
    return EXIT_FAILURE;
  }
  auto& scene = *scene_r;

  // Vulkan instance.
  const std::array<const char*, 2> inst_exts{
      "VK_KHR_get_physical_device_properties2",
      "VK_KHR_external_memory_capabilities",
  };
  VkApplicationInfo app{};
  app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo ici{};
  ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  ici.pApplicationInfo = &app;
  ici.enabledExtensionCount = static_cast<std::uint32_t>(inst_exts.size());
  ici.ppEnabledExtensionNames = inst_exts.data();
  VkInstance instance = VK_NULL_HANDLE;
  if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) {
    drm::println(stderr, "vkCreateInstance failed");
    return EXIT_FAILURE;
  }

  VkPhysicalDevice pd = pick_physical_device(instance, device.fd());
  if (pd == VK_NULL_HANDLE) {
    drm::println(stderr, "no VkPhysicalDevice matched the DRM fd");
    vkDestroyInstance(instance, nullptr);
    return EXIT_FAILURE;
  }

  std::uint32_t qf_count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(pd, &qf_count, nullptr);
  std::vector<VkQueueFamilyProperties> qfs(qf_count);
  vkGetPhysicalDeviceQueueFamilyProperties(pd, &qf_count, qfs.data());
  std::uint32_t qf_index = 0xFFFFFFFFU;
  for (std::uint32_t i = 0; i < qf_count; ++i) {
    if ((qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0U) {
      qf_index = i;
      break;
    }
  }
  if (qf_index == 0xFFFFFFFFU) {
    drm::println(stderr, "no graphics queue");
    vkDestroyInstance(instance, nullptr);
    return EXIT_FAILURE;
  }

  const float qp = 1.0F;
  VkDeviceQueueCreateInfo qci{};
  qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  qci.queueFamilyIndex = qf_index;
  qci.queueCount = 1;
  qci.pQueuePriorities = &qp;
  const std::array<const char*, 4> dev_exts{
      "VK_KHR_external_memory",
      "VK_KHR_external_memory_fd",
      "VK_EXT_external_memory_dma_buf",
      "VK_EXT_image_drm_format_modifier",
  };
  VkDeviceCreateInfo dci{};
  dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  dci.enabledExtensionCount = static_cast<std::uint32_t>(dev_exts.size());
  dci.ppEnabledExtensionNames = dev_exts.data();
  VkDevice vk_device = VK_NULL_HANDLE;
  if (vkCreateDevice(pd, &dci, nullptr, &vk_device) != VK_SUCCESS) {
    drm::println(stderr, "vkCreateDevice failed");
    vkDestroyInstance(instance, nullptr);
    return EXIT_FAILURE;
  }
  VkQueue queue = VK_NULL_HANDLE;
  vkGetDeviceQueue(vk_device, qf_index, 0, &queue);

  // Vulkan-allocated DRM-modifier-LINEAR ARGB8888 image for the bg.
  const std::uint64_t modifier = DRM_FORMAT_MOD_LINEAR;
  std::array<std::uint64_t, 1> modifiers{modifier};
  VkImageDrmFormatModifierListCreateInfoEXT drm_list{};
  drm_list.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT;
  drm_list.drmFormatModifierCount = 1;
  drm_list.pDrmFormatModifiers = modifiers.data();
  VkExternalMemoryImageCreateInfo ext_img{};
  ext_img.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
  ext_img.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
  ext_img.pNext = &drm_list;
  VkImageCreateInfo iic{};
  iic.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  iic.pNext = &ext_img;
  iic.imageType = VK_IMAGE_TYPE_2D;
  iic.format = VK_FORMAT_B8G8R8A8_UNORM;  // matches DRM_FORMAT_ARGB8888 byte order
  iic.extent = {fb_w, fb_h, 1};
  iic.mipLevels = 1;
  iic.arrayLayers = 1;
  iic.samples = VK_SAMPLE_COUNT_1_BIT;
  iic.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
  iic.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  iic.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  iic.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkImage vk_image = VK_NULL_HANDLE;
  if (vkCreateImage(vk_device, &iic, nullptr, &vk_image) != VK_SUCCESS) {
    drm::println(stderr, "vkCreateImage failed");
    vkDestroyDevice(vk_device, nullptr);
    vkDestroyInstance(instance, nullptr);
    return EXIT_FAILURE;
  }
  VkMemoryRequirements mr{};
  vkGetImageMemoryRequirements(vk_device, vk_image, &mr);
  VkExportMemoryAllocateInfo emai{};
  emai.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
  emai.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
  VkMemoryAllocateInfo mai{};
  mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  mai.pNext = &emai;
  mai.allocationSize = mr.size;
  mai.memoryTypeIndex = find_memory_type(pd, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (mai.memoryTypeIndex == 0xFFFFFFFFU) {
    drm::println(stderr, "no DEVICE_LOCAL memory type");
    vkDestroyImage(vk_device, vk_image, nullptr);
    vkDestroyDevice(vk_device, nullptr);
    vkDestroyInstance(instance, nullptr);
    return EXIT_FAILURE;
  }
  VkDeviceMemory vk_mem = VK_NULL_HANDLE;
  if (vkAllocateMemory(vk_device, &mai, nullptr, &vk_mem) != VK_SUCCESS) {
    drm::println(stderr, "vkAllocateMemory failed");
    vkDestroyImage(vk_device, vk_image, nullptr);
    vkDestroyDevice(vk_device, nullptr);
    vkDestroyInstance(instance, nullptr);
    return EXIT_FAILURE;
  }
  vkBindImageMemory(vk_device, vk_image, vk_mem, 0);

  auto get_memory_fd = reinterpret_cast<
      PFN_vkGetMemoryFdKHR>(  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
      vkGetDeviceProcAddr(vk_device, "vkGetMemoryFdKHR"));
  if (get_memory_fd == nullptr) {
    drm::println(stderr, "vkGetMemoryFdKHR missing");
    return EXIT_FAILURE;
  }
  VkMemoryGetFdInfoKHR mgfi{};
  mgfi.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
  mgfi.memory = vk_mem;
  mgfi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
  int dmabuf_fd = -1;
  if (get_memory_fd(vk_device, &mgfi, &dmabuf_fd) != VK_SUCCESS || dmabuf_fd < 0) {
    drm::println(stderr, "vkGetMemoryFdKHR failed");
    return EXIT_FAILURE;
  }
  VkImageSubresource sub{};
  sub.aspectMask = VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT;
  VkSubresourceLayout layout{};
  vkGetImageSubresourceLayout(vk_device, vk_image, &sub, &layout);
  drm::scene::ExternalPlaneInfo plane_info{};
  plane_info.fd = dmabuf_fd;
  plane_info.pitch = static_cast<std::uint32_t>(layout.rowPitch);
  plane_info.offset = static_cast<std::uint32_t>(layout.offset);
  std::array<drm::scene::ExternalPlaneInfo, 1> planes{plane_info};
  auto bg_source =
      drm::scene::ExternalDmaBufSource::create(device, fb_w, fb_h, DRM_FORMAT_ARGB8888, modifier,
                                                planes);
  ::close(dmabuf_fd);
  if (!bg_source) {
    drm::println(stderr, "ExternalDmaBufSource::create: {}", bg_source.error().message());
    return EXIT_FAILURE;
  }
  drm::scene::LayerDesc bg_desc;
  bg_desc.source = std::move(*bg_source);
  bg_desc.display.src_rect = drm::scene::Rect{0, 0, fb_w, fb_h};
  bg_desc.display.dst_rect = drm::scene::Rect{0, 0, fb_w, fb_h};
  bg_desc.display.zpos = 0;  // Tegra-style affinity: 0 → PRIMARY
  if (auto r = scene->add_layer(std::move(bg_desc)); !r) {
    drm::println(stderr, "add_layer (Vulkan bg): {}", r.error().message());
    return EXIT_FAILURE;
  }

  // Instruments layer — CPU-painted ARGB on a dumb buffer. Centered
  // single dial covering the middle third of the screen.
  const std::uint32_t inst_w = std::min(fb_w, fb_h * 4U / 3U);
  const std::uint32_t inst_h = std::min(fb_h, inst_w);
  auto inst_source =
      drm::scene::DumbBufferSource::create(device, inst_w, inst_h, DRM_FORMAT_ARGB8888);
  if (!inst_source) {
    drm::println(stderr, "DumbBufferSource::create (instruments): {}",
                 inst_source.error().message());
    return EXIT_FAILURE;
  }
  auto inst_src = std::move(*inst_source);
  auto* inst_src_raw = inst_src.get();
  // Initial transparent fill.
  if (auto m = inst_src->map(drm::MapAccess::Write); m) {
    std::memset(m->pixels().data(), 0, m->pixels().size_bytes());
  }
  drm::scene::LayerDesc inst_desc;
  inst_desc.source = std::move(inst_src);
  inst_desc.display.src_rect = drm::scene::Rect{0, 0, inst_w, inst_h};
  const std::int32_t inst_x =
      static_cast<std::int32_t>((fb_w - inst_w) / 2U);
  const std::int32_t inst_y =
      static_cast<std::int32_t>((fb_h - inst_h) / 2U);
  inst_desc.display.dst_rect = drm::scene::Rect{inst_x, inst_y, inst_w, inst_h};
  inst_desc.display.zpos = 4;  // Tegra-style affinity: > 0 → OVERLAY
  if (auto r = scene->add_layer(std::move(inst_desc)); !r) {
    drm::println(stderr, "add_layer (instruments): {}", r.error().message());
    return EXIT_FAILURE;
  }

  // Vulkan command pool + one re-recordable command buffer.
  VkCommandPoolCreateInfo pci{};
  pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  pci.queueFamilyIndex = qf_index;
  pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  VkCommandPool cmd_pool = VK_NULL_HANDLE;
  vkCreateCommandPool(vk_device, &pci, nullptr, &cmd_pool);
  VkCommandBufferAllocateInfo cbai{};
  cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cbai.commandPool = cmd_pool;
  cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cbai.commandBufferCount = 1;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  vkAllocateCommandBuffers(vk_device, &cbai, &cmd);

  drm::PageFlip page_flip(device);
  bool flip_pending = false;
  page_flip.set_handler([&](std::uint32_t, std::uint64_t, std::uint64_t) {
    flip_pending = false;
  });

  // First commit: scene comes up with the (zero-initialized) Vulkan
  // image on PRIMARY and the (transparent) instruments dumb buffer on
  // OVERLAY. The visible result is plain black for one frame; the
  // first render loop iteration immediately overwrites it.
  if (auto r = scene->commit(DRM_MODE_PAGE_FLIP_EVENT, &page_flip); !r) {
    drm::println(stderr, "first commit: {}", r.error().message());
    return EXIT_FAILURE;
  }
  flip_pending = true;

  using clk = std::chrono::steady_clock;
  const auto t0 = clk::now();
  const auto deadline = t0 + std::chrono::seconds(args.seconds);
  std::uint64_t frames = 0;
  bool first = true;

  // Optional jitter capture, env-gated to match cluster_sim.
  const bool jitter_enabled = std::getenv("CLUSTER_SIM_FRAME_JITTER") != nullptr;
  auto last_flip = clk::now();
  double max_dt_us = 0.0;
  double sum_dt_us = 0.0;
  std::uint64_t jitter_samples = 0;

  while (clk::now() < deadline && !g_quit.load(std::memory_order_relaxed)) {
    // Wait for previous flip.
    while (flip_pending && !g_quit.load(std::memory_order_relaxed)) {
      (void)page_flip.dispatch(0);
    }
    if (g_quit.load(std::memory_order_relaxed)) {
      break;
    }
    if (jitter_enabled) {
      const auto now = clk::now();
      const double dt = std::chrono::duration<double, std::micro>(now - last_flip).count();
      if (jitter_samples > 0) {
        sum_dt_us += dt;
        if (dt > max_dt_us) {
          max_dt_us = dt;
        }
      }
      last_flip = now;
      ++jitter_samples;
    }

    const double t = std::chrono::duration<double>(clk::now() - t0).count();
    // Cluster mood lighting: low-saturation drift, like a real
    // dashboard's amber-blue ambient. Period is a few seconds.
    const auto bg_r = static_cast<float>(0.04 + (0.04 * std::sin(t * 0.6)));
    const auto bg_g = static_cast<float>(0.04 + (0.04 * std::sin((t * 0.8) + 2.0)));
    const auto bg_b = static_cast<float>(0.08 + (0.08 * std::sin((t * 1.0) + 4.0)));
    VkClearColorValue clear{};
    clear.float32[0] = bg_b;  // VK_FORMAT_B8G8R8A8_UNORM: BGRA byte order
    clear.float32[1] = bg_g;
    clear.float32[2] = bg_r;
    clear.float32[3] = 1.0F;

    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo cbi{};
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &cbi);
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = first ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = vk_image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.srcAccessMask = 0;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &b);
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, vk_image, VK_IMAGE_LAYOUT_GENERAL, &clear, 1, &range);
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);
    first = false;

    // CPU-side instruments paint. Animation phase mirrors cluster_sim's
    // speedo cosine sweep.
    const double speedo_norm = dial_norm_from_phase(t / k_speedo_period_s);
    if (auto m = inst_src_raw->map(drm::MapAccess::Write); m) {
      paint_instruments(*m, inst_w, inst_h, speedo_norm);
    }

    if (auto r = scene->commit(DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_NONBLOCK, &page_flip);
        !r) {
      drm::println(stderr, "commit: {}", r.error().message());
      break;
    }
    flip_pending = true;
    ++frames;
  }
  drm::println(stderr, "cluster_sim_vulkan: {} frames in {}s", frames, args.seconds);
  if (jitter_enabled && jitter_samples > 1) {
    const double mean = sum_dt_us / static_cast<double>(jitter_samples - 1);
    drm::println(stderr, "frame-jitter: frames={} mean={:.1f}us max={:.1f}us", jitter_samples,
                 mean, max_dt_us);
  }

  // Wait for any pending flip before tearing down.
  while (flip_pending) {
    page_flip.dispatch(0);
  }

  scene.reset();
  vkDestroyCommandPool(vk_device, cmd_pool, nullptr);
  vkFreeMemory(vk_device, vk_mem, nullptr);
  vkDestroyImage(vk_device, vk_image, nullptr);
  vkDestroyDevice(vk_device, nullptr);
  vkDestroyInstance(instance, nullptr);
  return EXIT_SUCCESS;
}

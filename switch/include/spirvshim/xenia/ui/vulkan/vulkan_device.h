// Stand-in for xenia/ui/vulkan/vulkan_device.h: the OpenGL path uses Xenia's SpirvShaderTranslator
// without a Vulkan device (SpirvShaderTranslator::Features(bool) is set by hand). Only the members
// that spirv_shader_translator.cc reads in Features(const VulkanDevice*) exist, so that constructor
// still compiles; it is never called.
#pragma once
#include <cstdint>

#ifndef VK_MAKE_API_VERSION
#define VK_MAKE_API_VERSION(variant, major, minor, patch)                       \
  ((uint32_t(variant) << 29) | (uint32_t(major) << 22) | (uint32_t(minor) << 12) | \
   uint32_t(patch))
#endif

namespace xe::ui::vulkan {
class VulkanDevice {
 public:
  struct Properties {
    uint32_t apiVersion = 0;
    uint32_t maxStorageBufferRange = 0;
    bool fullDrawIndexUint32 = false;
    bool vertexPipelineStoresAndAtomics = false;
    bool fragmentStoresAndAtomics = false;
    bool shaderClipDistance = false;
    bool shaderCullDistance = false;
    bool imageViewFormatSwizzle = false;
    bool shaderSignedZeroInfNanPreserveFloat32 = false;
    bool shaderDenormFlushToZeroFloat32 = false;
    bool shaderRoundingModeRTEFloat32 = false;
    bool fragmentShaderSampleInterlock = false;
    bool shaderDemoteToHelperInvocation = false;
  };
  struct Extensions {
    bool ext_1_2_KHR_spirv_1_4 = false;
  };
  const Properties& properties() const { return properties_; }
  const Extensions& extensions() const { return extensions_; }

 private:
  Properties properties_;
  Extensions extensions_;
};
}  // namespace xe::ui::vulkan

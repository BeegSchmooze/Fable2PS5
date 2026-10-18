#pragma once
#include <vulkan/vulkan.h>
#include <vector>
#include <cstdio>
static VkResult CreatePs5DisplaySurface(VkInstance instance,VkPhysicalDevice physical,PFN_vkGetInstanceProcAddr get,uint32_t width,uint32_t height,VkSurfaceKHR* surface){
#define DISPLAY_PROC(name) auto name=reinterpret_cast<PFN_##name>(get(instance,#name));if(!name)return VK_ERROR_EXTENSION_NOT_PRESENT;
 DISPLAY_PROC(vkGetPhysicalDeviceDisplayPropertiesKHR)
 DISPLAY_PROC(vkGetPhysicalDeviceDisplayPlanePropertiesKHR)
 DISPLAY_PROC(vkGetDisplayModePropertiesKHR)
 DISPLAY_PROC(vkGetDisplayPlaneSupportedDisplaysKHR)
 DISPLAY_PROC(vkGetDisplayPlaneCapabilitiesKHR)
 DISPLAY_PROC(vkCreateDisplayPlaneSurfaceKHR)
#undef DISPLAY_PROC
 uint32_t count=0;VkResult status=vkGetPhysicalDeviceDisplayPropertiesKHR(physical,&count,nullptr);
 if(status!=VK_SUCCESS||!count)return status==VK_SUCCESS?VK_ERROR_INITIALIZATION_FAILED:status;
 std::vector<VkDisplayPropertiesKHR> displays(count);status=vkGetPhysicalDeviceDisplayPropertiesKHR(physical,&count,displays.data());if(status!=VK_SUCCESS)return status;
 uint32_t plane_count=0;status=vkGetPhysicalDeviceDisplayPlanePropertiesKHR(physical,&plane_count,nullptr);if(status!=VK_SUCCESS||!plane_count)return VK_ERROR_INITIALIZATION_FAILED;
 std::vector<VkDisplayPlanePropertiesKHR> planes(plane_count);status=vkGetPhysicalDeviceDisplayPlanePropertiesKHR(physical,&plane_count,planes.data());if(status!=VK_SUCCESS)return status;
 VkDisplayKHR chosen_display=VK_NULL_HANDLE;VkDisplayModePropertiesKHR chosen_mode{};
 for(const auto& display:displays){
  uint32_t mode_count=0;status=vkGetDisplayModePropertiesKHR(physical,display.display,&mode_count,nullptr);if(status!=VK_SUCCESS||!mode_count)continue;
  std::vector<VkDisplayModePropertiesKHR> modes(mode_count);status=vkGetDisplayModePropertiesKHR(physical,display.display,&mode_count,modes.data());if(status!=VK_SUCCESS)continue;
  for(const auto& mode:modes){
   bool exact=mode.parameters.visibleRegion.width==width&&mode.parameters.visibleRegion.height==height;
   if(chosen_display==VK_NULL_HANDLE||exact){chosen_display=display.display;chosen_mode=mode;}
   if(exact)break;
  }
  if(chosen_mode.parameters.visibleRegion.width==width&&chosen_mode.parameters.visibleRegion.height==height)break;
 }
 if(chosen_display==VK_NULL_HANDLE)return VK_ERROR_INITIALIZATION_FAILED;
 for(uint32_t plane=0;plane<plane_count;++plane){
  uint32_t supported_count=0;if(vkGetDisplayPlaneSupportedDisplaysKHR(physical,plane,&supported_count,nullptr)!=VK_SUCCESS||!supported_count)continue;
  std::vector<VkDisplayKHR> supported(supported_count);if(vkGetDisplayPlaneSupportedDisplaysKHR(physical,plane,&supported_count,supported.data())!=VK_SUCCESS)continue;
  bool found=false;for(auto d:supported)found|=d==chosen_display;if(!found)continue;
  VkDisplayPlaneCapabilitiesKHR caps{};if(vkGetDisplayPlaneCapabilitiesKHR(physical,chosen_mode.displayMode,plane,&caps)!=VK_SUCCESS)continue;
  VkDisplayPlaneAlphaFlagBitsKHR alpha=VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR;
  if(!(caps.supportedAlpha&alpha)){if(caps.supportedAlpha&VK_DISPLAY_PLANE_ALPHA_GLOBAL_BIT_KHR)alpha=VK_DISPLAY_PLANE_ALPHA_GLOBAL_BIT_KHR;else if(caps.supportedAlpha&VK_DISPLAY_PLANE_ALPHA_PER_PIXEL_BIT_KHR)alpha=VK_DISPLAY_PLANE_ALPHA_PER_PIXEL_BIT_KHR;else if(caps.supportedAlpha&VK_DISPLAY_PLANE_ALPHA_PER_PIXEL_PREMULTIPLIED_BIT_KHR)alpha=VK_DISPLAY_PLANE_ALPHA_PER_PIXEL_PREMULTIPLIED_BIT_KHR;else continue;}
  VkDisplaySurfaceCreateInfoKHR info{VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR};info.displayMode=chosen_mode.displayMode;info.planeIndex=plane;info.planeStackIndex=planes[plane].currentStackIndex;info.transform=VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;info.globalAlpha=1.0f;info.alphaMode=alpha;info.imageExtent=chosen_mode.parameters.visibleRegion;
  status=vkCreateDisplayPlaneSurfaceKHR(instance,&info,nullptr,surface);
  std::fprintf(stderr,"[FABLE2-DISPLAY] SURFACE_RESULT=%d EXTENT=%ux%u PLANE=%u\n",int(status),info.imageExtent.width,info.imageExtent.height,plane);std::fflush(stderr);
  if(status==VK_SUCCESS)return status;
 }
 return VK_ERROR_INITIALIZATION_FAILED;
}


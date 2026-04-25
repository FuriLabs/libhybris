/*
 * Copyright (c) 2022 Jolla Ltd.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

/* For RTLD_DEFAULT */
#define _GNU_SOURCE

#define VK_USE_PLATFORM_ANDROID_KHR 1
#define VK_USE_PLATFORM_WAYLAND_KHR 1

#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

#ifdef WANT_VULKAN_X11_STUBS
#include <X11/Xlib.h>
#include <xcb/xcb.h>
#include <X11/extensions/Xrandr.h>
#endif

#include <hardware/hardware.h>

#include <hybris/common/binding.h>
#include <hybris/common/floating_point_abi.h>
#include "config.h"
#include "logging.h"
#include "ws.h"

/*
 * Embedded hardware/hwvulkan.h content.
 * Keep it here so this file does not need <hardware/hwvulkan.h>.
 */
#define HWVULKAN_HARDWARE_MODULE_ID "vulkan"
#define HWVULKAN_MODULE_API_VERSION_0_1 HARDWARE_MODULE_API_VERSION(0, 1)
#define HWVULKAN_DEVICE_API_VERSION_0_1 HARDWARE_DEVICE_API_VERSION_2(0, 1, 0)
#define HWVULKAN_DEVICE_0 "vk0"
#define HWVULKAN_DISPATCH_MAGIC 0x01CDC0DE

#ifndef VK_ANDROID_NATIVE_BUFFER_EXTENSION_NAME
#define VK_ANDROID_NATIVE_BUFFER_EXTENSION_NAME "VK_ANDROID_native_buffer"
#endif


typedef union {
    uintptr_t magic;
    const void* vtbl;
} hwvulkan_dispatch_t;

typedef struct hwvulkan_module_t {
    struct hw_module_t common;
} hwvulkan_module_t;

typedef struct hwvulkan_device_t {
    struct hw_device_t common;

    PFN_vkEnumerateInstanceExtensionProperties EnumerateInstanceExtensionProperties;
    PFN_vkCreateInstance CreateInstance;
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
} hwvulkan_device_t;

static hw_module_t *vulkan_hardware_module = NULL;
static hwvulkan_device_t *vulkan_hal_device = NULL;
static VkInstance vulkan_instance = VK_NULL_HANDLE;

#define SUPPORTED_LOADER_ICD_INTERFACE_VERSION 5

VKAPI_ATTR VkResult VKAPI_CALL vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t* pSupportedVersion);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char* pName);
PFN_vkVoidFunction vkGetInstanceProcAddr(VkInstance instance, const char* pName);
#ifdef WANT_WAYLAND
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex, VkSurfaceKHR surface, VkBool32 *pSupported);
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, VkSurfaceCapabilitiesKHR *pSurfaceCapabilities);
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, uint32_t *pSurfaceFormatCount, VkSurfaceFormatKHR *pSurfaceFormats);
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, uint32_t *pPresentModeCount, VkPresentModeKHR *pPresentModes);
VKAPI_ATTR VkResult VKAPI_CALL vkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkSwapchainKHR *pSwapchain);
VKAPI_ATTR void VKAPI_CALL vkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks *pAllocator);
VKAPI_ATTR VkResult VKAPI_CALL vkGetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapchain, uint32_t *pSwapchainImageCount, VkImage *pSwapchainImages);
VKAPI_ATTR VkResult VKAPI_CALL vkAcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore semaphore, VkFence fence, uint32_t *pImageIndex);
VKAPI_ATTR VkResult VKAPI_CALL vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pPresentInfo);
#endif

static int hybris_vulkan_hal_initialize(void)
{
    if (vulkan_hal_device)
        return 0;

    if (hw_get_module(HWVULKAN_HARDWARE_MODULE_ID,
                      (const struct hw_module_t **)&vulkan_hardware_module) != 0) {
        fprintf(stderr, "failed to find/load Vulkan HAL module\n");
        return -1;
    }

    if (!vulkan_hardware_module ||
        !vulkan_hardware_module->methods ||
        !vulkan_hardware_module->methods->open) {
        fprintf(stderr, "invalid Vulkan HAL module\n");
        return -1;
    }

    if (vulkan_hardware_module->methods->open(
            vulkan_hardware_module,
            HWVULKAN_DEVICE_0,
            (struct hw_device_t **)&vulkan_hal_device) != 0) {
        fprintf(stderr, "failed to open Vulkan HAL device\n");
        vulkan_hal_device = NULL;
        return -1;
    }

    if (!vulkan_hal_device ||
        !vulkan_hal_device->EnumerateInstanceExtensionProperties ||
        !vulkan_hal_device->CreateInstance ||
        !vulkan_hal_device->GetInstanceProcAddr) {
        fprintf(stderr, "Vulkan HAL device is missing required functions\n");
        return -1;
    }

    return 0;
}

static void hybris_vulkan_hal_deinitialize(void)
{
    if (vulkan_hal_device) {
        if (vulkan_hal_device->common.close)
            vulkan_hal_device->common.close(&vulkan_hal_device->common);
        vulkan_hal_device = NULL;
    }

#ifndef ANDROID_BUILD
    if (vulkan_hardware_module && vulkan_hardware_module->dso)
        android_dlclose(vulkan_hardware_module->dso);
#else
    if (vulkan_hardware_module && vulkan_hardware_module->dso)
        dlclose(vulkan_hardware_module->dso);
#endif

    vulkan_hardware_module = NULL;
}

static void _init_androidvulkan(void)
{
    hybris_vulkan_hal_initialize();
}

static inline void hybris_vulkan_initialize(void)
{
    _init_androidvulkan();
}

static void *_android_vulkan_dlsym(const char *symbol)
{
    if (!vulkan_hal_device) {
        if (hybris_vulkan_hal_initialize() != 0)
            return NULL;
    }

    if (!symbol)
        return NULL;

    if (!strcmp(symbol, "vkCreateInstance"))
        return (void *)vulkan_hal_device->CreateInstance;

    if (!strcmp(symbol, "vkEnumerateInstanceExtensionProperties"))
        return (void *)vulkan_hal_device->EnumerateInstanceExtensionProperties;

    if (!strcmp(symbol, "vkGetInstanceProcAddr"))
        return (void *)vulkan_hal_device->GetInstanceProcAddr;

    return (void *)vulkan_hal_device->GetInstanceProcAddr(vulkan_instance, symbol);
}

VKAPI_ATTR VkResult VKAPI_CALL vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t* pSupportedVersion)
{
    if (*pSupportedVersion > SUPPORTED_LOADER_ICD_INTERFACE_VERSION) {
        *pSupportedVersion = SUPPORTED_LOADER_ICD_INTERFACE_VERSION;
    }
    return VK_SUCCESS;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char* pName)
{
    if (!pName)
        return NULL;

    if (!strcmp(pName, "vk_icdNegotiateLoaderICDInterfaceVersion")) {
        return (PFN_vkVoidFunction)vk_icdNegotiateLoaderICDInterfaceVersion;
    }
    if (!strcmp(pName, "vk_icdGetInstanceProcAddr")) {
        return (PFN_vkVoidFunction)vk_icdGetInstanceProcAddr;
    }
    return vkGetInstanceProcAddr(instance, pName);
}

struct ws_vulkan_interface hybris_vulkan_interface = {
    _android_vulkan_dlsym,
};

static PFN_vkVoidFunction (*_vkGetInstanceProcAddr)(VkInstance instance, const char* pName) = NULL;

VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkInstance* pInstance)
{
    VkResult result;

    if (_vkGetInstanceProcAddr == NULL) {
        if (!vulkan_hal_device && hybris_vulkan_hal_initialize() != 0)
            return VK_ERROR_INITIALIZATION_FAILED;
        _vkGetInstanceProcAddr = vulkan_hal_device->GetInstanceProcAddr;
    }
    ws_vkSetInstanceProcAddrFunc((PFN_vkVoidFunction)_vkGetInstanceProcAddr);

    result = ws_vkCreateInstance(pCreateInfo, pAllocator, pInstance);
    if (result == VK_SUCCESS)
        vulkan_instance = *pInstance;

    return result;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyInstance( VkInstance  instance, const VkAllocationCallbacks * pAllocator)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkInstance ,const VkAllocationCallbacks *))
        _android_vulkan_dlsym("vkDestroyInstance"))
            (instance, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumeratePhysicalDevices( VkInstance  instance,  uint32_t * pPhysicalDeviceCount,  VkPhysicalDevice * pPhysicalDevices)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkInstance , uint32_t *, VkPhysicalDevice *))
        _android_vulkan_dlsym("vkEnumeratePhysicalDevices"))
            (instance, pPhysicalDeviceCount, pPhysicalDevices);
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures( VkPhysicalDevice  physicalDevice,  VkPhysicalDeviceFeatures * pFeatures)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkPhysicalDevice , VkPhysicalDeviceFeatures *))
        _android_vulkan_dlsym("vkGetPhysicalDeviceFeatures"))
            (physicalDevice, pFeatures);
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFormatProperties( VkPhysicalDevice  physicalDevice,  VkFormat  format,  VkFormatProperties * pFormatProperties)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkPhysicalDevice , VkFormat , VkFormatProperties *))
        _android_vulkan_dlsym("vkGetPhysicalDeviceFormatProperties"))
            (physicalDevice, format, pFormatProperties);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceImageFormatProperties( VkPhysicalDevice  physicalDevice,  VkFormat  format,  VkImageType  type,  VkImageTiling  tiling,  VkImageUsageFlags  usage,  VkImageCreateFlags  flags,  VkImageFormatProperties * pImageFormatProperties)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkPhysicalDevice , VkFormat , VkImageType , VkImageTiling , VkImageUsageFlags , VkImageCreateFlags , VkImageFormatProperties *))
        _android_vulkan_dlsym("vkGetPhysicalDeviceImageFormatProperties"))
            (physicalDevice, format, type, tiling, usage, flags, pImageFormatProperties);
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties( VkPhysicalDevice  physicalDevice,  VkPhysicalDeviceProperties * pProperties)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkPhysicalDevice , VkPhysicalDeviceProperties *))
        _android_vulkan_dlsym("vkGetPhysicalDeviceProperties"))
            (physicalDevice, pProperties);
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceQueueFamilyProperties( VkPhysicalDevice  physicalDevice,  uint32_t * pQueueFamilyPropertyCount,  VkQueueFamilyProperties * pQueueFamilyProperties)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkPhysicalDevice , uint32_t *, VkQueueFamilyProperties *))
        _android_vulkan_dlsym("vkGetPhysicalDeviceQueueFamilyProperties"))
            (physicalDevice, pQueueFamilyPropertyCount, pQueueFamilyProperties);
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties( VkPhysicalDevice  physicalDevice,  VkPhysicalDeviceMemoryProperties * pMemoryProperties)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkPhysicalDevice , VkPhysicalDeviceMemoryProperties *))
        _android_vulkan_dlsym("vkGetPhysicalDeviceMemoryProperties"))
            (physicalDevice, pMemoryProperties);
}

VkResult vkEnumerateInstanceExtensionProperties(const char* pLayerName, uint32_t* pPropertyCount, VkExtensionProperties* pProperties)
{
    if (_vkGetInstanceProcAddr == NULL) {
        if (!vulkan_hal_device && hybris_vulkan_hal_initialize() != 0)
            return VK_ERROR_INITIALIZATION_FAILED;
        _vkGetInstanceProcAddr = vulkan_hal_device->GetInstanceProcAddr;
    }
    ws_vkSetInstanceProcAddrFunc((PFN_vkVoidFunction)_vkGetInstanceProcAddr);

    return ws_vkEnumerateInstanceExtensionProperties(pLayerName, pPropertyCount, pProperties);
}

#ifdef WANT_WAYLAND
VkResult vkCreateWaylandSurfaceKHR(VkInstance instance,
        const VkWaylandSurfaceCreateInfoKHR* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkSurfaceKHR* pSurface)
{
    return ws_vkCreateWaylandSurfaceKHR(instance, pCreateInfo, pAllocator, pSurface);
}

VkBool32 vkGetPhysicalDeviceWaylandPresentationSupportKHR(VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex, struct wl_display* display)
{
    return ws_vkGetPhysicalDeviceWaylandPresentationSupportKHR(physicalDevice, queueFamilyIndex, display);
}

void vkDestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface, const VkAllocationCallbacks* pAllocator)
{
    ws_vkDestroySurfaceKHR(instance, surface, pAllocator);
}
#else
VKAPI_ATTR void VKAPI_CALL vkDestroySurfaceKHR( VkInstance  instance,  VkSurfaceKHR  surface, const VkAllocationCallbacks * pAllocator)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkInstance , VkSurfaceKHR ,const VkAllocationCallbacks *))
        _android_vulkan_dlsym("vkDestroySurfaceKHR"))
            (instance, surface, pAllocator);
}
#endif

PFN_vkVoidFunction vkGetInstanceProcAddr(VkInstance instance, const char* pName)
{
    if (_vkGetInstanceProcAddr == NULL) {
        if (!vulkan_hal_device && hybris_vulkan_hal_initialize() != 0)
            return NULL;
        _vkGetInstanceProcAddr = vulkan_hal_device->GetInstanceProcAddr;
    }

    if (!strcmp(pName, "vkEnumerateInstanceExtensionProperties")) {
        return (PFN_vkVoidFunction)vkEnumerateInstanceExtensionProperties;
    } else if (!strcmp(pName, "vkCreateInstance")) {
        return (PFN_vkVoidFunction)vkCreateInstance;
    } else if (!strcmp(pName, "vkGetInstanceProcAddr")) {
        return (PFN_vkVoidFunction)vkGetInstanceProcAddr;
    } else if (!strcmp(pName, "vkCreateDevice")) {
        return (PFN_vkVoidFunction)vkCreateDevice;
    } else if (!strcmp(pName, "vkEnumerateDeviceExtensionProperties")) {
        return (PFN_vkVoidFunction)vkEnumerateDeviceExtensionProperties;
#ifdef WANT_WAYLAND
    } else if (!strcmp(pName, "vkCreateWaylandSurfaceKHR")) {
        return (PFN_vkVoidFunction)vkCreateWaylandSurfaceKHR;
    } else if (!strcmp(pName, "vkGetPhysicalDeviceWaylandPresentationSupportKHR")) {
        return (PFN_vkVoidFunction)vkGetPhysicalDeviceWaylandPresentationSupportKHR;
    } else if (!strcmp(pName, "vkDestroySurfaceKHR")) {
        return (PFN_vkVoidFunction)vkDestroySurfaceKHR;
    } else if (!strcmp(pName, "vkGetPhysicalDeviceSurfaceSupportKHR")) {
        return (PFN_vkVoidFunction)vkGetPhysicalDeviceSurfaceSupportKHR;
    } else if (!strcmp(pName, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR")) {
        return (PFN_vkVoidFunction)vkGetPhysicalDeviceSurfaceCapabilitiesKHR;
    } else if (!strcmp(pName, "vkGetPhysicalDeviceSurfaceFormatsKHR")) {
        return (PFN_vkVoidFunction)vkGetPhysicalDeviceSurfaceFormatsKHR;
    } else if (!strcmp(pName, "vkGetPhysicalDeviceSurfacePresentModesKHR")) {
        return (PFN_vkVoidFunction)vkGetPhysicalDeviceSurfacePresentModesKHR;
    } else if (!strcmp(pName, "vkCreateSwapchainKHR")) {
        return (PFN_vkVoidFunction)vkCreateSwapchainKHR;
#endif
    } else if (!strcmp(pName, "vk_icdNegotiateLoaderICDInterfaceVersion")) {
        return (PFN_vkVoidFunction)vk_icdNegotiateLoaderICDInterfaceVersion;
    } else if (!strcmp(pName, "vk_icdGetInstanceProcAddr")) {
        return (PFN_vkVoidFunction)vk_icdGetInstanceProcAddr;
    }

    return (*_vkGetInstanceProcAddr)(instance, pName);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr( VkDevice  device, const char * pName)
{
#ifdef WANT_WAYLAND
    if (!strcmp(pName, "vkCreateSwapchainKHR")) {
        return (PFN_vkVoidFunction)vkCreateSwapchainKHR;
    } else if (!strcmp(pName, "vkDestroySwapchainKHR")) {
        return (PFN_vkVoidFunction)vkDestroySwapchainKHR;
    } else if (!strcmp(pName, "vkGetSwapchainImagesKHR")) {
        return (PFN_vkVoidFunction)vkGetSwapchainImagesKHR;
    } else if (!strcmp(pName, "vkAcquireNextImageKHR")) {
        return (PFN_vkVoidFunction)vkAcquireNextImageKHR;
    } else if (!strcmp(pName, "vkQueuePresentKHR")) {
        return (PFN_vkVoidFunction)vkQueuePresentKHR;
    }
#endif

    if (!vulkan_hal_device) _init_androidvulkan();

    return ((PFN_vkVoidFunction (*)( VkDevice  ,const char * ))
        _android_vulkan_dlsym("vkGetDeviceProcAddr"))
            (device, pName);
}



static int hybris_vulkan_extension_name_present(const char *name,
                                                uint32_t count,
                                                VkExtensionProperties *props)
{
    uint32_t i;
    if (!name || !props)
        return 0;

    for (i = 0; i < count; i++) {
        if (!strcmp(props[i].extensionName, name))
            return 1;
    }

    return 0;
}

static VkResult hybris_vulkan_map_device_extensions_to_android_native_buffer(
        const VkDeviceCreateInfo *in_info,
        VkDeviceCreateInfo *out_info,
        const char ***owned_names_out)
{
    uint32_t i;
    const char **names;

    *out_info = *in_info;
    *owned_names_out = NULL;

    if (!in_info->enabledExtensionCount || !in_info->ppEnabledExtensionNames)
        return VK_SUCCESS;

    names = (const char **)calloc(in_info->enabledExtensionCount, sizeof(char *));
    if (!names)
        return VK_ERROR_OUT_OF_HOST_MEMORY;

    for (i = 0; i < in_info->enabledExtensionCount; i++) {
        const char *name = in_info->ppEnabledExtensionNames[i];

        if (!strcmp(name, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
            names[i] = VK_ANDROID_NATIVE_BUFFER_EXTENSION_NAME;
        else
            names[i] = name;
    }

    out_info->ppEnabledExtensionNames = names;
    *owned_names_out = names;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice( VkPhysicalDevice  physicalDevice, const VkDeviceCreateInfo * pCreateInfo, const VkAllocationCallbacks * pAllocator,  VkDevice * pDevice)
{
    VkResult result;
    VkDeviceCreateInfo createInfo;
    const char **ownedExtensionNames = NULL;
    PFN_vkCreateDevice pfnCreateDevice;

    if (!vulkan_hal_device) _init_androidvulkan();

    result = hybris_vulkan_map_device_extensions_to_android_native_buffer(
        pCreateInfo, &createInfo, &ownedExtensionNames);
    if (result != VK_SUCCESS)
        return result;

    pfnCreateDevice = (PFN_vkCreateDevice)_android_vulkan_dlsym("vkCreateDevice");
    if (!pfnCreateDevice) {
        free((void *)ownedExtensionNames);
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    result = pfnCreateDevice(physicalDevice, &createInfo, pAllocator, pDevice);

    free((void *)ownedExtensionNames);
    return result;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyDevice( VkDevice  device, const VkAllocationCallbacks * pAllocator)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  ,const VkAllocationCallbacks * ))
        _android_vulkan_dlsym("vkDestroyDevice"))
            (device, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties( VkPhysicalDevice  physicalDevice, const char * pLayerName,  uint32_t * pPropertyCount,  VkExtensionProperties * pProperties)
{
    VkResult result;
    PFN_vkEnumerateDeviceExtensionProperties pfnEnumerateDeviceExtensionProperties;

    if (!vulkan_hal_device) _init_androidvulkan();

    pfnEnumerateDeviceExtensionProperties =
        (PFN_vkEnumerateDeviceExtensionProperties)_android_vulkan_dlsym("vkEnumerateDeviceExtensionProperties");
    if (!pfnEnumerateDeviceExtensionProperties)
        return VK_ERROR_INITIALIZATION_FAILED;

    result = pfnEnumerateDeviceExtensionProperties(
        physicalDevice, pLayerName, pPropertyCount, pProperties);

    /*
     * Android Vulkan HALs commonly expose VK_ANDROID_native_buffer instead of
     * VK_KHR_swapchain. Android libvulkan maps that extension for applications.
     * Since this file now talks to the HAL directly, do the same mapping here.
     */
    if (!pLayerName && pProperties && (result == VK_SUCCESS || result == VK_INCOMPLETE)) {
        uint32_t i;

        for (i = 0; i < *pPropertyCount; i++) {
            if (!strcmp(pProperties[i].extensionName, VK_ANDROID_NATIVE_BUFFER_EXTENSION_NAME)) {
                strncpy(pProperties[i].extensionName,
                        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
                        VK_MAX_EXTENSION_NAME_SIZE);
                pProperties[i].extensionName[VK_MAX_EXTENSION_NAME_SIZE - 1] = '\0';
                pProperties[i].specVersion = VK_KHR_SWAPCHAIN_SPEC_VERSION;
            }
        }
    }

    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceLayerProperties( uint32_t * pPropertyCount,  VkLayerProperties * pProperties)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( uint32_t * , VkLayerProperties * ))
        _android_vulkan_dlsym("vkEnumerateInstanceLayerProperties"))
            (pPropertyCount, pProperties);
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceLayerProperties( VkPhysicalDevice  physicalDevice,  uint32_t * pPropertyCount,  VkLayerProperties * pProperties)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkPhysicalDevice  , uint32_t * , VkLayerProperties * ))
        _android_vulkan_dlsym("vkEnumerateDeviceLayerProperties"))
            (physicalDevice, pPropertyCount, pProperties);
}

VKAPI_ATTR void VKAPI_CALL vkGetDeviceQueue( VkDevice  device,  uint32_t  queueFamilyIndex,  uint32_t  queueIndex,  VkQueue * pQueue)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  , uint32_t  , uint32_t  , VkQueue * ))
        _android_vulkan_dlsym("vkGetDeviceQueue"))
            (device, queueFamilyIndex, queueIndex, pQueue);
}

VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit( VkQueue  queue,  uint32_t  submitCount, const VkSubmitInfo * pSubmits,  VkFence  fence)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkQueue  , uint32_t  ,const VkSubmitInfo * , VkFence  ))
        _android_vulkan_dlsym("vkQueueSubmit"))
            (queue, submitCount, pSubmits, fence);
}

VKAPI_ATTR VkResult VKAPI_CALL vkQueueWaitIdle( VkQueue  queue)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkQueue  ))
        _android_vulkan_dlsym("vkQueueWaitIdle"))
            (queue);
}

VKAPI_ATTR VkResult VKAPI_CALL vkDeviceWaitIdle( VkDevice  device)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  ))
        _android_vulkan_dlsym("vkDeviceWaitIdle"))
            (device);
}

VKAPI_ATTR VkResult VKAPI_CALL vkAllocateMemory( VkDevice  device, const VkMemoryAllocateInfo * pAllocateInfo, const VkAllocationCallbacks * pAllocator,  VkDeviceMemory * pMemory)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  ,const VkMemoryAllocateInfo * ,const VkAllocationCallbacks * , VkDeviceMemory * ))
        _android_vulkan_dlsym("vkAllocateMemory"))
            (device, pAllocateInfo, pAllocator, pMemory);
}

VKAPI_ATTR void VKAPI_CALL vkFreeMemory( VkDevice  device,  VkDeviceMemory  memory, const VkAllocationCallbacks * pAllocator)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  , VkDeviceMemory  ,const VkAllocationCallbacks * ))
        _android_vulkan_dlsym("vkFreeMemory"))
            (device, memory, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL vkMapMemory( VkDevice  device,  VkDeviceMemory  memory,  VkDeviceSize  offset,  VkDeviceSize  size,  VkMemoryMapFlags  flags,  void ** ppData)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , VkDeviceMemory  , VkDeviceSize  , VkDeviceSize  , VkMemoryMapFlags  , void ** ))
        _android_vulkan_dlsym("vkMapMemory"))
            (device, memory, offset, size, flags, ppData);
}

VKAPI_ATTR void VKAPI_CALL vkUnmapMemory( VkDevice  device,  VkDeviceMemory  memory)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  , VkDeviceMemory  ))
        _android_vulkan_dlsym("vkUnmapMemory"))
            (device, memory);
}

VKAPI_ATTR VkResult VKAPI_CALL vkFlushMappedMemoryRanges( VkDevice  device,  uint32_t  memoryRangeCount, const VkMappedMemoryRange * pMemoryRanges)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , uint32_t  ,const VkMappedMemoryRange * ))
        _android_vulkan_dlsym("vkFlushMappedMemoryRanges"))
            (device, memoryRangeCount, pMemoryRanges);
}

VKAPI_ATTR VkResult VKAPI_CALL vkInvalidateMappedMemoryRanges( VkDevice  device,  uint32_t  memoryRangeCount, const VkMappedMemoryRange * pMemoryRanges)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , uint32_t  ,const VkMappedMemoryRange * ))
        _android_vulkan_dlsym("vkInvalidateMappedMemoryRanges"))
            (device, memoryRangeCount, pMemoryRanges);
}

VKAPI_ATTR void VKAPI_CALL vkGetDeviceMemoryCommitment( VkDevice  device,  VkDeviceMemory  memory,  VkDeviceSize * pCommittedMemoryInBytes)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  , VkDeviceMemory  , VkDeviceSize * ))
        _android_vulkan_dlsym("vkGetDeviceMemoryCommitment"))
            (device, memory, pCommittedMemoryInBytes);
}

VKAPI_ATTR VkResult VKAPI_CALL vkBindBufferMemory( VkDevice  device,  VkBuffer  buffer,  VkDeviceMemory  memory,  VkDeviceSize  memoryOffset)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , VkBuffer  , VkDeviceMemory  , VkDeviceSize  ))
        _android_vulkan_dlsym("vkBindBufferMemory"))
            (device, buffer, memory, memoryOffset);
}

VKAPI_ATTR VkResult VKAPI_CALL vkBindImageMemory( VkDevice  device,  VkImage  image,  VkDeviceMemory  memory,  VkDeviceSize  memoryOffset)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , VkImage  , VkDeviceMemory  , VkDeviceSize  ))
        _android_vulkan_dlsym("vkBindImageMemory"))
            (device, image, memory, memoryOffset);
}

VKAPI_ATTR void VKAPI_CALL vkGetBufferMemoryRequirements( VkDevice  device,  VkBuffer  buffer,  VkMemoryRequirements * pMemoryRequirements)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  , VkBuffer  , VkMemoryRequirements * ))
        _android_vulkan_dlsym("vkGetBufferMemoryRequirements"))
            (device, buffer, pMemoryRequirements);
}

VKAPI_ATTR void VKAPI_CALL vkGetImageMemoryRequirements( VkDevice  device,  VkImage  image,  VkMemoryRequirements * pMemoryRequirements)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  , VkImage  , VkMemoryRequirements * ))
        _android_vulkan_dlsym("vkGetImageMemoryRequirements"))
            (device, image, pMemoryRequirements);
}

VKAPI_ATTR void VKAPI_CALL vkGetImageSparseMemoryRequirements( VkDevice  device,  VkImage  image,  uint32_t * pSparseMemoryRequirementCount,  VkSparseImageMemoryRequirements * pSparseMemoryRequirements)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  , VkImage  , uint32_t * , VkSparseImageMemoryRequirements * ))
        _android_vulkan_dlsym("vkGetImageSparseMemoryRequirements"))
            (device, image, pSparseMemoryRequirementCount, pSparseMemoryRequirements);
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceSparseImageFormatProperties( VkPhysicalDevice  physicalDevice,  VkFormat  format,  VkImageType  type,  VkSampleCountFlagBits  samples,  VkImageUsageFlags  usage,  VkImageTiling  tiling,  uint32_t * pPropertyCount,  VkSparseImageFormatProperties * pProperties)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkPhysicalDevice  , VkFormat  , VkImageType  , VkSampleCountFlagBits  , VkImageUsageFlags  , VkImageTiling  , uint32_t * , VkSparseImageFormatProperties * ))
        _android_vulkan_dlsym("vkGetPhysicalDeviceSparseImageFormatProperties"))
            (physicalDevice, format, type, samples, usage, tiling, pPropertyCount, pProperties);
}

VKAPI_ATTR VkResult VKAPI_CALL vkQueueBindSparse( VkQueue  queue,  uint32_t  bindInfoCount, const VkBindSparseInfo * pBindInfo,  VkFence  fence)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkQueue  , uint32_t  ,const VkBindSparseInfo * , VkFence  ))
        _android_vulkan_dlsym("vkQueueBindSparse"))
            (queue, bindInfoCount, pBindInfo, fence);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateFence( VkDevice  device, const VkFenceCreateInfo * pCreateInfo, const VkAllocationCallbacks * pAllocator,  VkFence * pFence)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  ,const VkFenceCreateInfo * ,const VkAllocationCallbacks * , VkFence * ))
        _android_vulkan_dlsym("vkCreateFence"))
            (device, pCreateInfo, pAllocator, pFence);
}

VKAPI_ATTR void VKAPI_CALL vkDestroyFence( VkDevice  device,  VkFence  fence, const VkAllocationCallbacks * pAllocator)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  , VkFence  ,const VkAllocationCallbacks * ))
        _android_vulkan_dlsym("vkDestroyFence"))
            (device, fence, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL vkResetFences( VkDevice  device,  uint32_t  fenceCount, const VkFence * pFences)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , uint32_t  ,const VkFence * ))
        _android_vulkan_dlsym("vkResetFences"))
            (device, fenceCount, pFences);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetFenceStatus( VkDevice  device,  VkFence  fence)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , VkFence  ))
        _android_vulkan_dlsym("vkGetFenceStatus"))
            (device, fence);
}

VKAPI_ATTR VkResult VKAPI_CALL vkWaitForFences( VkDevice  device,  uint32_t  fenceCount, const VkFence * pFences,  VkBool32  waitAll,  uint64_t  timeout)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , uint32_t  ,const VkFence * , VkBool32  , uint64_t  ))
        _android_vulkan_dlsym("vkWaitForFences"))
            (device, fenceCount, pFences, waitAll, timeout);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateSemaphore( VkDevice  device, const VkSemaphoreCreateInfo * pCreateInfo, const VkAllocationCallbacks * pAllocator,  VkSemaphore * pSemaphore)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  ,const VkSemaphoreCreateInfo * ,const VkAllocationCallbacks * , VkSemaphore * ))
        _android_vulkan_dlsym("vkCreateSemaphore"))
            (device, pCreateInfo, pAllocator, pSemaphore);
}

VKAPI_ATTR void VKAPI_CALL vkDestroySemaphore( VkDevice  device,  VkSemaphore  semaphore, const VkAllocationCallbacks * pAllocator)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  , VkSemaphore  ,const VkAllocationCallbacks * ))
        _android_vulkan_dlsym("vkDestroySemaphore"))
            (device, semaphore, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateEvent( VkDevice  device, const VkEventCreateInfo * pCreateInfo, const VkAllocationCallbacks * pAllocator,  VkEvent * pEvent)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  ,const VkEventCreateInfo * ,const VkAllocationCallbacks * , VkEvent * ))
        _android_vulkan_dlsym("vkCreateEvent"))
            (device, pCreateInfo, pAllocator, pEvent);
}

VKAPI_ATTR void VKAPI_CALL vkDestroyEvent( VkDevice  device,  VkEvent  event, const VkAllocationCallbacks * pAllocator)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  , VkEvent  ,const VkAllocationCallbacks * ))
        _android_vulkan_dlsym("vkDestroyEvent"))
            (device, event, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetEventStatus( VkDevice  device,  VkEvent  event)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , VkEvent  ))
        _android_vulkan_dlsym("vkGetEventStatus"))
            (device, event);
}

VKAPI_ATTR VkResult VKAPI_CALL vkSetEvent( VkDevice  device,  VkEvent  event)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , VkEvent  ))
        _android_vulkan_dlsym("vkSetEvent"))
            (device, event);
}

VKAPI_ATTR VkResult VKAPI_CALL vkResetEvent( VkDevice  device,  VkEvent  event)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , VkEvent  ))
        _android_vulkan_dlsym("vkResetEvent"))
            (device, event);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateQueryPool( VkDevice  device, const VkQueryPoolCreateInfo * pCreateInfo, const VkAllocationCallbacks * pAllocator,  VkQueryPool * pQueryPool)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  ,const VkQueryPoolCreateInfo * ,const VkAllocationCallbacks * , VkQueryPool * ))
        _android_vulkan_dlsym("vkCreateQueryPool"))
            (device, pCreateInfo, pAllocator, pQueryPool);
}

VKAPI_ATTR void VKAPI_CALL vkDestroyQueryPool( VkDevice  device,  VkQueryPool  queryPool, const VkAllocationCallbacks * pAllocator)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  , VkQueryPool  ,const VkAllocationCallbacks * ))
        _android_vulkan_dlsym("vkDestroyQueryPool"))
            (device, queryPool, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetQueryPoolResults( VkDevice  device,  VkQueryPool  queryPool,  uint32_t  firstQuery,  uint32_t  queryCount,  size_t  dataSize,  void * pData,  VkDeviceSize  stride,  VkQueryResultFlags  flags)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , VkQueryPool  , uint32_t  , uint32_t  , size_t  , void * , VkDeviceSize  , VkQueryResultFlags  ))
        _android_vulkan_dlsym("vkGetQueryPoolResults"))
            (device, queryPool, firstQuery, queryCount, dataSize, pData, stride, flags);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateBuffer( VkDevice  device, const VkBufferCreateInfo * pCreateInfo, const VkAllocationCallbacks * pAllocator,  VkBuffer * pBuffer)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  ,const VkBufferCreateInfo * ,const VkAllocationCallbacks * , VkBuffer * ))
        _android_vulkan_dlsym("vkCreateBuffer"))
            (device, pCreateInfo, pAllocator, pBuffer);
}

VKAPI_ATTR void VKAPI_CALL vkDestroyBuffer( VkDevice  device,  VkBuffer  buffer, const VkAllocationCallbacks * pAllocator)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  , VkBuffer  ,const VkAllocationCallbacks * ))
        _android_vulkan_dlsym("vkDestroyBuffer"))
            (device, buffer, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateBufferView( VkDevice  device, const VkBufferViewCreateInfo * pCreateInfo, const VkAllocationCallbacks * pAllocator,  VkBufferView * pView)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  ,const VkBufferViewCreateInfo * ,const VkAllocationCallbacks * , VkBufferView * ))
        _android_vulkan_dlsym("vkCreateBufferView"))
            (device, pCreateInfo, pAllocator, pView);
}

VKAPI_ATTR void VKAPI_CALL vkDestroyBufferView( VkDevice  device,  VkBufferView  bufferView, const VkAllocationCallbacks * pAllocator)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  , VkBufferView  ,const VkAllocationCallbacks * ))
        _android_vulkan_dlsym("vkDestroyBufferView"))
            (device, bufferView, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateImage( VkDevice  device, const VkImageCreateInfo * pCreateInfo, const VkAllocationCallbacks * pAllocator,  VkImage * pImage)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  ,const VkImageCreateInfo * ,const VkAllocationCallbacks * , VkImage * ))
        _android_vulkan_dlsym("vkCreateImage"))
            (device, pCreateInfo, pAllocator, pImage);
}

VKAPI_ATTR void VKAPI_CALL vkDestroyImage( VkDevice  device,  VkImage  image, const VkAllocationCallbacks * pAllocator)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  , VkImage  ,const VkAllocationCallbacks * ))
        _android_vulkan_dlsym("vkDestroyImage"))
            (device, image, pAllocator);
}

VKAPI_ATTR void VKAPI_CALL vkGetImageSubresourceLayout( VkDevice  device,  VkImage  image, const VkImageSubresource * pSubresource,  VkSubresourceLayout * pLayout)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  , VkImage  ,const VkImageSubresource * , VkSubresourceLayout * ))
        _android_vulkan_dlsym("vkGetImageSubresourceLayout"))
            (device, image, pSubresource, pLayout);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateImageView( VkDevice  device, const VkImageViewCreateInfo * pCreateInfo, const VkAllocationCallbacks * pAllocator,  VkImageView * pView)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  ,const VkImageViewCreateInfo * ,const VkAllocationCallbacks * , VkImageView * ))
        _android_vulkan_dlsym("vkCreateImageView"))
            (device, pCreateInfo, pAllocator, pView);
}

VKAPI_ATTR void VKAPI_CALL vkDestroyImageView( VkDevice  device,  VkImageView  imageView, const VkAllocationCallbacks * pAllocator)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  , VkImageView  ,const VkAllocationCallbacks * ))
        _android_vulkan_dlsym("vkDestroyImageView"))
            (device, imageView, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateShaderModule( VkDevice  device, const VkShaderModuleCreateInfo * pCreateInfo, const VkAllocationCallbacks * pAllocator,  VkShaderModule * pShaderModule)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  ,const VkShaderModuleCreateInfo * ,const VkAllocationCallbacks * , VkShaderModule * ))
        _android_vulkan_dlsym("vkCreateShaderModule"))
            (device, pCreateInfo, pAllocator, pShaderModule);
}

VKAPI_ATTR void VKAPI_CALL vkDestroyShaderModule( VkDevice  device,  VkShaderModule  shaderModule, const VkAllocationCallbacks * pAllocator)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  , VkShaderModule  ,const VkAllocationCallbacks * ))
        _android_vulkan_dlsym("vkDestroyShaderModule"))
            (device, shaderModule, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreatePipelineCache( VkDevice  device, const VkPipelineCacheCreateInfo * pCreateInfo, const VkAllocationCallbacks * pAllocator,  VkPipelineCache * pPipelineCache)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  ,const VkPipelineCacheCreateInfo * ,const VkAllocationCallbacks * , VkPipelineCache * ))
        _android_vulkan_dlsym("vkCreatePipelineCache"))
            (device, pCreateInfo, pAllocator, pPipelineCache);
}

VKAPI_ATTR void VKAPI_CALL vkDestroyPipelineCache( VkDevice  device,  VkPipelineCache  pipelineCache, const VkAllocationCallbacks * pAllocator)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  , VkPipelineCache  ,const VkAllocationCallbacks * ))
        _android_vulkan_dlsym("vkDestroyPipelineCache"))
            (device, pipelineCache, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPipelineCacheData( VkDevice  device,  VkPipelineCache  pipelineCache,  size_t * pDataSize,  void * pData)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , VkPipelineCache  , size_t * , void * ))
        _android_vulkan_dlsym("vkGetPipelineCacheData"))
            (device, pipelineCache, pDataSize, pData);
}

VKAPI_ATTR VkResult VKAPI_CALL vkMergePipelineCaches( VkDevice  device,  VkPipelineCache  dstCache,  uint32_t  srcCacheCount, const VkPipelineCache * pSrcCaches)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , VkPipelineCache  , uint32_t  ,const VkPipelineCache * ))
        _android_vulkan_dlsym("vkMergePipelineCaches"))
            (device, dstCache, srcCacheCount, pSrcCaches);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateGraphicsPipelines( VkDevice  device,  VkPipelineCache  pipelineCache,  uint32_t  createInfoCount, const VkGraphicsPipelineCreateInfo * pCreateInfos, const VkAllocationCallbacks * pAllocator,  VkPipeline * pPipelines)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , VkPipelineCache  , uint32_t  ,const VkGraphicsPipelineCreateInfo * ,const VkAllocationCallbacks * , VkPipeline * ))
        _android_vulkan_dlsym("vkCreateGraphicsPipelines"))
            (device, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateComputePipelines( VkDevice  device,  VkPipelineCache  pipelineCache,  uint32_t  createInfoCount, const VkComputePipelineCreateInfo * pCreateInfos, const VkAllocationCallbacks * pAllocator,  VkPipeline * pPipelines)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , VkPipelineCache  , uint32_t  ,const VkComputePipelineCreateInfo * ,const VkAllocationCallbacks * , VkPipeline * ))
        _android_vulkan_dlsym("vkCreateComputePipelines"))
            (device, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines);
}

VKAPI_ATTR void VKAPI_CALL vkDestroyPipeline( VkDevice  device,  VkPipeline  pipeline, const VkAllocationCallbacks * pAllocator)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  , VkPipeline  ,const VkAllocationCallbacks * ))
        _android_vulkan_dlsym("vkDestroyPipeline"))
            (device, pipeline, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreatePipelineLayout( VkDevice  device, const VkPipelineLayoutCreateInfo * pCreateInfo, const VkAllocationCallbacks * pAllocator,  VkPipelineLayout * pPipelineLayout)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  ,const VkPipelineLayoutCreateInfo * ,const VkAllocationCallbacks * , VkPipelineLayout * ))
        _android_vulkan_dlsym("vkCreatePipelineLayout"))
            (device, pCreateInfo, pAllocator, pPipelineLayout);
}

VKAPI_ATTR void VKAPI_CALL vkDestroyPipelineLayout( VkDevice  device,  VkPipelineLayout  pipelineLayout, const VkAllocationCallbacks * pAllocator)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  , VkPipelineLayout  ,const VkAllocationCallbacks * ))
        _android_vulkan_dlsym("vkDestroyPipelineLayout"))
            (device, pipelineLayout, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateSampler( VkDevice  device, const VkSamplerCreateInfo * pCreateInfo, const VkAllocationCallbacks * pAllocator,  VkSampler * pSampler)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  ,const VkSamplerCreateInfo * ,const VkAllocationCallbacks * , VkSampler * ))
        _android_vulkan_dlsym("vkCreateSampler"))
            (device, pCreateInfo, pAllocator, pSampler);
}

VKAPI_ATTR void VKAPI_CALL vkDestroySampler( VkDevice  device,  VkSampler  sampler, const VkAllocationCallbacks * pAllocator)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  , VkSampler  ,const VkAllocationCallbacks * ))
        _android_vulkan_dlsym("vkDestroySampler"))
            (device, sampler, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateDescriptorSetLayout( VkDevice  device, const VkDescriptorSetLayoutCreateInfo * pCreateInfo, const VkAllocationCallbacks * pAllocator,  VkDescriptorSetLayout * pSetLayout)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  ,const VkDescriptorSetLayoutCreateInfo * ,const VkAllocationCallbacks * , VkDescriptorSetLayout * ))
        _android_vulkan_dlsym("vkCreateDescriptorSetLayout"))
            (device, pCreateInfo, pAllocator, pSetLayout);
}

VKAPI_ATTR void VKAPI_CALL vkDestroyDescriptorSetLayout( VkDevice  device,  VkDescriptorSetLayout  descriptorSetLayout, const VkAllocationCallbacks * pAllocator)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  , VkDescriptorSetLayout  ,const VkAllocationCallbacks * ))
        _android_vulkan_dlsym("vkDestroyDescriptorSetLayout"))
            (device, descriptorSetLayout, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateDescriptorPool( VkDevice  device, const VkDescriptorPoolCreateInfo * pCreateInfo, const VkAllocationCallbacks * pAllocator,  VkDescriptorPool * pDescriptorPool)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  ,const VkDescriptorPoolCreateInfo * ,const VkAllocationCallbacks * , VkDescriptorPool * ))
        _android_vulkan_dlsym("vkCreateDescriptorPool"))
            (device, pCreateInfo, pAllocator, pDescriptorPool);
}

VKAPI_ATTR void VKAPI_CALL vkDestroyDescriptorPool( VkDevice  device,  VkDescriptorPool  descriptorPool, const VkAllocationCallbacks * pAllocator)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  , VkDescriptorPool  ,const VkAllocationCallbacks * ))
        _android_vulkan_dlsym("vkDestroyDescriptorPool"))
            (device, descriptorPool, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL vkResetDescriptorPool( VkDevice  device,  VkDescriptorPool  descriptorPool,  VkDescriptorPoolResetFlags  flags)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , VkDescriptorPool  , VkDescriptorPoolResetFlags  ))
        _android_vulkan_dlsym("vkResetDescriptorPool"))
            (device, descriptorPool, flags);
}

VKAPI_ATTR VkResult VKAPI_CALL vkAllocateDescriptorSets( VkDevice  device, const VkDescriptorSetAllocateInfo * pAllocateInfo,  VkDescriptorSet * pDescriptorSets)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  ,const VkDescriptorSetAllocateInfo * , VkDescriptorSet * ))
        _android_vulkan_dlsym("vkAllocateDescriptorSets"))
            (device, pAllocateInfo, pDescriptorSets);
}

VKAPI_ATTR VkResult VKAPI_CALL vkFreeDescriptorSets( VkDevice  device,  VkDescriptorPool  descriptorPool,  uint32_t  descriptorSetCount, const VkDescriptorSet * pDescriptorSets)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , VkDescriptorPool  , uint32_t  ,const VkDescriptorSet * ))
        _android_vulkan_dlsym("vkFreeDescriptorSets"))
            (device, descriptorPool, descriptorSetCount, pDescriptorSets);
}

VKAPI_ATTR void VKAPI_CALL vkUpdateDescriptorSets( VkDevice  device,  uint32_t  descriptorWriteCount, const VkWriteDescriptorSet * pDescriptorWrites,  uint32_t  descriptorCopyCount, const VkCopyDescriptorSet * pDescriptorCopies)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  , uint32_t  ,const VkWriteDescriptorSet * , uint32_t  ,const VkCopyDescriptorSet * ))
        _android_vulkan_dlsym("vkUpdateDescriptorSets"))
            (device, descriptorWriteCount, pDescriptorWrites, descriptorCopyCount, pDescriptorCopies);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateFramebuffer( VkDevice  device, const VkFramebufferCreateInfo * pCreateInfo, const VkAllocationCallbacks * pAllocator,  VkFramebuffer * pFramebuffer)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  ,const VkFramebufferCreateInfo * ,const VkAllocationCallbacks * , VkFramebuffer * ))
        _android_vulkan_dlsym("vkCreateFramebuffer"))
            (device, pCreateInfo, pAllocator, pFramebuffer);
}

VKAPI_ATTR void VKAPI_CALL vkDestroyFramebuffer( VkDevice  device,  VkFramebuffer  framebuffer, const VkAllocationCallbacks * pAllocator)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  , VkFramebuffer  ,const VkAllocationCallbacks * ))
        _android_vulkan_dlsym("vkDestroyFramebuffer"))
            (device, framebuffer, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateRenderPass( VkDevice  device, const VkRenderPassCreateInfo * pCreateInfo, const VkAllocationCallbacks * pAllocator,  VkRenderPass * pRenderPass)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  ,const VkRenderPassCreateInfo * ,const VkAllocationCallbacks * , VkRenderPass * ))
        _android_vulkan_dlsym("vkCreateRenderPass"))
            (device, pCreateInfo, pAllocator, pRenderPass);
}

VKAPI_ATTR void VKAPI_CALL vkDestroyRenderPass( VkDevice  device,  VkRenderPass  renderPass, const VkAllocationCallbacks * pAllocator)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  , VkRenderPass  ,const VkAllocationCallbacks * ))
        _android_vulkan_dlsym("vkDestroyRenderPass"))
            (device, renderPass, pAllocator);
}

VKAPI_ATTR void VKAPI_CALL vkGetRenderAreaGranularity( VkDevice  device,  VkRenderPass  renderPass,  VkExtent2D * pGranularity)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  , VkRenderPass  , VkExtent2D * ))
        _android_vulkan_dlsym("vkGetRenderAreaGranularity"))
            (device, renderPass, pGranularity);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateCommandPool( VkDevice  device, const VkCommandPoolCreateInfo * pCreateInfo, const VkAllocationCallbacks * pAllocator,  VkCommandPool * pCommandPool)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  ,const VkCommandPoolCreateInfo * ,const VkAllocationCallbacks * , VkCommandPool * ))
        _android_vulkan_dlsym("vkCreateCommandPool"))
            (device, pCreateInfo, pAllocator, pCommandPool);
}

VKAPI_ATTR void VKAPI_CALL vkDestroyCommandPool( VkDevice  device,  VkCommandPool  commandPool, const VkAllocationCallbacks * pAllocator)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  , VkCommandPool  ,const VkAllocationCallbacks * ))
        _android_vulkan_dlsym("vkDestroyCommandPool"))
            (device, commandPool, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL vkResetCommandPool( VkDevice  device,  VkCommandPool  commandPool,  VkCommandPoolResetFlags  flags)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , VkCommandPool  , VkCommandPoolResetFlags  ))
        _android_vulkan_dlsym("vkResetCommandPool"))
            (device, commandPool, flags);
}

VKAPI_ATTR VkResult VKAPI_CALL vkAllocateCommandBuffers( VkDevice  device, const VkCommandBufferAllocateInfo * pAllocateInfo,  VkCommandBuffer * pCommandBuffers)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  ,const VkCommandBufferAllocateInfo * , VkCommandBuffer * ))
        _android_vulkan_dlsym("vkAllocateCommandBuffers"))
            (device, pAllocateInfo, pCommandBuffers);
}

VKAPI_ATTR void VKAPI_CALL vkFreeCommandBuffers( VkDevice  device,  VkCommandPool  commandPool,  uint32_t  commandBufferCount, const VkCommandBuffer * pCommandBuffers)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  , VkCommandPool  , uint32_t  ,const VkCommandBuffer * ))
        _android_vulkan_dlsym("vkFreeCommandBuffers"))
            (device, commandPool, commandBufferCount, pCommandBuffers);
}

VKAPI_ATTR VkResult VKAPI_CALL vkBeginCommandBuffer( VkCommandBuffer  commandBuffer, const VkCommandBufferBeginInfo * pBeginInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkCommandBuffer  ,const VkCommandBufferBeginInfo * ))
        _android_vulkan_dlsym("vkBeginCommandBuffer"))
            (commandBuffer, pBeginInfo);
}

VKAPI_ATTR VkResult VKAPI_CALL vkEndCommandBuffer( VkCommandBuffer  commandBuffer)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkCommandBuffer  ))
        _android_vulkan_dlsym("vkEndCommandBuffer"))
            (commandBuffer);
}

VKAPI_ATTR VkResult VKAPI_CALL vkResetCommandBuffer( VkCommandBuffer  commandBuffer,  VkCommandBufferResetFlags  flags)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkCommandBuffer  , VkCommandBufferResetFlags  ))
        _android_vulkan_dlsym("vkResetCommandBuffer"))
            (commandBuffer, flags);
}

VKAPI_ATTR void VKAPI_CALL vkCmdBindPipeline( VkCommandBuffer  commandBuffer,  VkPipelineBindPoint  pipelineBindPoint,  VkPipeline  pipeline)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkPipelineBindPoint  , VkPipeline  ))
        _android_vulkan_dlsym("vkCmdBindPipeline"))
            (commandBuffer, pipelineBindPoint, pipeline);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetViewport( VkCommandBuffer  commandBuffer,  uint32_t  firstViewport,  uint32_t  viewportCount, const VkViewport * pViewports)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , uint32_t  , uint32_t  ,const VkViewport * ))
        _android_vulkan_dlsym("vkCmdSetViewport"))
            (commandBuffer, firstViewport, viewportCount, pViewports);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetScissor( VkCommandBuffer  commandBuffer,  uint32_t  firstScissor,  uint32_t  scissorCount, const VkRect2D * pScissors)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , uint32_t  , uint32_t  ,const VkRect2D * ))
        _android_vulkan_dlsym("vkCmdSetScissor"))
            (commandBuffer, firstScissor, scissorCount, pScissors);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetLineWidth( VkCommandBuffer  commandBuffer,  float  lineWidth)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , float  ))
        _android_vulkan_dlsym("vkCmdSetLineWidth"))
            (commandBuffer, lineWidth);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthBias( VkCommandBuffer  commandBuffer,  float  depthBiasConstantFactor,  float  depthBiasClamp,  float  depthBiasSlopeFactor)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , float  , float  , float  ))
        _android_vulkan_dlsym("vkCmdSetDepthBias"))
            (commandBuffer, depthBiasConstantFactor, depthBiasClamp, depthBiasSlopeFactor);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetBlendConstants( VkCommandBuffer  commandBuffer, const float  blendConstants[4])
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  ,const float  [4]))
        _android_vulkan_dlsym("vkCmdSetBlendConstants"))
            (commandBuffer, blendConstants);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthBounds( VkCommandBuffer  commandBuffer,  float  minDepthBounds,  float  maxDepthBounds)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , float  , float  ))
        _android_vulkan_dlsym("vkCmdSetDepthBounds"))
            (commandBuffer, minDepthBounds, maxDepthBounds);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetStencilCompareMask( VkCommandBuffer  commandBuffer,  VkStencilFaceFlags  faceMask,  uint32_t  compareMask)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkStencilFaceFlags  , uint32_t  ))
        _android_vulkan_dlsym("vkCmdSetStencilCompareMask"))
            (commandBuffer, faceMask, compareMask);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetStencilWriteMask( VkCommandBuffer  commandBuffer,  VkStencilFaceFlags  faceMask,  uint32_t  writeMask)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkStencilFaceFlags  , uint32_t  ))
        _android_vulkan_dlsym("vkCmdSetStencilWriteMask"))
            (commandBuffer, faceMask, writeMask);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetStencilReference( VkCommandBuffer  commandBuffer,  VkStencilFaceFlags  faceMask,  uint32_t  reference)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkStencilFaceFlags  , uint32_t  ))
        _android_vulkan_dlsym("vkCmdSetStencilReference"))
            (commandBuffer, faceMask, reference);
}

VKAPI_ATTR void VKAPI_CALL vkCmdBindDescriptorSets( VkCommandBuffer  commandBuffer,  VkPipelineBindPoint  pipelineBindPoint,  VkPipelineLayout  layout,  uint32_t  firstSet,  uint32_t  descriptorSetCount, const VkDescriptorSet * pDescriptorSets,  uint32_t  dynamicOffsetCount, const uint32_t * pDynamicOffsets)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkPipelineBindPoint  , VkPipelineLayout  , uint32_t  , uint32_t  ,const VkDescriptorSet * , uint32_t  ,const uint32_t * ))
        _android_vulkan_dlsym("vkCmdBindDescriptorSets"))
            (commandBuffer, pipelineBindPoint, layout, firstSet, descriptorSetCount, pDescriptorSets, dynamicOffsetCount, pDynamicOffsets);
}

VKAPI_ATTR void VKAPI_CALL vkCmdBindIndexBuffer( VkCommandBuffer  commandBuffer,  VkBuffer  buffer,  VkDeviceSize  offset,  VkIndexType  indexType)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkBuffer  , VkDeviceSize  , VkIndexType  ))
        _android_vulkan_dlsym("vkCmdBindIndexBuffer"))
            (commandBuffer, buffer, offset, indexType);
}

VKAPI_ATTR void VKAPI_CALL vkCmdBindVertexBuffers( VkCommandBuffer  commandBuffer,  uint32_t  firstBinding,  uint32_t  bindingCount, const VkBuffer * pBuffers, const VkDeviceSize * pOffsets)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , uint32_t  , uint32_t  ,const VkBuffer * ,const VkDeviceSize * ))
        _android_vulkan_dlsym("vkCmdBindVertexBuffers"))
            (commandBuffer, firstBinding, bindingCount, pBuffers, pOffsets);
}

VKAPI_ATTR void VKAPI_CALL vkCmdDraw( VkCommandBuffer  commandBuffer,  uint32_t  vertexCount,  uint32_t  instanceCount,  uint32_t  firstVertex,  uint32_t  firstInstance)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , uint32_t  , uint32_t  , uint32_t  , uint32_t  ))
        _android_vulkan_dlsym("vkCmdDraw"))
            (commandBuffer, vertexCount, instanceCount, firstVertex, firstInstance);
}

VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndexed( VkCommandBuffer  commandBuffer,  uint32_t  indexCount,  uint32_t  instanceCount,  uint32_t  firstIndex,  int32_t  vertexOffset,  uint32_t  firstInstance)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , uint32_t  , uint32_t  , uint32_t  , int32_t  , uint32_t  ))
        _android_vulkan_dlsym("vkCmdDrawIndexed"))
            (commandBuffer, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndirect( VkCommandBuffer  commandBuffer,  VkBuffer  buffer,  VkDeviceSize  offset,  uint32_t  drawCount,  uint32_t  stride)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkBuffer  , VkDeviceSize  , uint32_t  , uint32_t  ))
        _android_vulkan_dlsym("vkCmdDrawIndirect"))
            (commandBuffer, buffer, offset, drawCount, stride);
}

VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndexedIndirect( VkCommandBuffer  commandBuffer,  VkBuffer  buffer,  VkDeviceSize  offset,  uint32_t  drawCount,  uint32_t  stride)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkBuffer  , VkDeviceSize  , uint32_t  , uint32_t  ))
        _android_vulkan_dlsym("vkCmdDrawIndexedIndirect"))
            (commandBuffer, buffer, offset, drawCount, stride);
}

VKAPI_ATTR void VKAPI_CALL vkCmdDispatch( VkCommandBuffer  commandBuffer,  uint32_t  groupCountX,  uint32_t  groupCountY,  uint32_t  groupCountZ)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , uint32_t  , uint32_t  , uint32_t  ))
        _android_vulkan_dlsym("vkCmdDispatch"))
            (commandBuffer, groupCountX, groupCountY, groupCountZ);
}

VKAPI_ATTR void VKAPI_CALL vkCmdDispatchIndirect( VkCommandBuffer  commandBuffer,  VkBuffer  buffer,  VkDeviceSize  offset)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkBuffer  , VkDeviceSize  ))
        _android_vulkan_dlsym("vkCmdDispatchIndirect"))
            (commandBuffer, buffer, offset);
}

VKAPI_ATTR void VKAPI_CALL vkCmdCopyBuffer( VkCommandBuffer  commandBuffer,  VkBuffer  srcBuffer,  VkBuffer  dstBuffer,  uint32_t  regionCount, const VkBufferCopy * pRegions)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkBuffer  , VkBuffer  , uint32_t  ,const VkBufferCopy * ))
        _android_vulkan_dlsym("vkCmdCopyBuffer"))
            (commandBuffer, srcBuffer, dstBuffer, regionCount, pRegions);
}

VKAPI_ATTR void VKAPI_CALL vkCmdCopyImage( VkCommandBuffer  commandBuffer,  VkImage  srcImage,  VkImageLayout  srcImageLayout,  VkImage  dstImage,  VkImageLayout  dstImageLayout,  uint32_t  regionCount, const VkImageCopy * pRegions)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkImage  , VkImageLayout  , VkImage  , VkImageLayout  , uint32_t  ,const VkImageCopy * ))
        _android_vulkan_dlsym("vkCmdCopyImage"))
            (commandBuffer, srcImage, srcImageLayout, dstImage, dstImageLayout, regionCount, pRegions);
}

VKAPI_ATTR void VKAPI_CALL vkCmdBlitImage( VkCommandBuffer  commandBuffer,  VkImage  srcImage,  VkImageLayout  srcImageLayout,  VkImage  dstImage,  VkImageLayout  dstImageLayout,  uint32_t  regionCount, const VkImageBlit * pRegions,  VkFilter  filter)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkImage  , VkImageLayout  , VkImage  , VkImageLayout  , uint32_t  ,const VkImageBlit * , VkFilter  ))
        _android_vulkan_dlsym("vkCmdBlitImage"))
            (commandBuffer, srcImage, srcImageLayout, dstImage, dstImageLayout, regionCount, pRegions, filter);
}

VKAPI_ATTR void VKAPI_CALL vkCmdCopyBufferToImage( VkCommandBuffer  commandBuffer,  VkBuffer  srcBuffer,  VkImage  dstImage,  VkImageLayout  dstImageLayout,  uint32_t  regionCount, const VkBufferImageCopy * pRegions)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkBuffer  , VkImage  , VkImageLayout  , uint32_t  ,const VkBufferImageCopy * ))
        _android_vulkan_dlsym("vkCmdCopyBufferToImage"))
            (commandBuffer, srcBuffer, dstImage, dstImageLayout, regionCount, pRegions);
}

VKAPI_ATTR void VKAPI_CALL vkCmdCopyImageToBuffer( VkCommandBuffer  commandBuffer,  VkImage  srcImage,  VkImageLayout  srcImageLayout,  VkBuffer  dstBuffer,  uint32_t  regionCount, const VkBufferImageCopy * pRegions)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkImage  , VkImageLayout  , VkBuffer  , uint32_t  ,const VkBufferImageCopy * ))
        _android_vulkan_dlsym("vkCmdCopyImageToBuffer"))
            (commandBuffer, srcImage, srcImageLayout, dstBuffer, regionCount, pRegions);
}

VKAPI_ATTR void VKAPI_CALL vkCmdUpdateBuffer( VkCommandBuffer  commandBuffer,  VkBuffer  dstBuffer,  VkDeviceSize  dstOffset,  VkDeviceSize  dataSize, const void * pData)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkBuffer  , VkDeviceSize  , VkDeviceSize  ,const void * ))
        _android_vulkan_dlsym("vkCmdUpdateBuffer"))
            (commandBuffer, dstBuffer, dstOffset, dataSize, pData);
}

VKAPI_ATTR void VKAPI_CALL vkCmdFillBuffer( VkCommandBuffer  commandBuffer,  VkBuffer  dstBuffer,  VkDeviceSize  dstOffset,  VkDeviceSize  size,  uint32_t  data)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkBuffer  , VkDeviceSize  , VkDeviceSize  , uint32_t  ))
        _android_vulkan_dlsym("vkCmdFillBuffer"))
            (commandBuffer, dstBuffer, dstOffset, size, data);
}

VKAPI_ATTR void VKAPI_CALL vkCmdClearColorImage( VkCommandBuffer  commandBuffer,  VkImage  image,  VkImageLayout  imageLayout, const VkClearColorValue * pColor,  uint32_t  rangeCount, const VkImageSubresourceRange * pRanges)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkImage  , VkImageLayout  ,const VkClearColorValue * , uint32_t  ,const VkImageSubresourceRange * ))
        _android_vulkan_dlsym("vkCmdClearColorImage"))
            (commandBuffer, image, imageLayout, pColor, rangeCount, pRanges);
}

VKAPI_ATTR void VKAPI_CALL vkCmdClearDepthStencilImage( VkCommandBuffer  commandBuffer,  VkImage  image,  VkImageLayout  imageLayout, const VkClearDepthStencilValue * pDepthStencil,  uint32_t  rangeCount, const VkImageSubresourceRange * pRanges)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkImage  , VkImageLayout  ,const VkClearDepthStencilValue * , uint32_t  ,const VkImageSubresourceRange * ))
        _android_vulkan_dlsym("vkCmdClearDepthStencilImage"))
            (commandBuffer, image, imageLayout, pDepthStencil, rangeCount, pRanges);
}

VKAPI_ATTR void VKAPI_CALL vkCmdClearAttachments( VkCommandBuffer  commandBuffer,  uint32_t  attachmentCount, const VkClearAttachment * pAttachments,  uint32_t  rectCount, const VkClearRect * pRects)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , uint32_t  ,const VkClearAttachment * , uint32_t  ,const VkClearRect * ))
        _android_vulkan_dlsym("vkCmdClearAttachments"))
            (commandBuffer, attachmentCount, pAttachments, rectCount, pRects);
}

VKAPI_ATTR void VKAPI_CALL vkCmdResolveImage( VkCommandBuffer  commandBuffer,  VkImage  srcImage,  VkImageLayout  srcImageLayout,  VkImage  dstImage,  VkImageLayout  dstImageLayout,  uint32_t  regionCount, const VkImageResolve * pRegions)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkImage  , VkImageLayout  , VkImage  , VkImageLayout  , uint32_t  ,const VkImageResolve * ))
        _android_vulkan_dlsym("vkCmdResolveImage"))
            (commandBuffer, srcImage, srcImageLayout, dstImage, dstImageLayout, regionCount, pRegions);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetEvent( VkCommandBuffer  commandBuffer,  VkEvent  event,  VkPipelineStageFlags  stageMask)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkEvent  , VkPipelineStageFlags  ))
        _android_vulkan_dlsym("vkCmdSetEvent"))
            (commandBuffer, event, stageMask);
}

VKAPI_ATTR void VKAPI_CALL vkCmdResetEvent( VkCommandBuffer  commandBuffer,  VkEvent  event,  VkPipelineStageFlags  stageMask)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkEvent  , VkPipelineStageFlags  ))
        _android_vulkan_dlsym("vkCmdResetEvent"))
            (commandBuffer, event, stageMask);
}

VKAPI_ATTR void VKAPI_CALL vkCmdWaitEvents( VkCommandBuffer  commandBuffer,  uint32_t  eventCount, const VkEvent * pEvents,  VkPipelineStageFlags  srcStageMask,  VkPipelineStageFlags  dstStageMask,  uint32_t  memoryBarrierCount, const VkMemoryBarrier * pMemoryBarriers,  uint32_t  bufferMemoryBarrierCount, const VkBufferMemoryBarrier * pBufferMemoryBarriers,  uint32_t  imageMemoryBarrierCount, const VkImageMemoryBarrier * pImageMemoryBarriers)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , uint32_t  ,const VkEvent * , VkPipelineStageFlags  , VkPipelineStageFlags  , uint32_t  ,const VkMemoryBarrier * , uint32_t  ,const VkBufferMemoryBarrier * , uint32_t  ,const VkImageMemoryBarrier * ))
        _android_vulkan_dlsym("vkCmdWaitEvents"))
            (commandBuffer, eventCount, pEvents, srcStageMask, dstStageMask, memoryBarrierCount, pMemoryBarriers, bufferMemoryBarrierCount, pBufferMemoryBarriers, imageMemoryBarrierCount, pImageMemoryBarriers);
}

VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier( VkCommandBuffer  commandBuffer,  VkPipelineStageFlags  srcStageMask,  VkPipelineStageFlags  dstStageMask,  VkDependencyFlags  dependencyFlags,  uint32_t  memoryBarrierCount, const VkMemoryBarrier * pMemoryBarriers,  uint32_t  bufferMemoryBarrierCount, const VkBufferMemoryBarrier * pBufferMemoryBarriers,  uint32_t  imageMemoryBarrierCount, const VkImageMemoryBarrier * pImageMemoryBarriers)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkPipelineStageFlags  , VkPipelineStageFlags  , VkDependencyFlags  , uint32_t  ,const VkMemoryBarrier * , uint32_t  ,const VkBufferMemoryBarrier * , uint32_t  ,const VkImageMemoryBarrier * ))
        _android_vulkan_dlsym("vkCmdPipelineBarrier"))
            (commandBuffer, srcStageMask, dstStageMask, dependencyFlags, memoryBarrierCount, pMemoryBarriers, bufferMemoryBarrierCount, pBufferMemoryBarriers, imageMemoryBarrierCount, pImageMemoryBarriers);
}

VKAPI_ATTR void VKAPI_CALL vkCmdBeginQuery( VkCommandBuffer  commandBuffer,  VkQueryPool  queryPool,  uint32_t  query,  VkQueryControlFlags  flags)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkQueryPool  , uint32_t  , VkQueryControlFlags  ))
        _android_vulkan_dlsym("vkCmdBeginQuery"))
            (commandBuffer, queryPool, query, flags);
}

VKAPI_ATTR void VKAPI_CALL vkCmdEndQuery( VkCommandBuffer  commandBuffer,  VkQueryPool  queryPool,  uint32_t  query)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkQueryPool  , uint32_t  ))
        _android_vulkan_dlsym("vkCmdEndQuery"))
            (commandBuffer, queryPool, query);
}

VKAPI_ATTR void VKAPI_CALL vkCmdResetQueryPool( VkCommandBuffer  commandBuffer,  VkQueryPool  queryPool,  uint32_t  firstQuery,  uint32_t  queryCount)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkQueryPool  , uint32_t  , uint32_t  ))
        _android_vulkan_dlsym("vkCmdResetQueryPool"))
            (commandBuffer, queryPool, firstQuery, queryCount);
}

VKAPI_ATTR void VKAPI_CALL vkCmdWriteTimestamp( VkCommandBuffer  commandBuffer,  VkPipelineStageFlagBits  pipelineStage,  VkQueryPool  queryPool,  uint32_t  query)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkPipelineStageFlagBits  , VkQueryPool  , uint32_t  ))
        _android_vulkan_dlsym("vkCmdWriteTimestamp"))
            (commandBuffer, pipelineStage, queryPool, query);
}

VKAPI_ATTR void VKAPI_CALL vkCmdCopyQueryPoolResults( VkCommandBuffer  commandBuffer,  VkQueryPool  queryPool,  uint32_t  firstQuery,  uint32_t  queryCount,  VkBuffer  dstBuffer,  VkDeviceSize  dstOffset,  VkDeviceSize  stride,  VkQueryResultFlags  flags)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkQueryPool  , uint32_t  , uint32_t  , VkBuffer  , VkDeviceSize  , VkDeviceSize  , VkQueryResultFlags  ))
        _android_vulkan_dlsym("vkCmdCopyQueryPoolResults"))
            (commandBuffer, queryPool, firstQuery, queryCount, dstBuffer, dstOffset, stride, flags);
}

VKAPI_ATTR void VKAPI_CALL vkCmdPushConstants( VkCommandBuffer  commandBuffer,  VkPipelineLayout  layout,  VkShaderStageFlags  stageFlags,  uint32_t  offset,  uint32_t  size, const void * pValues)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkPipelineLayout  , VkShaderStageFlags  , uint32_t  , uint32_t  ,const void * ))
        _android_vulkan_dlsym("vkCmdPushConstants"))
            (commandBuffer, layout, stageFlags, offset, size, pValues);
}

VKAPI_ATTR void VKAPI_CALL vkCmdBeginRenderPass( VkCommandBuffer  commandBuffer, const VkRenderPassBeginInfo * pRenderPassBegin,  VkSubpassContents  contents)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  ,const VkRenderPassBeginInfo * , VkSubpassContents  ))
        _android_vulkan_dlsym("vkCmdBeginRenderPass"))
            (commandBuffer, pRenderPassBegin, contents);
}

VKAPI_ATTR void VKAPI_CALL vkCmdNextSubpass( VkCommandBuffer  commandBuffer,  VkSubpassContents  contents)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkSubpassContents  ))
        _android_vulkan_dlsym("vkCmdNextSubpass"))
            (commandBuffer, contents);
}

VKAPI_ATTR void VKAPI_CALL vkCmdEndRenderPass( VkCommandBuffer  commandBuffer)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  ))
        _android_vulkan_dlsym("vkCmdEndRenderPass"))
            (commandBuffer);
}

VKAPI_ATTR void VKAPI_CALL vkCmdExecuteCommands( VkCommandBuffer  commandBuffer,  uint32_t  commandBufferCount, const VkCommandBuffer * pCommandBuffers)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , uint32_t  ,const VkCommandBuffer * ))
        _android_vulkan_dlsym("vkCmdExecuteCommands"))
            (commandBuffer, commandBufferCount, pCommandBuffers);
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceVersion( uint32_t * pApiVersion)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( uint32_t * ))
        _android_vulkan_dlsym("vkEnumerateInstanceVersion"))
            (pApiVersion);
}

VKAPI_ATTR VkResult VKAPI_CALL vkBindBufferMemory2( VkDevice  device,  uint32_t  bindInfoCount, const VkBindBufferMemoryInfo * pBindInfos)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , uint32_t  ,const VkBindBufferMemoryInfo * ))
        _android_vulkan_dlsym("vkBindBufferMemory2"))
            (device, bindInfoCount, pBindInfos);
}

VKAPI_ATTR VkResult VKAPI_CALL vkBindImageMemory2( VkDevice  device,  uint32_t  bindInfoCount, const VkBindImageMemoryInfo * pBindInfos)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , uint32_t  ,const VkBindImageMemoryInfo * ))
        _android_vulkan_dlsym("vkBindImageMemory2"))
            (device, bindInfoCount, pBindInfos);
}

VKAPI_ATTR void VKAPI_CALL vkGetDeviceGroupPeerMemoryFeatures( VkDevice  device,  uint32_t  heapIndex,  uint32_t  localDeviceIndex,  uint32_t  remoteDeviceIndex,  VkPeerMemoryFeatureFlags * pPeerMemoryFeatures)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  , uint32_t  , uint32_t  , uint32_t  , VkPeerMemoryFeatureFlags * ))
        _android_vulkan_dlsym("vkGetDeviceGroupPeerMemoryFeatures"))
            (device, heapIndex, localDeviceIndex, remoteDeviceIndex, pPeerMemoryFeatures);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetDeviceMask( VkCommandBuffer  commandBuffer,  uint32_t  deviceMask)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , uint32_t  ))
        _android_vulkan_dlsym("vkCmdSetDeviceMask"))
            (commandBuffer, deviceMask);
}

VKAPI_ATTR void VKAPI_CALL vkCmdDispatchBase( VkCommandBuffer  commandBuffer,  uint32_t  baseGroupX,  uint32_t  baseGroupY,  uint32_t  baseGroupZ,  uint32_t  groupCountX,  uint32_t  groupCountY,  uint32_t  groupCountZ)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , uint32_t  , uint32_t  , uint32_t  , uint32_t  , uint32_t  , uint32_t  ))
        _android_vulkan_dlsym("vkCmdDispatchBase"))
            (commandBuffer, baseGroupX, baseGroupY, baseGroupZ, groupCountX, groupCountY, groupCountZ);
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumeratePhysicalDeviceGroups( VkInstance  instance,  uint32_t * pPhysicalDeviceGroupCount,  VkPhysicalDeviceGroupProperties * pPhysicalDeviceGroupProperties)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkInstance  , uint32_t * , VkPhysicalDeviceGroupProperties * ))
        _android_vulkan_dlsym("vkEnumeratePhysicalDeviceGroups"))
            (instance, pPhysicalDeviceGroupCount, pPhysicalDeviceGroupProperties);
}

VKAPI_ATTR void VKAPI_CALL vkGetImageMemoryRequirements2( VkDevice  device, const VkImageMemoryRequirementsInfo2 * pInfo,  VkMemoryRequirements2 * pMemoryRequirements)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  ,const VkImageMemoryRequirementsInfo2 * , VkMemoryRequirements2 * ))
        _android_vulkan_dlsym("vkGetImageMemoryRequirements2"))
            (device, pInfo, pMemoryRequirements);
}

VKAPI_ATTR void VKAPI_CALL vkGetBufferMemoryRequirements2( VkDevice  device, const VkBufferMemoryRequirementsInfo2 * pInfo,  VkMemoryRequirements2 * pMemoryRequirements)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  ,const VkBufferMemoryRequirementsInfo2 * , VkMemoryRequirements2 * ))
        _android_vulkan_dlsym("vkGetBufferMemoryRequirements2"))
            (device, pInfo, pMemoryRequirements);
}

VKAPI_ATTR void VKAPI_CALL vkGetImageSparseMemoryRequirements2( VkDevice  device, const VkImageSparseMemoryRequirementsInfo2 * pInfo,  uint32_t * pSparseMemoryRequirementCount,  VkSparseImageMemoryRequirements2 * pSparseMemoryRequirements)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  ,const VkImageSparseMemoryRequirementsInfo2 * , uint32_t * , VkSparseImageMemoryRequirements2 * ))
        _android_vulkan_dlsym("vkGetImageSparseMemoryRequirements2"))
            (device, pInfo, pSparseMemoryRequirementCount, pSparseMemoryRequirements);
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures2( VkPhysicalDevice  physicalDevice,  VkPhysicalDeviceFeatures2 * pFeatures)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkPhysicalDevice  , VkPhysicalDeviceFeatures2 * ))
        _android_vulkan_dlsym("vkGetPhysicalDeviceFeatures2"))
            (physicalDevice, pFeatures);
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties2( VkPhysicalDevice  physicalDevice,  VkPhysicalDeviceProperties2 * pProperties)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkPhysicalDevice  , VkPhysicalDeviceProperties2 * ))
        _android_vulkan_dlsym("vkGetPhysicalDeviceProperties2"))
            (physicalDevice, pProperties);
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFormatProperties2( VkPhysicalDevice  physicalDevice,  VkFormat  format,  VkFormatProperties2 * pFormatProperties)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkPhysicalDevice  , VkFormat  , VkFormatProperties2 * ))
        _android_vulkan_dlsym("vkGetPhysicalDeviceFormatProperties2"))
            (physicalDevice, format, pFormatProperties);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceImageFormatProperties2( VkPhysicalDevice  physicalDevice, const VkPhysicalDeviceImageFormatInfo2 * pImageFormatInfo,  VkImageFormatProperties2 * pImageFormatProperties)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkPhysicalDevice  ,const VkPhysicalDeviceImageFormatInfo2 * , VkImageFormatProperties2 * ))
        _android_vulkan_dlsym("vkGetPhysicalDeviceImageFormatProperties2"))
            (physicalDevice, pImageFormatInfo, pImageFormatProperties);
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceQueueFamilyProperties2( VkPhysicalDevice  physicalDevice,  uint32_t * pQueueFamilyPropertyCount,  VkQueueFamilyProperties2 * pQueueFamilyProperties)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkPhysicalDevice  , uint32_t * , VkQueueFamilyProperties2 * ))
        _android_vulkan_dlsym("vkGetPhysicalDeviceQueueFamilyProperties2"))
            (physicalDevice, pQueueFamilyPropertyCount, pQueueFamilyProperties);
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties2( VkPhysicalDevice  physicalDevice,  VkPhysicalDeviceMemoryProperties2 * pMemoryProperties)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkPhysicalDevice  , VkPhysicalDeviceMemoryProperties2 * ))
        _android_vulkan_dlsym("vkGetPhysicalDeviceMemoryProperties2"))
            (physicalDevice, pMemoryProperties);
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceSparseImageFormatProperties2( VkPhysicalDevice  physicalDevice, const VkPhysicalDeviceSparseImageFormatInfo2 * pFormatInfo,  uint32_t * pPropertyCount,  VkSparseImageFormatProperties2 * pProperties)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkPhysicalDevice  ,const VkPhysicalDeviceSparseImageFormatInfo2 * , uint32_t * , VkSparseImageFormatProperties2 * ))
        _android_vulkan_dlsym("vkGetPhysicalDeviceSparseImageFormatProperties2"))
            (physicalDevice, pFormatInfo, pPropertyCount, pProperties);
}

VKAPI_ATTR void VKAPI_CALL vkTrimCommandPool( VkDevice  device,  VkCommandPool  commandPool,  VkCommandPoolTrimFlags  flags)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  , VkCommandPool  , VkCommandPoolTrimFlags  ))
        _android_vulkan_dlsym("vkTrimCommandPool"))
            (device, commandPool, flags);
}

VKAPI_ATTR void VKAPI_CALL vkGetDeviceQueue2( VkDevice  device, const VkDeviceQueueInfo2 * pQueueInfo,  VkQueue * pQueue)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  ,const VkDeviceQueueInfo2 * , VkQueue * ))
        _android_vulkan_dlsym("vkGetDeviceQueue2"))
            (device, pQueueInfo, pQueue);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateSamplerYcbcrConversion( VkDevice  device, const VkSamplerYcbcrConversionCreateInfo * pCreateInfo, const VkAllocationCallbacks * pAllocator,  VkSamplerYcbcrConversion * pYcbcrConversion)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  ,const VkSamplerYcbcrConversionCreateInfo * ,const VkAllocationCallbacks * , VkSamplerYcbcrConversion * ))
        _android_vulkan_dlsym("vkCreateSamplerYcbcrConversion"))
            (device, pCreateInfo, pAllocator, pYcbcrConversion);
}

VKAPI_ATTR void VKAPI_CALL vkDestroySamplerYcbcrConversion( VkDevice  device,  VkSamplerYcbcrConversion  ycbcrConversion, const VkAllocationCallbacks * pAllocator)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  , VkSamplerYcbcrConversion  ,const VkAllocationCallbacks * ))
        _android_vulkan_dlsym("vkDestroySamplerYcbcrConversion"))
            (device, ycbcrConversion, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateDescriptorUpdateTemplate( VkDevice  device, const VkDescriptorUpdateTemplateCreateInfo * pCreateInfo, const VkAllocationCallbacks * pAllocator,  VkDescriptorUpdateTemplate * pDescriptorUpdateTemplate)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  ,const VkDescriptorUpdateTemplateCreateInfo * ,const VkAllocationCallbacks * , VkDescriptorUpdateTemplate * ))
        _android_vulkan_dlsym("vkCreateDescriptorUpdateTemplate"))
            (device, pCreateInfo, pAllocator, pDescriptorUpdateTemplate);
}

VKAPI_ATTR void VKAPI_CALL vkDestroyDescriptorUpdateTemplate( VkDevice  device,  VkDescriptorUpdateTemplate  descriptorUpdateTemplate, const VkAllocationCallbacks * pAllocator)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  , VkDescriptorUpdateTemplate  ,const VkAllocationCallbacks * ))
        _android_vulkan_dlsym("vkDestroyDescriptorUpdateTemplate"))
            (device, descriptorUpdateTemplate, pAllocator);
}

VKAPI_ATTR void VKAPI_CALL vkUpdateDescriptorSetWithTemplate( VkDevice  device,  VkDescriptorSet  descriptorSet,  VkDescriptorUpdateTemplate  descriptorUpdateTemplate, const void * pData)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  , VkDescriptorSet  , VkDescriptorUpdateTemplate  ,const void * ))
        _android_vulkan_dlsym("vkUpdateDescriptorSetWithTemplate"))
            (device, descriptorSet, descriptorUpdateTemplate, pData);
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceExternalBufferProperties( VkPhysicalDevice  physicalDevice, const VkPhysicalDeviceExternalBufferInfo * pExternalBufferInfo,  VkExternalBufferProperties * pExternalBufferProperties)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkPhysicalDevice  ,const VkPhysicalDeviceExternalBufferInfo * , VkExternalBufferProperties * ))
        _android_vulkan_dlsym("vkGetPhysicalDeviceExternalBufferProperties"))
            (physicalDevice, pExternalBufferInfo, pExternalBufferProperties);
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceExternalFenceProperties( VkPhysicalDevice  physicalDevice, const VkPhysicalDeviceExternalFenceInfo * pExternalFenceInfo,  VkExternalFenceProperties * pExternalFenceProperties)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkPhysicalDevice  ,const VkPhysicalDeviceExternalFenceInfo * , VkExternalFenceProperties * ))
        _android_vulkan_dlsym("vkGetPhysicalDeviceExternalFenceProperties"))
            (physicalDevice, pExternalFenceInfo, pExternalFenceProperties);
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceExternalSemaphoreProperties( VkPhysicalDevice  physicalDevice, const VkPhysicalDeviceExternalSemaphoreInfo * pExternalSemaphoreInfo,  VkExternalSemaphoreProperties * pExternalSemaphoreProperties)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkPhysicalDevice  ,const VkPhysicalDeviceExternalSemaphoreInfo * , VkExternalSemaphoreProperties * ))
        _android_vulkan_dlsym("vkGetPhysicalDeviceExternalSemaphoreProperties"))
            (physicalDevice, pExternalSemaphoreInfo, pExternalSemaphoreProperties);
}

VKAPI_ATTR void VKAPI_CALL vkGetDescriptorSetLayoutSupport( VkDevice  device, const VkDescriptorSetLayoutCreateInfo * pCreateInfo,  VkDescriptorSetLayoutSupport * pSupport)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  ,const VkDescriptorSetLayoutCreateInfo * , VkDescriptorSetLayoutSupport * ))
        _android_vulkan_dlsym("vkGetDescriptorSetLayoutSupport"))
            (device, pCreateInfo, pSupport);
}

VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndirectCount( VkCommandBuffer  commandBuffer,  VkBuffer  buffer,  VkDeviceSize  offset,  VkBuffer  countBuffer,  VkDeviceSize  countBufferOffset,  uint32_t  maxDrawCount,  uint32_t  stride)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkBuffer  , VkDeviceSize  , VkBuffer  , VkDeviceSize  , uint32_t  , uint32_t  ))
        _android_vulkan_dlsym("vkCmdDrawIndirectCount"))
            (commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);
}

VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndexedIndirectCount( VkCommandBuffer  commandBuffer,  VkBuffer  buffer,  VkDeviceSize  offset,  VkBuffer  countBuffer,  VkDeviceSize  countBufferOffset,  uint32_t  maxDrawCount,  uint32_t  stride)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkBuffer  , VkDeviceSize  , VkBuffer  , VkDeviceSize  , uint32_t  , uint32_t  ))
        _android_vulkan_dlsym("vkCmdDrawIndexedIndirectCount"))
            (commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateRenderPass2( VkDevice  device, const VkRenderPassCreateInfo2 * pCreateInfo, const VkAllocationCallbacks * pAllocator,  VkRenderPass * pRenderPass)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  ,const VkRenderPassCreateInfo2 * ,const VkAllocationCallbacks * , VkRenderPass * ))
        _android_vulkan_dlsym("vkCreateRenderPass2"))
            (device, pCreateInfo, pAllocator, pRenderPass);
}

VKAPI_ATTR void VKAPI_CALL vkCmdBeginRenderPass2( VkCommandBuffer  commandBuffer, const VkRenderPassBeginInfo * pRenderPassBegin, const VkSubpassBeginInfo * pSubpassBeginInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  ,const VkRenderPassBeginInfo * ,const VkSubpassBeginInfo * ))
        _android_vulkan_dlsym("vkCmdBeginRenderPass2"))
            (commandBuffer, pRenderPassBegin, pSubpassBeginInfo);
}

VKAPI_ATTR void VKAPI_CALL vkCmdNextSubpass2( VkCommandBuffer  commandBuffer, const VkSubpassBeginInfo * pSubpassBeginInfo, const VkSubpassEndInfo * pSubpassEndInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  ,const VkSubpassBeginInfo * ,const VkSubpassEndInfo * ))
        _android_vulkan_dlsym("vkCmdNextSubpass2"))
            (commandBuffer, pSubpassBeginInfo, pSubpassEndInfo);
}

VKAPI_ATTR void VKAPI_CALL vkCmdEndRenderPass2( VkCommandBuffer  commandBuffer, const VkSubpassEndInfo * pSubpassEndInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  ,const VkSubpassEndInfo * ))
        _android_vulkan_dlsym("vkCmdEndRenderPass2"))
            (commandBuffer, pSubpassEndInfo);
}

VKAPI_ATTR void VKAPI_CALL vkResetQueryPool( VkDevice  device,  VkQueryPool  queryPool,  uint32_t  firstQuery,  uint32_t  queryCount)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  , VkQueryPool  , uint32_t  , uint32_t  ))
        _android_vulkan_dlsym("vkResetQueryPool"))
            (device, queryPool, firstQuery, queryCount);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetSemaphoreCounterValue( VkDevice  device,  VkSemaphore  semaphore,  uint64_t * pValue)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , VkSemaphore  , uint64_t * ))
        _android_vulkan_dlsym("vkGetSemaphoreCounterValue"))
            (device, semaphore, pValue);
}

VKAPI_ATTR VkResult VKAPI_CALL vkWaitSemaphores( VkDevice  device, const VkSemaphoreWaitInfo * pWaitInfo,  uint64_t  timeout)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  ,const VkSemaphoreWaitInfo * , uint64_t  ))
        _android_vulkan_dlsym("vkWaitSemaphores"))
            (device, pWaitInfo, timeout);
}

VKAPI_ATTR VkResult VKAPI_CALL vkSignalSemaphore( VkDevice  device, const VkSemaphoreSignalInfo * pSignalInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  ,const VkSemaphoreSignalInfo * ))
        _android_vulkan_dlsym("vkSignalSemaphore"))
            (device, pSignalInfo);
}

VKAPI_ATTR VkDeviceAddress VKAPI_CALL vkGetBufferDeviceAddress( VkDevice  device, const VkBufferDeviceAddressInfo * pInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkDeviceAddress (*)( VkDevice  ,const VkBufferDeviceAddressInfo * ))
        _android_vulkan_dlsym("vkGetBufferDeviceAddress"))
            (device, pInfo);
}

VKAPI_ATTR uint64_t VKAPI_CALL vkGetBufferOpaqueCaptureAddress( VkDevice  device, const VkBufferDeviceAddressInfo * pInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((uint64_t (*)( VkDevice  ,const VkBufferDeviceAddressInfo * ))
        _android_vulkan_dlsym("vkGetBufferOpaqueCaptureAddress"))
            (device, pInfo);
}

VKAPI_ATTR uint64_t VKAPI_CALL vkGetDeviceMemoryOpaqueCaptureAddress( VkDevice  device, const VkDeviceMemoryOpaqueCaptureAddressInfo * pInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((uint64_t (*)( VkDevice  ,const VkDeviceMemoryOpaqueCaptureAddressInfo * ))
        _android_vulkan_dlsym("vkGetDeviceMemoryOpaqueCaptureAddress"))
            (device, pInfo);
}

#if VK_HEADER_VERSION >= 204

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceToolProperties( VkPhysicalDevice  physicalDevice,  uint32_t * pToolCount,  VkPhysicalDeviceToolProperties * pToolProperties)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkPhysicalDevice  , uint32_t * , VkPhysicalDeviceToolProperties * ))
        _android_vulkan_dlsym("vkGetPhysicalDeviceToolProperties"))
            (physicalDevice, pToolCount, pToolProperties);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreatePrivateDataSlot( VkDevice  device, const VkPrivateDataSlotCreateInfo * pCreateInfo, const VkAllocationCallbacks * pAllocator,  VkPrivateDataSlot * pPrivateDataSlot)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  ,const VkPrivateDataSlotCreateInfo * ,const VkAllocationCallbacks * , VkPrivateDataSlot * ))
        _android_vulkan_dlsym("vkCreatePrivateDataSlot"))
            (device, pCreateInfo, pAllocator, pPrivateDataSlot);
}

VKAPI_ATTR void VKAPI_CALL vkDestroyPrivateDataSlot( VkDevice  device,  VkPrivateDataSlot  privateDataSlot, const VkAllocationCallbacks * pAllocator)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  , VkPrivateDataSlot  ,const VkAllocationCallbacks * ))
        _android_vulkan_dlsym("vkDestroyPrivateDataSlot"))
            (device, privateDataSlot, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL vkSetPrivateData( VkDevice  device,  VkObjectType  objectType,  uint64_t  objectHandle,  VkPrivateDataSlot  privateDataSlot,  uint64_t  data)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , VkObjectType  , uint64_t  , VkPrivateDataSlot  , uint64_t  ))
        _android_vulkan_dlsym("vkSetPrivateData"))
            (device, objectType, objectHandle, privateDataSlot, data);
}

VKAPI_ATTR void VKAPI_CALL vkGetPrivateData( VkDevice  device,  VkObjectType  objectType,  uint64_t  objectHandle,  VkPrivateDataSlot  privateDataSlot,  uint64_t * pData)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  , VkObjectType  , uint64_t  , VkPrivateDataSlot  , uint64_t * ))
        _android_vulkan_dlsym("vkGetPrivateData"))
            (device, objectType, objectHandle, privateDataSlot, pData);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetEvent2( VkCommandBuffer  commandBuffer,  VkEvent  event, const VkDependencyInfo * pDependencyInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkEvent  ,const VkDependencyInfo * ))
        _android_vulkan_dlsym("vkCmdSetEvent2"))
            (commandBuffer, event, pDependencyInfo);
}

VKAPI_ATTR void VKAPI_CALL vkCmdResetEvent2( VkCommandBuffer  commandBuffer,  VkEvent  event,  VkPipelineStageFlags2  stageMask)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkEvent  , VkPipelineStageFlags2  ))
        _android_vulkan_dlsym("vkCmdResetEvent2"))
            (commandBuffer, event, stageMask);
}

VKAPI_ATTR void VKAPI_CALL vkCmdWaitEvents2( VkCommandBuffer  commandBuffer,  uint32_t  eventCount, const VkEvent * pEvents, const VkDependencyInfo * pDependencyInfos)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , uint32_t  ,const VkEvent * ,const VkDependencyInfo * ))
        _android_vulkan_dlsym("vkCmdWaitEvents2"))
            (commandBuffer, eventCount, pEvents, pDependencyInfos);
}

VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier2( VkCommandBuffer  commandBuffer, const VkDependencyInfo * pDependencyInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  ,const VkDependencyInfo * ))
        _android_vulkan_dlsym("vkCmdPipelineBarrier2"))
            (commandBuffer, pDependencyInfo);
}

VKAPI_ATTR void VKAPI_CALL vkCmdWriteTimestamp2( VkCommandBuffer  commandBuffer,  VkPipelineStageFlags2  stage,  VkQueryPool  queryPool,  uint32_t  query)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkPipelineStageFlags2  , VkQueryPool  , uint32_t  ))
        _android_vulkan_dlsym("vkCmdWriteTimestamp2"))
            (commandBuffer, stage, queryPool, query);
}

VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit2( VkQueue  queue,  uint32_t  submitCount, const VkSubmitInfo2 * pSubmits,  VkFence  fence)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkQueue  , uint32_t  ,const VkSubmitInfo2 * , VkFence  ))
        _android_vulkan_dlsym("vkQueueSubmit2"))
            (queue, submitCount, pSubmits, fence);
}

VKAPI_ATTR void VKAPI_CALL vkCmdCopyBuffer2( VkCommandBuffer  commandBuffer, const VkCopyBufferInfo2 * pCopyBufferInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  ,const VkCopyBufferInfo2 * ))
        _android_vulkan_dlsym("vkCmdCopyBuffer2"))
            (commandBuffer, pCopyBufferInfo);
}

VKAPI_ATTR void VKAPI_CALL vkCmdCopyImage2( VkCommandBuffer  commandBuffer, const VkCopyImageInfo2 * pCopyImageInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  ,const VkCopyImageInfo2 * ))
        _android_vulkan_dlsym("vkCmdCopyImage2"))
            (commandBuffer, pCopyImageInfo);
}

VKAPI_ATTR void VKAPI_CALL vkCmdCopyBufferToImage2( VkCommandBuffer  commandBuffer, const VkCopyBufferToImageInfo2 * pCopyBufferToImageInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  ,const VkCopyBufferToImageInfo2 * ))
        _android_vulkan_dlsym("vkCmdCopyBufferToImage2"))
            (commandBuffer, pCopyBufferToImageInfo);
}

VKAPI_ATTR void VKAPI_CALL vkCmdCopyImageToBuffer2( VkCommandBuffer  commandBuffer, const VkCopyImageToBufferInfo2 * pCopyImageToBufferInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  ,const VkCopyImageToBufferInfo2 * ))
        _android_vulkan_dlsym("vkCmdCopyImageToBuffer2"))
            (commandBuffer, pCopyImageToBufferInfo);
}

VKAPI_ATTR void VKAPI_CALL vkCmdBlitImage2( VkCommandBuffer  commandBuffer, const VkBlitImageInfo2 * pBlitImageInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  ,const VkBlitImageInfo2 * ))
        _android_vulkan_dlsym("vkCmdBlitImage2"))
            (commandBuffer, pBlitImageInfo);
}

VKAPI_ATTR void VKAPI_CALL vkCmdResolveImage2( VkCommandBuffer  commandBuffer, const VkResolveImageInfo2 * pResolveImageInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  ,const VkResolveImageInfo2 * ))
        _android_vulkan_dlsym("vkCmdResolveImage2"))
            (commandBuffer, pResolveImageInfo);
}

VKAPI_ATTR void VKAPI_CALL vkCmdBeginRendering( VkCommandBuffer  commandBuffer, const VkRenderingInfo * pRenderingInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  ,const VkRenderingInfo * ))
        _android_vulkan_dlsym("vkCmdBeginRendering"))
            (commandBuffer, pRenderingInfo);
}

VKAPI_ATTR void VKAPI_CALL vkCmdEndRendering( VkCommandBuffer  commandBuffer)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  ))
        _android_vulkan_dlsym("vkCmdEndRendering"))
            (commandBuffer);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetCullMode( VkCommandBuffer  commandBuffer,  VkCullModeFlags  cullMode)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkCullModeFlags  ))
        _android_vulkan_dlsym("vkCmdSetCullMode"))
            (commandBuffer, cullMode);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetFrontFace( VkCommandBuffer  commandBuffer,  VkFrontFace  frontFace)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkFrontFace  ))
        _android_vulkan_dlsym("vkCmdSetFrontFace"))
            (commandBuffer, frontFace);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetPrimitiveTopology( VkCommandBuffer  commandBuffer,  VkPrimitiveTopology  primitiveTopology)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkPrimitiveTopology  ))
        _android_vulkan_dlsym("vkCmdSetPrimitiveTopology"))
            (commandBuffer, primitiveTopology);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetViewportWithCount( VkCommandBuffer  commandBuffer,  uint32_t  viewportCount, const VkViewport * pViewports)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , uint32_t  ,const VkViewport * ))
        _android_vulkan_dlsym("vkCmdSetViewportWithCount"))
            (commandBuffer, viewportCount, pViewports);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetScissorWithCount( VkCommandBuffer  commandBuffer,  uint32_t  scissorCount, const VkRect2D * pScissors)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , uint32_t  ,const VkRect2D * ))
        _android_vulkan_dlsym("vkCmdSetScissorWithCount"))
            (commandBuffer, scissorCount, pScissors);
}

VKAPI_ATTR void VKAPI_CALL vkCmdBindVertexBuffers2( VkCommandBuffer  commandBuffer,  uint32_t  firstBinding,  uint32_t  bindingCount, const VkBuffer * pBuffers, const VkDeviceSize * pOffsets, const VkDeviceSize * pSizes, const VkDeviceSize * pStrides)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , uint32_t  , uint32_t  ,const VkBuffer * ,const VkDeviceSize * ,const VkDeviceSize * ,const VkDeviceSize * ))
        _android_vulkan_dlsym("vkCmdBindVertexBuffers2"))
            (commandBuffer, firstBinding, bindingCount, pBuffers, pOffsets, pSizes, pStrides);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthTestEnable( VkCommandBuffer  commandBuffer,  VkBool32  depthTestEnable)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkBool32  ))
        _android_vulkan_dlsym("vkCmdSetDepthTestEnable"))
            (commandBuffer, depthTestEnable);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthWriteEnable( VkCommandBuffer  commandBuffer,  VkBool32  depthWriteEnable)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkBool32  ))
        _android_vulkan_dlsym("vkCmdSetDepthWriteEnable"))
            (commandBuffer, depthWriteEnable);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthCompareOp( VkCommandBuffer  commandBuffer,  VkCompareOp  depthCompareOp)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkCompareOp  ))
        _android_vulkan_dlsym("vkCmdSetDepthCompareOp"))
            (commandBuffer, depthCompareOp);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthBoundsTestEnable( VkCommandBuffer  commandBuffer,  VkBool32  depthBoundsTestEnable)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkBool32  ))
        _android_vulkan_dlsym("vkCmdSetDepthBoundsTestEnable"))
            (commandBuffer, depthBoundsTestEnable);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetStencilTestEnable( VkCommandBuffer  commandBuffer,  VkBool32  stencilTestEnable)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkBool32  ))
        _android_vulkan_dlsym("vkCmdSetStencilTestEnable"))
            (commandBuffer, stencilTestEnable);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetStencilOp( VkCommandBuffer  commandBuffer,  VkStencilFaceFlags  faceMask,  VkStencilOp  failOp,  VkStencilOp  passOp,  VkStencilOp  depthFailOp,  VkCompareOp  compareOp)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkStencilFaceFlags  , VkStencilOp  , VkStencilOp  , VkStencilOp  , VkCompareOp  ))
        _android_vulkan_dlsym("vkCmdSetStencilOp"))
            (commandBuffer, faceMask, failOp, passOp, depthFailOp, compareOp);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetRasterizerDiscardEnable( VkCommandBuffer  commandBuffer,  VkBool32  rasterizerDiscardEnable)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkBool32  ))
        _android_vulkan_dlsym("vkCmdSetRasterizerDiscardEnable"))
            (commandBuffer, rasterizerDiscardEnable);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthBiasEnable( VkCommandBuffer  commandBuffer,  VkBool32  depthBiasEnable)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkBool32  ))
        _android_vulkan_dlsym("vkCmdSetDepthBiasEnable"))
            (commandBuffer, depthBiasEnable);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetPrimitiveRestartEnable( VkCommandBuffer  commandBuffer,  VkBool32  primitiveRestartEnable)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkBool32  ))
        _android_vulkan_dlsym("vkCmdSetPrimitiveRestartEnable"))
            (commandBuffer, primitiveRestartEnable);
}

VKAPI_ATTR void VKAPI_CALL vkGetDeviceBufferMemoryRequirements( VkDevice  device, const VkDeviceBufferMemoryRequirements * pInfo,  VkMemoryRequirements2 * pMemoryRequirements)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  ,const VkDeviceBufferMemoryRequirements * , VkMemoryRequirements2 * ))
        _android_vulkan_dlsym("vkGetDeviceBufferMemoryRequirements"))
            (device, pInfo, pMemoryRequirements);
}

VKAPI_ATTR void VKAPI_CALL vkGetDeviceImageMemoryRequirements( VkDevice  device, const VkDeviceImageMemoryRequirements * pInfo,  VkMemoryRequirements2 * pMemoryRequirements)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  ,const VkDeviceImageMemoryRequirements * , VkMemoryRequirements2 * ))
        _android_vulkan_dlsym("vkGetDeviceImageMemoryRequirements"))
            (device, pInfo, pMemoryRequirements);
}

VKAPI_ATTR void VKAPI_CALL vkGetDeviceImageSparseMemoryRequirements( VkDevice  device, const VkDeviceImageMemoryRequirements * pInfo,  uint32_t * pSparseMemoryRequirementCount,  VkSparseImageMemoryRequirements2 * pSparseMemoryRequirements)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  ,const VkDeviceImageMemoryRequirements * , uint32_t * , VkSparseImageMemoryRequirements2 * ))
        _android_vulkan_dlsym("vkGetDeviceImageSparseMemoryRequirements"))
            (device, pInfo, pSparseMemoryRequirementCount, pSparseMemoryRequirements);
}

#endif

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceSupportKHR( VkPhysicalDevice  physicalDevice,  uint32_t  queueFamilyIndex,  VkSurfaceKHR  surface,  VkBool32 * pSupported)
{
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR func;

    if (pSupported == NULL)
        return VK_ERROR_INITIALIZATION_FAILED;

#ifdef WANT_WAYLAND
    if (vulkan_wayland_has_mapping(surface)) {
        *pSupported = VK_TRUE;
        return VK_SUCCESS;
    }
#endif

    if (!vulkan_hal_device) {
        if (hybris_vulkan_hal_initialize() != 0)
            return VK_ERROR_INITIALIZATION_FAILED;
    }

    func = (PFN_vkGetPhysicalDeviceSurfaceSupportKHR)
        vulkan_hal_device->GetInstanceProcAddr(vulkan_instance, "vkGetPhysicalDeviceSurfaceSupportKHR");

    if (func == NULL) {
        *pSupported = VK_TRUE;
        return VK_SUCCESS;
    }

    return func(physicalDevice, queueFamilyIndex, surface, pSupported);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceCapabilitiesKHR( VkPhysicalDevice  physicalDevice,  VkSurfaceKHR  surface,  VkSurfaceCapabilitiesKHR * pSurfaceCapabilities)
{
    return ws_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, surface, pSurfaceCapabilities);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceFormatsKHR( VkPhysicalDevice  physicalDevice,  VkSurfaceKHR  surface,  uint32_t * pSurfaceFormatCount,  VkSurfaceFormatKHR * pSurfaceFormats)
{
    static const VkSurfaceFormatKHR formats[] = {
        { VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
        { VK_FORMAT_R8G8B8A8_SRGB,  VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
        { VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
        { VK_FORMAT_B8G8R8A8_SRGB,  VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
    };
    uint32_t available = sizeof(formats) / sizeof(formats[0]);
    uint32_t requested;
    uint32_t written;

    (void)physicalDevice;
    (void)surface;

    if (pSurfaceFormatCount == NULL)
        return VK_ERROR_INITIALIZATION_FAILED;

    if (pSurfaceFormats == NULL) {
        *pSurfaceFormatCount = available;
        return VK_SUCCESS;
    }

    requested = *pSurfaceFormatCount;
    written = requested < available ? requested : available;

    memcpy(pSurfaceFormats, formats, written * sizeof(VkSurfaceFormatKHR));
    *pSurfaceFormatCount = written;

    return written < available ? VK_INCOMPLETE : VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfacePresentModesKHR( VkPhysicalDevice  physicalDevice,  VkSurfaceKHR  surface,  uint32_t * pPresentModeCount,  VkPresentModeKHR * pPresentModes)
{
    static const VkPresentModeKHR modes[] = {
        VK_PRESENT_MODE_FIFO_KHR,
        VK_PRESENT_MODE_MAILBOX_KHR,
    };
    uint32_t available = sizeof(modes) / sizeof(modes[0]);
    uint32_t requested;
    uint32_t written;

    (void)physicalDevice;
    (void)surface;

    if (pPresentModeCount == NULL)
        return VK_ERROR_INITIALIZATION_FAILED;

    if (pPresentModes == NULL) {
        *pPresentModeCount = available;
        return VK_SUCCESS;
    }

    requested = *pPresentModeCount;
    written = requested < available ? requested : available;

    memcpy(pPresentModes, modes, written * sizeof(VkPresentModeKHR));
    *pPresentModeCount = written;

    return written < available ? VK_INCOMPLETE : VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateSwapchainKHR( VkDevice  device, const VkSwapchainCreateInfoKHR * pCreateInfo, const VkAllocationCallbacks * pAllocator,  VkSwapchainKHR * pSwapchain)
{
    return ws_vkCreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);
}

VKAPI_ATTR void VKAPI_CALL vkDestroySwapchainKHR( VkDevice  device,  VkSwapchainKHR  swapchain, const VkAllocationCallbacks * pAllocator)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  , VkSwapchainKHR  ,const VkAllocationCallbacks * ))
        _android_vulkan_dlsym("vkDestroySwapchainKHR"))
            (device, swapchain, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetSwapchainImagesKHR( VkDevice  device,  VkSwapchainKHR  swapchain,  uint32_t * pSwapchainImageCount,  VkImage * pSwapchainImages)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , VkSwapchainKHR  , uint32_t * , VkImage * ))
        _android_vulkan_dlsym("vkGetSwapchainImagesKHR"))
            (device, swapchain, pSwapchainImageCount, pSwapchainImages);
}

VKAPI_ATTR VkResult VKAPI_CALL vkAcquireNextImageKHR( VkDevice  device,  VkSwapchainKHR  swapchain,  uint64_t  timeout,  VkSemaphore  semaphore,  VkFence  fence,  uint32_t * pImageIndex)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , VkSwapchainKHR  , uint64_t  , VkSemaphore  , VkFence  , uint32_t * ))
        _android_vulkan_dlsym("vkAcquireNextImageKHR"))
            (device, swapchain, timeout, semaphore, fence, pImageIndex);
}

VKAPI_ATTR VkResult VKAPI_CALL vkQueuePresentKHR( VkQueue  queue, const VkPresentInfoKHR * pPresentInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkQueue  ,const VkPresentInfoKHR * ))
        _android_vulkan_dlsym("vkQueuePresentKHR"))
            (queue, pPresentInfo);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetDeviceGroupPresentCapabilitiesKHR( VkDevice  device,  VkDeviceGroupPresentCapabilitiesKHR * pDeviceGroupPresentCapabilities)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , VkDeviceGroupPresentCapabilitiesKHR * ))
        _android_vulkan_dlsym("vkGetDeviceGroupPresentCapabilitiesKHR"))
            (device, pDeviceGroupPresentCapabilities);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetDeviceGroupSurfacePresentModesKHR( VkDevice  device,  VkSurfaceKHR  surface,  VkDeviceGroupPresentModeFlagsKHR * pModes)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , VkSurfaceKHR  , VkDeviceGroupPresentModeFlagsKHR * ))
        _android_vulkan_dlsym("vkGetDeviceGroupSurfacePresentModesKHR"))
            (device, surface, pModes);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDevicePresentRectanglesKHR( VkPhysicalDevice  physicalDevice,  VkSurfaceKHR  surface,  uint32_t * pRectCount,  VkRect2D * pRects)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkPhysicalDevice  , VkSurfaceKHR  , uint32_t * , VkRect2D * ))
        _android_vulkan_dlsym("vkGetPhysicalDevicePresentRectanglesKHR"))
            (physicalDevice, surface, pRectCount, pRects);
}

VKAPI_ATTR VkResult VKAPI_CALL vkAcquireNextImage2KHR( VkDevice  device, const VkAcquireNextImageInfoKHR * pAcquireInfo,  uint32_t * pImageIndex)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  ,const VkAcquireNextImageInfoKHR * , uint32_t * ))
        _android_vulkan_dlsym("vkAcquireNextImage2KHR"))
            (device, pAcquireInfo, pImageIndex);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceDisplayPropertiesKHR( VkPhysicalDevice  physicalDevice,  uint32_t * pPropertyCount,  VkDisplayPropertiesKHR * pProperties)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkPhysicalDevice  , uint32_t * , VkDisplayPropertiesKHR * ))
        _android_vulkan_dlsym("vkGetPhysicalDeviceDisplayPropertiesKHR"))
            (physicalDevice, pPropertyCount, pProperties);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceDisplayPlanePropertiesKHR( VkPhysicalDevice  physicalDevice,  uint32_t * pPropertyCount,  VkDisplayPlanePropertiesKHR * pProperties)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkPhysicalDevice  , uint32_t * , VkDisplayPlanePropertiesKHR * ))
        _android_vulkan_dlsym("vkGetPhysicalDeviceDisplayPlanePropertiesKHR"))
            (physicalDevice, pPropertyCount, pProperties);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetDisplayPlaneSupportedDisplaysKHR( VkPhysicalDevice  physicalDevice,  uint32_t  planeIndex,  uint32_t * pDisplayCount,  VkDisplayKHR * pDisplays)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkPhysicalDevice  , uint32_t  , uint32_t * , VkDisplayKHR * ))
        _android_vulkan_dlsym("vkGetDisplayPlaneSupportedDisplaysKHR"))
            (physicalDevice, planeIndex, pDisplayCount, pDisplays);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetDisplayModePropertiesKHR( VkPhysicalDevice  physicalDevice,  VkDisplayKHR  display,  uint32_t * pPropertyCount,  VkDisplayModePropertiesKHR * pProperties)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkPhysicalDevice  , VkDisplayKHR  , uint32_t * , VkDisplayModePropertiesKHR * ))
        _android_vulkan_dlsym("vkGetDisplayModePropertiesKHR"))
            (physicalDevice, display, pPropertyCount, pProperties);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateDisplayModeKHR( VkPhysicalDevice  physicalDevice,  VkDisplayKHR  display, const VkDisplayModeCreateInfoKHR * pCreateInfo, const VkAllocationCallbacks * pAllocator,  VkDisplayModeKHR * pMode)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkPhysicalDevice  , VkDisplayKHR  ,const VkDisplayModeCreateInfoKHR * ,const VkAllocationCallbacks * , VkDisplayModeKHR * ))
        _android_vulkan_dlsym("vkCreateDisplayModeKHR"))
            (physicalDevice, display, pCreateInfo, pAllocator, pMode);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetDisplayPlaneCapabilitiesKHR( VkPhysicalDevice  physicalDevice,  VkDisplayModeKHR  mode,  uint32_t  planeIndex,  VkDisplayPlaneCapabilitiesKHR * pCapabilities)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkPhysicalDevice  , VkDisplayModeKHR  , uint32_t  , VkDisplayPlaneCapabilitiesKHR * ))
        _android_vulkan_dlsym("vkGetDisplayPlaneCapabilitiesKHR"))
            (physicalDevice, mode, planeIndex, pCapabilities);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateDisplayPlaneSurfaceKHR( VkInstance  instance, const VkDisplaySurfaceCreateInfoKHR * pCreateInfo, const VkAllocationCallbacks * pAllocator,  VkSurfaceKHR * pSurface)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkInstance  ,const VkDisplaySurfaceCreateInfoKHR * ,const VkAllocationCallbacks * , VkSurfaceKHR * ))
        _android_vulkan_dlsym("vkCreateDisplayPlaneSurfaceKHR"))
            (instance, pCreateInfo, pAllocator, pSurface);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateSharedSwapchainsKHR( VkDevice  device,  uint32_t  swapchainCount, const VkSwapchainCreateInfoKHR * pCreateInfos, const VkAllocationCallbacks * pAllocator,  VkSwapchainKHR * pSwapchains)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , uint32_t  ,const VkSwapchainCreateInfoKHR * ,const VkAllocationCallbacks * , VkSwapchainKHR * ))
        _android_vulkan_dlsym("vkCreateSharedSwapchainsKHR"))
            (device, swapchainCount, pCreateInfos, pAllocator, pSwapchains);
}

#if VK_HEADER_VERSION >= 238

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceVideoCapabilitiesKHR( VkPhysicalDevice  physicalDevice, const VkVideoProfileInfoKHR * pVideoProfile,  VkVideoCapabilitiesKHR * pCapabilities)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkPhysicalDevice  ,const VkVideoProfileInfoKHR * , VkVideoCapabilitiesKHR * ))
        _android_vulkan_dlsym("vkGetPhysicalDeviceVideoCapabilitiesKHR"))
            (physicalDevice, pVideoProfile, pCapabilities);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceVideoFormatPropertiesKHR( VkPhysicalDevice  physicalDevice, const VkPhysicalDeviceVideoFormatInfoKHR * pVideoFormatInfo,  uint32_t * pVideoFormatPropertyCount,  VkVideoFormatPropertiesKHR * pVideoFormatProperties)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkPhysicalDevice  ,const VkPhysicalDeviceVideoFormatInfoKHR * , uint32_t * , VkVideoFormatPropertiesKHR * ))
        _android_vulkan_dlsym("vkGetPhysicalDeviceVideoFormatPropertiesKHR"))
            (physicalDevice, pVideoFormatInfo, pVideoFormatPropertyCount, pVideoFormatProperties);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateVideoSessionKHR( VkDevice  device, const VkVideoSessionCreateInfoKHR * pCreateInfo, const VkAllocationCallbacks * pAllocator,  VkVideoSessionKHR * pVideoSession)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  ,const VkVideoSessionCreateInfoKHR * ,const VkAllocationCallbacks * , VkVideoSessionKHR * ))
        _android_vulkan_dlsym("vkCreateVideoSessionKHR"))
            (device, pCreateInfo, pAllocator, pVideoSession);
}

VKAPI_ATTR void VKAPI_CALL vkDestroyVideoSessionKHR( VkDevice  device,  VkVideoSessionKHR  videoSession, const VkAllocationCallbacks * pAllocator)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  , VkVideoSessionKHR  ,const VkAllocationCallbacks * ))
        _android_vulkan_dlsym("vkDestroyVideoSessionKHR"))
            (device, videoSession, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetVideoSessionMemoryRequirementsKHR( VkDevice  device,  VkVideoSessionKHR  videoSession,  uint32_t * pMemoryRequirementsCount,  VkVideoSessionMemoryRequirementsKHR * pMemoryRequirements)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , VkVideoSessionKHR  , uint32_t * , VkVideoSessionMemoryRequirementsKHR * ))
        _android_vulkan_dlsym("vkGetVideoSessionMemoryRequirementsKHR"))
            (device, videoSession, pMemoryRequirementsCount, pMemoryRequirements);
}

VKAPI_ATTR VkResult VKAPI_CALL vkBindVideoSessionMemoryKHR( VkDevice  device,  VkVideoSessionKHR  videoSession,  uint32_t  bindSessionMemoryInfoCount, const VkBindVideoSessionMemoryInfoKHR * pBindSessionMemoryInfos)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , VkVideoSessionKHR  , uint32_t  ,const VkBindVideoSessionMemoryInfoKHR * ))
        _android_vulkan_dlsym("vkBindVideoSessionMemoryKHR"))
            (device, videoSession, bindSessionMemoryInfoCount, pBindSessionMemoryInfos);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateVideoSessionParametersKHR( VkDevice  device, const VkVideoSessionParametersCreateInfoKHR * pCreateInfo, const VkAllocationCallbacks * pAllocator,  VkVideoSessionParametersKHR * pVideoSessionParameters)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  ,const VkVideoSessionParametersCreateInfoKHR * ,const VkAllocationCallbacks * , VkVideoSessionParametersKHR * ))
        _android_vulkan_dlsym("vkCreateVideoSessionParametersKHR"))
            (device, pCreateInfo, pAllocator, pVideoSessionParameters);
}

VKAPI_ATTR VkResult VKAPI_CALL vkUpdateVideoSessionParametersKHR( VkDevice  device,  VkVideoSessionParametersKHR  videoSessionParameters, const VkVideoSessionParametersUpdateInfoKHR * pUpdateInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , VkVideoSessionParametersKHR  ,const VkVideoSessionParametersUpdateInfoKHR * ))
        _android_vulkan_dlsym("vkUpdateVideoSessionParametersKHR"))
            (device, videoSessionParameters, pUpdateInfo);
}

VKAPI_ATTR void VKAPI_CALL vkDestroyVideoSessionParametersKHR( VkDevice  device,  VkVideoSessionParametersKHR  videoSessionParameters, const VkAllocationCallbacks * pAllocator)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  , VkVideoSessionParametersKHR  ,const VkAllocationCallbacks * ))
        _android_vulkan_dlsym("vkDestroyVideoSessionParametersKHR"))
            (device, videoSessionParameters, pAllocator);
}

VKAPI_ATTR void VKAPI_CALL vkCmdBeginVideoCodingKHR( VkCommandBuffer  commandBuffer, const VkVideoBeginCodingInfoKHR * pBeginInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  ,const VkVideoBeginCodingInfoKHR * ))
        _android_vulkan_dlsym("vkCmdBeginVideoCodingKHR"))
            (commandBuffer, pBeginInfo);
}

VKAPI_ATTR void VKAPI_CALL vkCmdEndVideoCodingKHR( VkCommandBuffer  commandBuffer, const VkVideoEndCodingInfoKHR * pEndCodingInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  ,const VkVideoEndCodingInfoKHR * ))
        _android_vulkan_dlsym("vkCmdEndVideoCodingKHR"))
            (commandBuffer, pEndCodingInfo);
}

VKAPI_ATTR void VKAPI_CALL vkCmdControlVideoCodingKHR( VkCommandBuffer  commandBuffer, const VkVideoCodingControlInfoKHR * pCodingControlInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  ,const VkVideoCodingControlInfoKHR * ))
        _android_vulkan_dlsym("vkCmdControlVideoCodingKHR"))
            (commandBuffer, pCodingControlInfo);
}

VKAPI_ATTR void VKAPI_CALL vkCmdDecodeVideoKHR( VkCommandBuffer  commandBuffer, const VkVideoDecodeInfoKHR * pDecodeInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  ,const VkVideoDecodeInfoKHR * ))
        _android_vulkan_dlsym("vkCmdDecodeVideoKHR"))
            (commandBuffer, pDecodeInfo);
}

#endif

#if VK_HEADER_VERSION >= 197

VKAPI_ATTR void VKAPI_CALL vkCmdBeginRenderingKHR( VkCommandBuffer  commandBuffer, const VkRenderingInfo * pRenderingInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  ,const VkRenderingInfo * ))
        _android_vulkan_dlsym("vkCmdBeginRenderingKHR"))
            (commandBuffer, pRenderingInfo);
}

VKAPI_ATTR void VKAPI_CALL vkCmdEndRenderingKHR( VkCommandBuffer  commandBuffer)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  ))
        _android_vulkan_dlsym("vkCmdEndRenderingKHR"))
            (commandBuffer);
}

#endif

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures2KHR( VkPhysicalDevice  physicalDevice,  VkPhysicalDeviceFeatures2 * pFeatures)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkPhysicalDevice  , VkPhysicalDeviceFeatures2 * ))
        _android_vulkan_dlsym("vkGetPhysicalDeviceFeatures2KHR"))
            (physicalDevice, pFeatures);
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties2KHR( VkPhysicalDevice  physicalDevice,  VkPhysicalDeviceProperties2 * pProperties)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkPhysicalDevice  , VkPhysicalDeviceProperties2 * ))
        _android_vulkan_dlsym("vkGetPhysicalDeviceProperties2KHR"))
            (physicalDevice, pProperties);
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFormatProperties2KHR( VkPhysicalDevice  physicalDevice,  VkFormat  format,  VkFormatProperties2 * pFormatProperties)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkPhysicalDevice  , VkFormat  , VkFormatProperties2 * ))
        _android_vulkan_dlsym("vkGetPhysicalDeviceFormatProperties2KHR"))
            (physicalDevice, format, pFormatProperties);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceImageFormatProperties2KHR( VkPhysicalDevice  physicalDevice, const VkPhysicalDeviceImageFormatInfo2 * pImageFormatInfo,  VkImageFormatProperties2 * pImageFormatProperties)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkPhysicalDevice  ,const VkPhysicalDeviceImageFormatInfo2 * , VkImageFormatProperties2 * ))
        _android_vulkan_dlsym("vkGetPhysicalDeviceImageFormatProperties2KHR"))
            (physicalDevice, pImageFormatInfo, pImageFormatProperties);
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceQueueFamilyProperties2KHR( VkPhysicalDevice  physicalDevice,  uint32_t * pQueueFamilyPropertyCount,  VkQueueFamilyProperties2 * pQueueFamilyProperties)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkPhysicalDevice  , uint32_t * , VkQueueFamilyProperties2 * ))
        _android_vulkan_dlsym("vkGetPhysicalDeviceQueueFamilyProperties2KHR"))
            (physicalDevice, pQueueFamilyPropertyCount, pQueueFamilyProperties);
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties2KHR( VkPhysicalDevice  physicalDevice,  VkPhysicalDeviceMemoryProperties2 * pMemoryProperties)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkPhysicalDevice  , VkPhysicalDeviceMemoryProperties2 * ))
        _android_vulkan_dlsym("vkGetPhysicalDeviceMemoryProperties2KHR"))
            (physicalDevice, pMemoryProperties);
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceSparseImageFormatProperties2KHR( VkPhysicalDevice  physicalDevice, const VkPhysicalDeviceSparseImageFormatInfo2 * pFormatInfo,  uint32_t * pPropertyCount,  VkSparseImageFormatProperties2 * pProperties)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkPhysicalDevice  ,const VkPhysicalDeviceSparseImageFormatInfo2 * , uint32_t * , VkSparseImageFormatProperties2 * ))
        _android_vulkan_dlsym("vkGetPhysicalDeviceSparseImageFormatProperties2KHR"))
            (physicalDevice, pFormatInfo, pPropertyCount, pProperties);
}

VKAPI_ATTR void VKAPI_CALL vkGetDeviceGroupPeerMemoryFeaturesKHR( VkDevice  device,  uint32_t  heapIndex,  uint32_t  localDeviceIndex,  uint32_t  remoteDeviceIndex,  VkPeerMemoryFeatureFlags * pPeerMemoryFeatures)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  , uint32_t  , uint32_t  , uint32_t  , VkPeerMemoryFeatureFlags * ))
        _android_vulkan_dlsym("vkGetDeviceGroupPeerMemoryFeaturesKHR"))
            (device, heapIndex, localDeviceIndex, remoteDeviceIndex, pPeerMemoryFeatures);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetDeviceMaskKHR( VkCommandBuffer  commandBuffer,  uint32_t  deviceMask)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , uint32_t  ))
        _android_vulkan_dlsym("vkCmdSetDeviceMaskKHR"))
            (commandBuffer, deviceMask);
}

VKAPI_ATTR void VKAPI_CALL vkCmdDispatchBaseKHR( VkCommandBuffer  commandBuffer,  uint32_t  baseGroupX,  uint32_t  baseGroupY,  uint32_t  baseGroupZ,  uint32_t  groupCountX,  uint32_t  groupCountY,  uint32_t  groupCountZ)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , uint32_t  , uint32_t  , uint32_t  , uint32_t  , uint32_t  , uint32_t  ))
        _android_vulkan_dlsym("vkCmdDispatchBaseKHR"))
            (commandBuffer, baseGroupX, baseGroupY, baseGroupZ, groupCountX, groupCountY, groupCountZ);
}

VKAPI_ATTR void VKAPI_CALL vkTrimCommandPoolKHR( VkDevice  device,  VkCommandPool  commandPool,  VkCommandPoolTrimFlags  flags)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  , VkCommandPool  , VkCommandPoolTrimFlags  ))
        _android_vulkan_dlsym("vkTrimCommandPoolKHR"))
            (device, commandPool, flags);
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumeratePhysicalDeviceGroupsKHR( VkInstance  instance,  uint32_t * pPhysicalDeviceGroupCount,  VkPhysicalDeviceGroupProperties * pPhysicalDeviceGroupProperties)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkInstance  , uint32_t * , VkPhysicalDeviceGroupProperties * ))
        _android_vulkan_dlsym("vkEnumeratePhysicalDeviceGroupsKHR"))
            (instance, pPhysicalDeviceGroupCount, pPhysicalDeviceGroupProperties);
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceExternalBufferPropertiesKHR( VkPhysicalDevice  physicalDevice, const VkPhysicalDeviceExternalBufferInfo * pExternalBufferInfo,  VkExternalBufferProperties * pExternalBufferProperties)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkPhysicalDevice  ,const VkPhysicalDeviceExternalBufferInfo * , VkExternalBufferProperties * ))
        _android_vulkan_dlsym("vkGetPhysicalDeviceExternalBufferPropertiesKHR"))
            (physicalDevice, pExternalBufferInfo, pExternalBufferProperties);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetMemoryFdKHR( VkDevice  device, const VkMemoryGetFdInfoKHR * pGetFdInfo,  int * pFd)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  ,const VkMemoryGetFdInfoKHR * , int * ))
        _android_vulkan_dlsym("vkGetMemoryFdKHR"))
            (device, pGetFdInfo, pFd);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetMemoryFdPropertiesKHR( VkDevice  device,  VkExternalMemoryHandleTypeFlagBits  handleType,  int  fd,  VkMemoryFdPropertiesKHR * pMemoryFdProperties)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , VkExternalMemoryHandleTypeFlagBits  , int  , VkMemoryFdPropertiesKHR * ))
        _android_vulkan_dlsym("vkGetMemoryFdPropertiesKHR"))
            (device, handleType, fd, pMemoryFdProperties);
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceExternalSemaphorePropertiesKHR( VkPhysicalDevice  physicalDevice, const VkPhysicalDeviceExternalSemaphoreInfo * pExternalSemaphoreInfo,  VkExternalSemaphoreProperties * pExternalSemaphoreProperties)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkPhysicalDevice  ,const VkPhysicalDeviceExternalSemaphoreInfo * , VkExternalSemaphoreProperties * ))
        _android_vulkan_dlsym("vkGetPhysicalDeviceExternalSemaphorePropertiesKHR"))
            (physicalDevice, pExternalSemaphoreInfo, pExternalSemaphoreProperties);
}

VKAPI_ATTR VkResult VKAPI_CALL vkImportSemaphoreFdKHR( VkDevice  device, const VkImportSemaphoreFdInfoKHR * pImportSemaphoreFdInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  ,const VkImportSemaphoreFdInfoKHR * ))
        _android_vulkan_dlsym("vkImportSemaphoreFdKHR"))
            (device, pImportSemaphoreFdInfo);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetSemaphoreFdKHR( VkDevice  device, const VkSemaphoreGetFdInfoKHR * pGetFdInfo,  int * pFd)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  ,const VkSemaphoreGetFdInfoKHR * , int * ))
        _android_vulkan_dlsym("vkGetSemaphoreFdKHR"))
            (device, pGetFdInfo, pFd);
}

VKAPI_ATTR void VKAPI_CALL vkCmdPushDescriptorSetKHR( VkCommandBuffer  commandBuffer,  VkPipelineBindPoint  pipelineBindPoint,  VkPipelineLayout  layout,  uint32_t  set,  uint32_t  descriptorWriteCount, const VkWriteDescriptorSet * pDescriptorWrites)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkPipelineBindPoint  , VkPipelineLayout  , uint32_t  , uint32_t  ,const VkWriteDescriptorSet * ))
        _android_vulkan_dlsym("vkCmdPushDescriptorSetKHR"))
            (commandBuffer, pipelineBindPoint, layout, set, descriptorWriteCount, pDescriptorWrites);
}

VKAPI_ATTR void VKAPI_CALL vkCmdPushDescriptorSetWithTemplateKHR( VkCommandBuffer  commandBuffer,  VkDescriptorUpdateTemplate  descriptorUpdateTemplate,  VkPipelineLayout  layout,  uint32_t  set, const void * pData)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkDescriptorUpdateTemplate  , VkPipelineLayout  , uint32_t  ,const void * ))
        _android_vulkan_dlsym("vkCmdPushDescriptorSetWithTemplateKHR"))
            (commandBuffer, descriptorUpdateTemplate, layout, set, pData);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateDescriptorUpdateTemplateKHR( VkDevice  device, const VkDescriptorUpdateTemplateCreateInfo * pCreateInfo, const VkAllocationCallbacks * pAllocator,  VkDescriptorUpdateTemplate * pDescriptorUpdateTemplate)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  ,const VkDescriptorUpdateTemplateCreateInfo * ,const VkAllocationCallbacks * , VkDescriptorUpdateTemplate * ))
        _android_vulkan_dlsym("vkCreateDescriptorUpdateTemplateKHR"))
            (device, pCreateInfo, pAllocator, pDescriptorUpdateTemplate);
}

VKAPI_ATTR void VKAPI_CALL vkDestroyDescriptorUpdateTemplateKHR( VkDevice  device,  VkDescriptorUpdateTemplate  descriptorUpdateTemplate, const VkAllocationCallbacks * pAllocator)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  , VkDescriptorUpdateTemplate  ,const VkAllocationCallbacks * ))
        _android_vulkan_dlsym("vkDestroyDescriptorUpdateTemplateKHR"))
            (device, descriptorUpdateTemplate, pAllocator);
}

VKAPI_ATTR void VKAPI_CALL vkUpdateDescriptorSetWithTemplateKHR( VkDevice  device,  VkDescriptorSet  descriptorSet,  VkDescriptorUpdateTemplate  descriptorUpdateTemplate, const void * pData)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  , VkDescriptorSet  , VkDescriptorUpdateTemplate  ,const void * ))
        _android_vulkan_dlsym("vkUpdateDescriptorSetWithTemplateKHR"))
            (device, descriptorSet, descriptorUpdateTemplate, pData);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateRenderPass2KHR( VkDevice  device, const VkRenderPassCreateInfo2 * pCreateInfo, const VkAllocationCallbacks * pAllocator,  VkRenderPass * pRenderPass)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  ,const VkRenderPassCreateInfo2 * ,const VkAllocationCallbacks * , VkRenderPass * ))
        _android_vulkan_dlsym("vkCreateRenderPass2KHR"))
            (device, pCreateInfo, pAllocator, pRenderPass);
}

VKAPI_ATTR void VKAPI_CALL vkCmdBeginRenderPass2KHR( VkCommandBuffer  commandBuffer, const VkRenderPassBeginInfo * pRenderPassBegin, const VkSubpassBeginInfo * pSubpassBeginInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  ,const VkRenderPassBeginInfo * ,const VkSubpassBeginInfo * ))
        _android_vulkan_dlsym("vkCmdBeginRenderPass2KHR"))
            (commandBuffer, pRenderPassBegin, pSubpassBeginInfo);
}

VKAPI_ATTR void VKAPI_CALL vkCmdNextSubpass2KHR( VkCommandBuffer  commandBuffer, const VkSubpassBeginInfo * pSubpassBeginInfo, const VkSubpassEndInfo * pSubpassEndInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  ,const VkSubpassBeginInfo * ,const VkSubpassEndInfo * ))
        _android_vulkan_dlsym("vkCmdNextSubpass2KHR"))
            (commandBuffer, pSubpassBeginInfo, pSubpassEndInfo);
}

VKAPI_ATTR void VKAPI_CALL vkCmdEndRenderPass2KHR( VkCommandBuffer  commandBuffer, const VkSubpassEndInfo * pSubpassEndInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  ,const VkSubpassEndInfo * ))
        _android_vulkan_dlsym("vkCmdEndRenderPass2KHR"))
            (commandBuffer, pSubpassEndInfo);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetSwapchainStatusKHR( VkDevice  device,  VkSwapchainKHR  swapchain)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , VkSwapchainKHR  ))
        _android_vulkan_dlsym("vkGetSwapchainStatusKHR"))
            (device, swapchain);
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceExternalFencePropertiesKHR( VkPhysicalDevice  physicalDevice, const VkPhysicalDeviceExternalFenceInfo * pExternalFenceInfo,  VkExternalFenceProperties * pExternalFenceProperties)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkPhysicalDevice  ,const VkPhysicalDeviceExternalFenceInfo * , VkExternalFenceProperties * ))
        _android_vulkan_dlsym("vkGetPhysicalDeviceExternalFencePropertiesKHR"))
            (physicalDevice, pExternalFenceInfo, pExternalFenceProperties);
}

VKAPI_ATTR VkResult VKAPI_CALL vkImportFenceFdKHR( VkDevice  device, const VkImportFenceFdInfoKHR * pImportFenceFdInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  ,const VkImportFenceFdInfoKHR * ))
        _android_vulkan_dlsym("vkImportFenceFdKHR"))
            (device, pImportFenceFdInfo);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetFenceFdKHR( VkDevice  device, const VkFenceGetFdInfoKHR * pGetFdInfo,  int * pFd)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  ,const VkFenceGetFdInfoKHR * , int * ))
        _android_vulkan_dlsym("vkGetFenceFdKHR"))
            (device, pGetFdInfo, pFd);
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumeratePhysicalDeviceQueueFamilyPerformanceQueryCountersKHR( VkPhysicalDevice  physicalDevice,  uint32_t  queueFamilyIndex,  uint32_t * pCounterCount,  VkPerformanceCounterKHR * pCounters,  VkPerformanceCounterDescriptionKHR * pCounterDescriptions)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkPhysicalDevice  , uint32_t  , uint32_t * , VkPerformanceCounterKHR * , VkPerformanceCounterDescriptionKHR * ))
        _android_vulkan_dlsym("vkEnumeratePhysicalDeviceQueueFamilyPerformanceQueryCountersKHR"))
            (physicalDevice, queueFamilyIndex, pCounterCount, pCounters, pCounterDescriptions);
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceQueueFamilyPerformanceQueryPassesKHR( VkPhysicalDevice  physicalDevice, const VkQueryPoolPerformanceCreateInfoKHR * pPerformanceQueryCreateInfo,  uint32_t * pNumPasses)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkPhysicalDevice  ,const VkQueryPoolPerformanceCreateInfoKHR * , uint32_t * ))
        _android_vulkan_dlsym("vkGetPhysicalDeviceQueueFamilyPerformanceQueryPassesKHR"))
            (physicalDevice, pPerformanceQueryCreateInfo, pNumPasses);
}

VKAPI_ATTR VkResult VKAPI_CALL vkAcquireProfilingLockKHR( VkDevice  device, const VkAcquireProfilingLockInfoKHR * pInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  ,const VkAcquireProfilingLockInfoKHR * ))
        _android_vulkan_dlsym("vkAcquireProfilingLockKHR"))
            (device, pInfo);
}

VKAPI_ATTR void VKAPI_CALL vkReleaseProfilingLockKHR( VkDevice  device)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  ))
        _android_vulkan_dlsym("vkReleaseProfilingLockKHR"))
            (device);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceCapabilities2KHR( VkPhysicalDevice  physicalDevice, const VkPhysicalDeviceSurfaceInfo2KHR * pSurfaceInfo,  VkSurfaceCapabilities2KHR * pSurfaceCapabilities)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkPhysicalDevice  ,const VkPhysicalDeviceSurfaceInfo2KHR * , VkSurfaceCapabilities2KHR * ))
        _android_vulkan_dlsym("vkGetPhysicalDeviceSurfaceCapabilities2KHR"))
            (physicalDevice, pSurfaceInfo, pSurfaceCapabilities);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceFormats2KHR( VkPhysicalDevice  physicalDevice, const VkPhysicalDeviceSurfaceInfo2KHR * pSurfaceInfo,  uint32_t * pSurfaceFormatCount,  VkSurfaceFormat2KHR * pSurfaceFormats)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkPhysicalDevice  ,const VkPhysicalDeviceSurfaceInfo2KHR * , uint32_t * , VkSurfaceFormat2KHR * ))
        _android_vulkan_dlsym("vkGetPhysicalDeviceSurfaceFormats2KHR"))
            (physicalDevice, pSurfaceInfo, pSurfaceFormatCount, pSurfaceFormats);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceDisplayProperties2KHR( VkPhysicalDevice  physicalDevice,  uint32_t * pPropertyCount,  VkDisplayProperties2KHR * pProperties)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkPhysicalDevice  , uint32_t * , VkDisplayProperties2KHR * ))
        _android_vulkan_dlsym("vkGetPhysicalDeviceDisplayProperties2KHR"))
            (physicalDevice, pPropertyCount, pProperties);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceDisplayPlaneProperties2KHR( VkPhysicalDevice  physicalDevice,  uint32_t * pPropertyCount,  VkDisplayPlaneProperties2KHR * pProperties)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkPhysicalDevice  , uint32_t * , VkDisplayPlaneProperties2KHR * ))
        _android_vulkan_dlsym("vkGetPhysicalDeviceDisplayPlaneProperties2KHR"))
            (physicalDevice, pPropertyCount, pProperties);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetDisplayModeProperties2KHR( VkPhysicalDevice  physicalDevice,  VkDisplayKHR  display,  uint32_t * pPropertyCount,  VkDisplayModeProperties2KHR * pProperties)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkPhysicalDevice  , VkDisplayKHR  , uint32_t * , VkDisplayModeProperties2KHR * ))
        _android_vulkan_dlsym("vkGetDisplayModeProperties2KHR"))
            (physicalDevice, display, pPropertyCount, pProperties);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetDisplayPlaneCapabilities2KHR( VkPhysicalDevice  physicalDevice, const VkDisplayPlaneInfo2KHR * pDisplayPlaneInfo,  VkDisplayPlaneCapabilities2KHR * pCapabilities)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkPhysicalDevice  ,const VkDisplayPlaneInfo2KHR * , VkDisplayPlaneCapabilities2KHR * ))
        _android_vulkan_dlsym("vkGetDisplayPlaneCapabilities2KHR"))
            (physicalDevice, pDisplayPlaneInfo, pCapabilities);
}

VKAPI_ATTR void VKAPI_CALL vkGetImageMemoryRequirements2KHR( VkDevice  device, const VkImageMemoryRequirementsInfo2 * pInfo,  VkMemoryRequirements2 * pMemoryRequirements)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  ,const VkImageMemoryRequirementsInfo2 * , VkMemoryRequirements2 * ))
        _android_vulkan_dlsym("vkGetImageMemoryRequirements2KHR"))
            (device, pInfo, pMemoryRequirements);
}

VKAPI_ATTR void VKAPI_CALL vkGetBufferMemoryRequirements2KHR( VkDevice  device, const VkBufferMemoryRequirementsInfo2 * pInfo,  VkMemoryRequirements2 * pMemoryRequirements)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  ,const VkBufferMemoryRequirementsInfo2 * , VkMemoryRequirements2 * ))
        _android_vulkan_dlsym("vkGetBufferMemoryRequirements2KHR"))
            (device, pInfo, pMemoryRequirements);
}

VKAPI_ATTR void VKAPI_CALL vkGetImageSparseMemoryRequirements2KHR( VkDevice  device, const VkImageSparseMemoryRequirementsInfo2 * pInfo,  uint32_t * pSparseMemoryRequirementCount,  VkSparseImageMemoryRequirements2 * pSparseMemoryRequirements)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  ,const VkImageSparseMemoryRequirementsInfo2 * , uint32_t * , VkSparseImageMemoryRequirements2 * ))
        _android_vulkan_dlsym("vkGetImageSparseMemoryRequirements2KHR"))
            (device, pInfo, pSparseMemoryRequirementCount, pSparseMemoryRequirements);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateSamplerYcbcrConversionKHR( VkDevice  device, const VkSamplerYcbcrConversionCreateInfo * pCreateInfo, const VkAllocationCallbacks * pAllocator,  VkSamplerYcbcrConversion * pYcbcrConversion)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  ,const VkSamplerYcbcrConversionCreateInfo * ,const VkAllocationCallbacks * , VkSamplerYcbcrConversion * ))
        _android_vulkan_dlsym("vkCreateSamplerYcbcrConversionKHR"))
            (device, pCreateInfo, pAllocator, pYcbcrConversion);
}

VKAPI_ATTR void VKAPI_CALL vkDestroySamplerYcbcrConversionKHR( VkDevice  device,  VkSamplerYcbcrConversion  ycbcrConversion, const VkAllocationCallbacks * pAllocator)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  , VkSamplerYcbcrConversion  ,const VkAllocationCallbacks * ))
        _android_vulkan_dlsym("vkDestroySamplerYcbcrConversionKHR"))
            (device, ycbcrConversion, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL vkBindBufferMemory2KHR( VkDevice  device,  uint32_t  bindInfoCount, const VkBindBufferMemoryInfo * pBindInfos)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , uint32_t  ,const VkBindBufferMemoryInfo * ))
        _android_vulkan_dlsym("vkBindBufferMemory2KHR"))
            (device, bindInfoCount, pBindInfos);
}

VKAPI_ATTR VkResult VKAPI_CALL vkBindImageMemory2KHR( VkDevice  device,  uint32_t  bindInfoCount, const VkBindImageMemoryInfo * pBindInfos)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , uint32_t  ,const VkBindImageMemoryInfo * ))
        _android_vulkan_dlsym("vkBindImageMemory2KHR"))
            (device, bindInfoCount, pBindInfos);
}

VKAPI_ATTR void VKAPI_CALL vkGetDescriptorSetLayoutSupportKHR( VkDevice  device, const VkDescriptorSetLayoutCreateInfo * pCreateInfo,  VkDescriptorSetLayoutSupport * pSupport)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  ,const VkDescriptorSetLayoutCreateInfo * , VkDescriptorSetLayoutSupport * ))
        _android_vulkan_dlsym("vkGetDescriptorSetLayoutSupportKHR"))
            (device, pCreateInfo, pSupport);
}

VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndirectCountKHR( VkCommandBuffer  commandBuffer,  VkBuffer  buffer,  VkDeviceSize  offset,  VkBuffer  countBuffer,  VkDeviceSize  countBufferOffset,  uint32_t  maxDrawCount,  uint32_t  stride)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkBuffer  , VkDeviceSize  , VkBuffer  , VkDeviceSize  , uint32_t  , uint32_t  ))
        _android_vulkan_dlsym("vkCmdDrawIndirectCountKHR"))
            (commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);
}

VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndexedIndirectCountKHR( VkCommandBuffer  commandBuffer,  VkBuffer  buffer,  VkDeviceSize  offset,  VkBuffer  countBuffer,  VkDeviceSize  countBufferOffset,  uint32_t  maxDrawCount,  uint32_t  stride)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkBuffer  , VkDeviceSize  , VkBuffer  , VkDeviceSize  , uint32_t  , uint32_t  ))
        _android_vulkan_dlsym("vkCmdDrawIndexedIndirectCountKHR"))
            (commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetSemaphoreCounterValueKHR( VkDevice  device,  VkSemaphore  semaphore,  uint64_t * pValue)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , VkSemaphore  , uint64_t * ))
        _android_vulkan_dlsym("vkGetSemaphoreCounterValueKHR"))
            (device, semaphore, pValue);
}

VKAPI_ATTR VkResult VKAPI_CALL vkWaitSemaphoresKHR( VkDevice  device, const VkSemaphoreWaitInfo * pWaitInfo,  uint64_t  timeout)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  ,const VkSemaphoreWaitInfo * , uint64_t  ))
        _android_vulkan_dlsym("vkWaitSemaphoresKHR"))
            (device, pWaitInfo, timeout);
}

VKAPI_ATTR VkResult VKAPI_CALL vkSignalSemaphoreKHR( VkDevice  device, const VkSemaphoreSignalInfo * pSignalInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  ,const VkSemaphoreSignalInfo * ))
        _android_vulkan_dlsym("vkSignalSemaphoreKHR"))
            (device, pSignalInfo);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceFragmentShadingRatesKHR( VkPhysicalDevice  physicalDevice,  uint32_t * pFragmentShadingRateCount,  VkPhysicalDeviceFragmentShadingRateKHR * pFragmentShadingRates)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkPhysicalDevice  , uint32_t * , VkPhysicalDeviceFragmentShadingRateKHR * ))
        _android_vulkan_dlsym("vkGetPhysicalDeviceFragmentShadingRatesKHR"))
            (physicalDevice, pFragmentShadingRateCount, pFragmentShadingRates);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetFragmentShadingRateKHR( VkCommandBuffer  commandBuffer, const VkExtent2D * pFragmentSize, const VkFragmentShadingRateCombinerOpKHR  combinerOps[2])
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  ,const VkExtent2D * ,const VkFragmentShadingRateCombinerOpKHR  [2]))
        _android_vulkan_dlsym("vkCmdSetFragmentShadingRateKHR"))
            (commandBuffer, pFragmentSize, combinerOps);
}

#if VK_HEADER_VERSION >= 276

VKAPI_ATTR void VKAPI_CALL vkCmdSetRenderingAttachmentLocationsKHR( VkCommandBuffer  commandBuffer, const VkRenderingAttachmentLocationInfo * pLocationInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  ,const VkRenderingAttachmentLocationInfo * ))
        _android_vulkan_dlsym("vkCmdSetRenderingAttachmentLocationsKHR"))
            (commandBuffer, pLocationInfo);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetRenderingInputAttachmentIndicesKHR( VkCommandBuffer  commandBuffer, const VkRenderingInputAttachmentIndexInfo * pInputAttachmentIndexInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  ,const VkRenderingInputAttachmentIndexInfo * ))
        _android_vulkan_dlsym("vkCmdSetRenderingInputAttachmentIndicesKHR"))
            (commandBuffer, pInputAttachmentIndexInfo);
}

#endif

#if VK_HEADER_VERSION >= 185

VKAPI_ATTR VkResult VKAPI_CALL vkWaitForPresentKHR( VkDevice  device,  VkSwapchainKHR  swapchain,  uint64_t  presentId,  uint64_t  timeout)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , VkSwapchainKHR  , uint64_t  , uint64_t  ))
        _android_vulkan_dlsym("vkWaitForPresentKHR"))
            (device, swapchain, presentId, timeout);
}

#endif

VKAPI_ATTR VkDeviceAddress VKAPI_CALL vkGetBufferDeviceAddressKHR( VkDevice  device, const VkBufferDeviceAddressInfo * pInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkDeviceAddress (*)( VkDevice  ,const VkBufferDeviceAddressInfo * ))
        _android_vulkan_dlsym("vkGetBufferDeviceAddressKHR"))
            (device, pInfo);
}

VKAPI_ATTR uint64_t VKAPI_CALL vkGetBufferOpaqueCaptureAddressKHR( VkDevice  device, const VkBufferDeviceAddressInfo * pInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((uint64_t (*)( VkDevice  ,const VkBufferDeviceAddressInfo * ))
        _android_vulkan_dlsym("vkGetBufferOpaqueCaptureAddressKHR"))
            (device, pInfo);
}

VKAPI_ATTR uint64_t VKAPI_CALL vkGetDeviceMemoryOpaqueCaptureAddressKHR( VkDevice  device, const VkDeviceMemoryOpaqueCaptureAddressInfo * pInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((uint64_t (*)( VkDevice  ,const VkDeviceMemoryOpaqueCaptureAddressInfo * ))
        _android_vulkan_dlsym("vkGetDeviceMemoryOpaqueCaptureAddressKHR"))
            (device, pInfo);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateDeferredOperationKHR( VkDevice  device, const VkAllocationCallbacks * pAllocator,  VkDeferredOperationKHR * pDeferredOperation)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  ,const VkAllocationCallbacks * , VkDeferredOperationKHR * ))
        _android_vulkan_dlsym("vkCreateDeferredOperationKHR"))
            (device, pAllocator, pDeferredOperation);
}

VKAPI_ATTR void VKAPI_CALL vkDestroyDeferredOperationKHR( VkDevice  device,  VkDeferredOperationKHR  operation, const VkAllocationCallbacks * pAllocator)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  , VkDeferredOperationKHR  ,const VkAllocationCallbacks * ))
        _android_vulkan_dlsym("vkDestroyDeferredOperationKHR"))
            (device, operation, pAllocator);
}

VKAPI_ATTR uint32_t VKAPI_CALL vkGetDeferredOperationMaxConcurrencyKHR( VkDevice  device,  VkDeferredOperationKHR  operation)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((uint32_t (*)( VkDevice  , VkDeferredOperationKHR  ))
        _android_vulkan_dlsym("vkGetDeferredOperationMaxConcurrencyKHR"))
            (device, operation);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetDeferredOperationResultKHR( VkDevice  device,  VkDeferredOperationKHR  operation)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , VkDeferredOperationKHR  ))
        _android_vulkan_dlsym("vkGetDeferredOperationResultKHR"))
            (device, operation);
}

VKAPI_ATTR VkResult VKAPI_CALL vkDeferredOperationJoinKHR( VkDevice  device,  VkDeferredOperationKHR  operation)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , VkDeferredOperationKHR  ))
        _android_vulkan_dlsym("vkDeferredOperationJoinKHR"))
            (device, operation);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPipelineExecutablePropertiesKHR( VkDevice  device, const VkPipelineInfoKHR * pPipelineInfo,  uint32_t * pExecutableCount,  VkPipelineExecutablePropertiesKHR * pProperties)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  ,const VkPipelineInfoKHR * , uint32_t * , VkPipelineExecutablePropertiesKHR * ))
        _android_vulkan_dlsym("vkGetPipelineExecutablePropertiesKHR"))
            (device, pPipelineInfo, pExecutableCount, pProperties);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPipelineExecutableStatisticsKHR( VkDevice  device, const VkPipelineExecutableInfoKHR * pExecutableInfo,  uint32_t * pStatisticCount,  VkPipelineExecutableStatisticKHR * pStatistics)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  ,const VkPipelineExecutableInfoKHR * , uint32_t * , VkPipelineExecutableStatisticKHR * ))
        _android_vulkan_dlsym("vkGetPipelineExecutableStatisticsKHR"))
            (device, pExecutableInfo, pStatisticCount, pStatistics);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPipelineExecutableInternalRepresentationsKHR( VkDevice  device, const VkPipelineExecutableInfoKHR * pExecutableInfo,  uint32_t * pInternalRepresentationCount,  VkPipelineExecutableInternalRepresentationKHR * pInternalRepresentations)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  ,const VkPipelineExecutableInfoKHR * , uint32_t * , VkPipelineExecutableInternalRepresentationKHR * ))
        _android_vulkan_dlsym("vkGetPipelineExecutableInternalRepresentationsKHR"))
            (device, pExecutableInfo, pInternalRepresentationCount, pInternalRepresentations);
}

#if VK_HEADER_VERSION >= 244

VKAPI_ATTR VkResult VKAPI_CALL vkMapMemory2KHR( VkDevice  device, const VkMemoryMapInfoKHR * pMemoryMapInfo,  void ** ppData)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  ,const VkMemoryMapInfoKHR * , void ** ))
        _android_vulkan_dlsym("vkMapMemory2KHR"))
            (device, pMemoryMapInfo, ppData);
}

VKAPI_ATTR VkResult VKAPI_CALL vkUnmapMemory2KHR( VkDevice  device, const VkMemoryUnmapInfoKHR * pMemoryUnmapInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  ,const VkMemoryUnmapInfoKHR * ))
        _android_vulkan_dlsym("vkUnmapMemory2KHR"))
            (device, pMemoryUnmapInfo);
}

#endif

#if VK_HEADER_VERSION >= 274

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceVideoEncodeQualityLevelPropertiesKHR( VkPhysicalDevice  physicalDevice, const VkPhysicalDeviceVideoEncodeQualityLevelInfoKHR * pQualityLevelInfo,  VkVideoEncodeQualityLevelPropertiesKHR * pQualityLevelProperties)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkPhysicalDevice  ,const VkPhysicalDeviceVideoEncodeQualityLevelInfoKHR * , VkVideoEncodeQualityLevelPropertiesKHR * ))
        _android_vulkan_dlsym("vkGetPhysicalDeviceVideoEncodeQualityLevelPropertiesKHR"))
            (physicalDevice, pQualityLevelInfo, pQualityLevelProperties);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetEncodedVideoSessionParametersKHR( VkDevice  device, const VkVideoEncodeSessionParametersGetInfoKHR * pVideoSessionParametersInfo,  VkVideoEncodeSessionParametersFeedbackInfoKHR * pFeedbackInfo,  size_t * pDataSize,  void * pData)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  ,const VkVideoEncodeSessionParametersGetInfoKHR * , VkVideoEncodeSessionParametersFeedbackInfoKHR * , size_t * , void * ))
        _android_vulkan_dlsym("vkGetEncodedVideoSessionParametersKHR"))
            (device, pVideoSessionParametersInfo, pFeedbackInfo, pDataSize, pData);
}

VKAPI_ATTR void VKAPI_CALL vkCmdEncodeVideoKHR( VkCommandBuffer  commandBuffer, const VkVideoEncodeInfoKHR * pEncodeInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  ,const VkVideoEncodeInfoKHR * ))
        _android_vulkan_dlsym("vkCmdEncodeVideoKHR"))
            (commandBuffer, pEncodeInfo);
}

#endif

VKAPI_ATTR void VKAPI_CALL vkCmdSetEvent2KHR( VkCommandBuffer  commandBuffer,  VkEvent  event, const VkDependencyInfo * pDependencyInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkEvent  ,const VkDependencyInfo * ))
        _android_vulkan_dlsym("vkCmdSetEvent2KHR"))
            (commandBuffer, event, pDependencyInfo);
}

VKAPI_ATTR void VKAPI_CALL vkCmdResetEvent2KHR( VkCommandBuffer  commandBuffer,  VkEvent  event,  VkPipelineStageFlags2  stageMask)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkEvent  , VkPipelineStageFlags2  ))
        _android_vulkan_dlsym("vkCmdResetEvent2KHR"))
            (commandBuffer, event, stageMask);
}

VKAPI_ATTR void VKAPI_CALL vkCmdWaitEvents2KHR( VkCommandBuffer  commandBuffer,  uint32_t  eventCount, const VkEvent * pEvents, const VkDependencyInfo * pDependencyInfos)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , uint32_t  ,const VkEvent * ,const VkDependencyInfo * ))
        _android_vulkan_dlsym("vkCmdWaitEvents2KHR"))
            (commandBuffer, eventCount, pEvents, pDependencyInfos);
}

VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier2KHR( VkCommandBuffer  commandBuffer, const VkDependencyInfo * pDependencyInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  ,const VkDependencyInfo * ))
        _android_vulkan_dlsym("vkCmdPipelineBarrier2KHR"))
            (commandBuffer, pDependencyInfo);
}

VKAPI_ATTR void VKAPI_CALL vkCmdWriteTimestamp2KHR( VkCommandBuffer  commandBuffer,  VkPipelineStageFlags2  stage,  VkQueryPool  queryPool,  uint32_t  query)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkPipelineStageFlags2  , VkQueryPool  , uint32_t  ))
        _android_vulkan_dlsym("vkCmdWriteTimestamp2KHR"))
            (commandBuffer, stage, queryPool, query);
}

VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit2KHR( VkQueue  queue,  uint32_t  submitCount, const VkSubmitInfo2 * pSubmits,  VkFence  fence)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkQueue  , uint32_t  ,const VkSubmitInfo2 * , VkFence  ))
        _android_vulkan_dlsym("vkQueueSubmit2KHR"))
            (queue, submitCount, pSubmits, fence);
}

VKAPI_ATTR void VKAPI_CALL vkCmdWriteBufferMarker2AMD( VkCommandBuffer  commandBuffer,  VkPipelineStageFlags2  stage,  VkBuffer  dstBuffer,  VkDeviceSize  dstOffset,  uint32_t  marker)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkPipelineStageFlags2  , VkBuffer  , VkDeviceSize  , uint32_t  ))
        _android_vulkan_dlsym("vkCmdWriteBufferMarker2AMD"))
            (commandBuffer, stage, dstBuffer, dstOffset, marker);
}

VKAPI_ATTR void VKAPI_CALL vkGetQueueCheckpointData2NV( VkQueue  queue,  uint32_t * pCheckpointDataCount,  VkCheckpointData2NV * pCheckpointData)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkQueue  , uint32_t * , VkCheckpointData2NV * ))
        _android_vulkan_dlsym("vkGetQueueCheckpointData2NV"))
            (queue, pCheckpointDataCount, pCheckpointData);
}

VKAPI_ATTR void VKAPI_CALL vkCmdCopyBuffer2KHR( VkCommandBuffer  commandBuffer, const VkCopyBufferInfo2 * pCopyBufferInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  ,const VkCopyBufferInfo2 * ))
        _android_vulkan_dlsym("vkCmdCopyBuffer2KHR"))
            (commandBuffer, pCopyBufferInfo);
}

VKAPI_ATTR void VKAPI_CALL vkCmdCopyImage2KHR( VkCommandBuffer  commandBuffer, const VkCopyImageInfo2 * pCopyImageInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  ,const VkCopyImageInfo2 * ))
        _android_vulkan_dlsym("vkCmdCopyImage2KHR"))
            (commandBuffer, pCopyImageInfo);
}

VKAPI_ATTR void VKAPI_CALL vkCmdCopyBufferToImage2KHR( VkCommandBuffer  commandBuffer, const VkCopyBufferToImageInfo2 * pCopyBufferToImageInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  ,const VkCopyBufferToImageInfo2 * ))
        _android_vulkan_dlsym("vkCmdCopyBufferToImage2KHR"))
            (commandBuffer, pCopyBufferToImageInfo);
}

VKAPI_ATTR void VKAPI_CALL vkCmdCopyImageToBuffer2KHR( VkCommandBuffer  commandBuffer, const VkCopyImageToBufferInfo2 * pCopyImageToBufferInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  ,const VkCopyImageToBufferInfo2 * ))
        _android_vulkan_dlsym("vkCmdCopyImageToBuffer2KHR"))
            (commandBuffer, pCopyImageToBufferInfo);
}

VKAPI_ATTR void VKAPI_CALL vkCmdBlitImage2KHR( VkCommandBuffer  commandBuffer, const VkBlitImageInfo2 * pBlitImageInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  ,const VkBlitImageInfo2 * ))
        _android_vulkan_dlsym("vkCmdBlitImage2KHR"))
            (commandBuffer, pBlitImageInfo);
}

VKAPI_ATTR void VKAPI_CALL vkCmdResolveImage2KHR( VkCommandBuffer  commandBuffer, const VkResolveImageInfo2 * pResolveImageInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  ,const VkResolveImageInfo2 * ))
        _android_vulkan_dlsym("vkCmdResolveImage2KHR"))
            (commandBuffer, pResolveImageInfo);
}

#if VK_HEADER_VERSION >= 213

VKAPI_ATTR void VKAPI_CALL vkCmdTraceRaysIndirect2KHR( VkCommandBuffer  commandBuffer,  VkDeviceAddress  indirectDeviceAddress)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkDeviceAddress  ))
        _android_vulkan_dlsym("vkCmdTraceRaysIndirect2KHR"))
            (commandBuffer, indirectDeviceAddress);
}

#endif

#if VK_HEADER_VERSION >= 195

VKAPI_ATTR void VKAPI_CALL vkGetDeviceBufferMemoryRequirementsKHR( VkDevice  device, const VkDeviceBufferMemoryRequirements * pInfo,  VkMemoryRequirements2 * pMemoryRequirements)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  ,const VkDeviceBufferMemoryRequirements * , VkMemoryRequirements2 * ))
        _android_vulkan_dlsym("vkGetDeviceBufferMemoryRequirementsKHR"))
            (device, pInfo, pMemoryRequirements);
}

VKAPI_ATTR void VKAPI_CALL vkGetDeviceImageMemoryRequirementsKHR( VkDevice  device, const VkDeviceImageMemoryRequirements * pInfo,  VkMemoryRequirements2 * pMemoryRequirements)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  ,const VkDeviceImageMemoryRequirements * , VkMemoryRequirements2 * ))
        _android_vulkan_dlsym("vkGetDeviceImageMemoryRequirementsKHR"))
            (device, pInfo, pMemoryRequirements);
}

VKAPI_ATTR void VKAPI_CALL vkGetDeviceImageSparseMemoryRequirementsKHR( VkDevice  device, const VkDeviceImageMemoryRequirements * pInfo,  uint32_t * pSparseMemoryRequirementCount,  VkSparseImageMemoryRequirements2 * pSparseMemoryRequirements)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  ,const VkDeviceImageMemoryRequirements * , uint32_t * , VkSparseImageMemoryRequirements2 * ))
        _android_vulkan_dlsym("vkGetDeviceImageSparseMemoryRequirementsKHR"))
            (device, pInfo, pSparseMemoryRequirementCount, pSparseMemoryRequirements);
}

#endif

#if VK_HEADER_VERSION >= 260

VKAPI_ATTR void VKAPI_CALL vkCmdBindIndexBuffer2KHR( VkCommandBuffer  commandBuffer,  VkBuffer  buffer,  VkDeviceSize  offset,  VkDeviceSize  size,  VkIndexType  indexType)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkBuffer  , VkDeviceSize  , VkDeviceSize  , VkIndexType  ))
        _android_vulkan_dlsym("vkCmdBindIndexBuffer2KHR"))
            (commandBuffer, buffer, offset, size, indexType);
}

VKAPI_ATTR void VKAPI_CALL vkGetRenderingAreaGranularityKHR( VkDevice  device, const VkRenderingAreaInfoKHR * pRenderingAreaInfo,  VkExtent2D * pGranularity)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  ,const VkRenderingAreaInfoKHR * , VkExtent2D * ))
        _android_vulkan_dlsym("vkGetRenderingAreaGranularityKHR"))
            (device, pRenderingAreaInfo, pGranularity);
}

VKAPI_ATTR void VKAPI_CALL vkGetDeviceImageSubresourceLayoutKHR( VkDevice  device, const VkDeviceImageSubresourceInfoKHR * pInfo,  VkSubresourceLayout2KHR * pLayout)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  ,const VkDeviceImageSubresourceInfoKHR * , VkSubresourceLayout2KHR * ))
        _android_vulkan_dlsym("vkGetDeviceImageSubresourceLayoutKHR"))
            (device, pInfo, pLayout);
}

VKAPI_ATTR void VKAPI_CALL vkGetImageSubresourceLayout2KHR( VkDevice  device,  VkImage  image, const VkImageSubresource2KHR * pSubresource,  VkSubresourceLayout2KHR * pLayout)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  , VkImage  ,const VkImageSubresource2KHR * , VkSubresourceLayout2KHR * ))
        _android_vulkan_dlsym("vkGetImageSubresourceLayout2KHR"))
            (device, image, pSubresource, pLayout);
}

#endif

#if VK_HEADER_VERSION >= 255

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR( VkPhysicalDevice  physicalDevice,  uint32_t * pPropertyCount,  VkCooperativeMatrixPropertiesKHR * pProperties)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkPhysicalDevice  , uint32_t * , VkCooperativeMatrixPropertiesKHR * ))
        _android_vulkan_dlsym("vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR"))
            (physicalDevice, pPropertyCount, pProperties);
}

#endif

#if VK_HEADER_VERSION >= 276

VKAPI_ATTR void VKAPI_CALL vkCmdSetLineStippleKHR( VkCommandBuffer  commandBuffer,  uint32_t  lineStippleFactor,  uint16_t  lineStipplePattern)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , uint32_t  , uint16_t  ))
        _android_vulkan_dlsym("vkCmdSetLineStippleKHR"))
            (commandBuffer, lineStippleFactor, lineStipplePattern);
}

#endif

#if VK_HEADER_VERSION >= 273

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceCalibrateableTimeDomainsKHR( VkPhysicalDevice  physicalDevice,  uint32_t * pTimeDomainCount,  VkTimeDomainKHR * pTimeDomains)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkPhysicalDevice  , uint32_t * , VkTimeDomainKHR * ))
        _android_vulkan_dlsym("vkGetPhysicalDeviceCalibrateableTimeDomainsKHR"))
            (physicalDevice, pTimeDomainCount, pTimeDomains);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetCalibratedTimestampsKHR( VkDevice  device,  uint32_t  timestampCount, const VkCalibratedTimestampInfoKHR * pTimestampInfos,  uint64_t * pTimestamps,  uint64_t * pMaxDeviation)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , uint32_t  ,const VkCalibratedTimestampInfoKHR * , uint64_t * , uint64_t * ))
        _android_vulkan_dlsym("vkGetCalibratedTimestampsKHR"))
            (device, timestampCount, pTimestampInfos, pTimestamps, pMaxDeviation);
}

#endif

#if VK_HEADER_VERSION >= 274

VKAPI_ATTR void VKAPI_CALL vkCmdBindDescriptorSets2KHR( VkCommandBuffer  commandBuffer, const VkBindDescriptorSetsInfoKHR * pBindDescriptorSetsInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  ,const VkBindDescriptorSetsInfoKHR * ))
        _android_vulkan_dlsym("vkCmdBindDescriptorSets2KHR"))
            (commandBuffer, pBindDescriptorSetsInfo);
}

VKAPI_ATTR void VKAPI_CALL vkCmdPushConstants2KHR( VkCommandBuffer  commandBuffer, const VkPushConstantsInfoKHR * pPushConstantsInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  ,const VkPushConstantsInfoKHR * ))
        _android_vulkan_dlsym("vkCmdPushConstants2KHR"))
            (commandBuffer, pPushConstantsInfo);
}

VKAPI_ATTR void VKAPI_CALL vkCmdPushDescriptorSet2KHR( VkCommandBuffer  commandBuffer, const VkPushDescriptorSetInfoKHR * pPushDescriptorSetInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  ,const VkPushDescriptorSetInfoKHR * ))
        _android_vulkan_dlsym("vkCmdPushDescriptorSet2KHR"))
            (commandBuffer, pPushDescriptorSetInfo);
}

VKAPI_ATTR void VKAPI_CALL vkCmdPushDescriptorSetWithTemplate2KHR( VkCommandBuffer  commandBuffer, const VkPushDescriptorSetWithTemplateInfoKHR * pPushDescriptorSetWithTemplateInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  ,const VkPushDescriptorSetWithTemplateInfoKHR * ))
        _android_vulkan_dlsym("vkCmdPushDescriptorSetWithTemplate2KHR"))
            (commandBuffer, pPushDescriptorSetWithTemplateInfo);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetDescriptorBufferOffsets2EXT( VkCommandBuffer  commandBuffer, const VkSetDescriptorBufferOffsetsInfoEXT * pSetDescriptorBufferOffsetsInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  ,const VkSetDescriptorBufferOffsetsInfoEXT * ))
        _android_vulkan_dlsym("vkCmdSetDescriptorBufferOffsets2EXT"))
            (commandBuffer, pSetDescriptorBufferOffsetsInfo);
}

VKAPI_ATTR void VKAPI_CALL vkCmdBindDescriptorBufferEmbeddedSamplers2EXT( VkCommandBuffer  commandBuffer, const VkBindDescriptorBufferEmbeddedSamplersInfoEXT * pBindDescriptorBufferEmbeddedSamplersInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  ,const VkBindDescriptorBufferEmbeddedSamplersInfoEXT * ))
        _android_vulkan_dlsym("vkCmdBindDescriptorBufferEmbeddedSamplers2EXT"))
            (commandBuffer, pBindDescriptorBufferEmbeddedSamplersInfo);
}

#endif

VKAPI_ATTR VkResult VKAPI_CALL vkCreateDebugReportCallbackEXT( VkInstance  instance, const VkDebugReportCallbackCreateInfoEXT * pCreateInfo, const VkAllocationCallbacks * pAllocator,  VkDebugReportCallbackEXT * pCallback)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkInstance  ,const VkDebugReportCallbackCreateInfoEXT * ,const VkAllocationCallbacks * , VkDebugReportCallbackEXT * ))
        _android_vulkan_dlsym("vkCreateDebugReportCallbackEXT"))
            (instance, pCreateInfo, pAllocator, pCallback);
}

VKAPI_ATTR void VKAPI_CALL vkDestroyDebugReportCallbackEXT( VkInstance  instance,  VkDebugReportCallbackEXT  callback, const VkAllocationCallbacks * pAllocator)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkInstance  , VkDebugReportCallbackEXT  ,const VkAllocationCallbacks * ))
        _android_vulkan_dlsym("vkDestroyDebugReportCallbackEXT"))
            (instance, callback, pAllocator);
}

VKAPI_ATTR void VKAPI_CALL vkDebugReportMessageEXT( VkInstance  instance,  VkDebugReportFlagsEXT  flags,  VkDebugReportObjectTypeEXT  objectType,  uint64_t  object,  size_t  location,  int32_t  messageCode, const char * pLayerPrefix, const char * pMessage)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkInstance  , VkDebugReportFlagsEXT  , VkDebugReportObjectTypeEXT  , uint64_t  , size_t  , int32_t  ,const char * ,const char * ))
        _android_vulkan_dlsym("vkDebugReportMessageEXT"))
            (instance, flags, objectType, object, location, messageCode, pLayerPrefix, pMessage);
}

VKAPI_ATTR VkResult VKAPI_CALL vkDebugMarkerSetObjectTagEXT( VkDevice  device, const VkDebugMarkerObjectTagInfoEXT * pTagInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  ,const VkDebugMarkerObjectTagInfoEXT * ))
        _android_vulkan_dlsym("vkDebugMarkerSetObjectTagEXT"))
            (device, pTagInfo);
}

VKAPI_ATTR VkResult VKAPI_CALL vkDebugMarkerSetObjectNameEXT( VkDevice  device, const VkDebugMarkerObjectNameInfoEXT * pNameInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  ,const VkDebugMarkerObjectNameInfoEXT * ))
        _android_vulkan_dlsym("vkDebugMarkerSetObjectNameEXT"))
            (device, pNameInfo);
}

VKAPI_ATTR void VKAPI_CALL vkCmdDebugMarkerBeginEXT( VkCommandBuffer  commandBuffer, const VkDebugMarkerMarkerInfoEXT * pMarkerInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  ,const VkDebugMarkerMarkerInfoEXT * ))
        _android_vulkan_dlsym("vkCmdDebugMarkerBeginEXT"))
            (commandBuffer, pMarkerInfo);
}

VKAPI_ATTR void VKAPI_CALL vkCmdDebugMarkerEndEXT( VkCommandBuffer  commandBuffer)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  ))
        _android_vulkan_dlsym("vkCmdDebugMarkerEndEXT"))
            (commandBuffer);
}

VKAPI_ATTR void VKAPI_CALL vkCmdDebugMarkerInsertEXT( VkCommandBuffer  commandBuffer, const VkDebugMarkerMarkerInfoEXT * pMarkerInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  ,const VkDebugMarkerMarkerInfoEXT * ))
        _android_vulkan_dlsym("vkCmdDebugMarkerInsertEXT"))
            (commandBuffer, pMarkerInfo);
}

VKAPI_ATTR void VKAPI_CALL vkCmdBindTransformFeedbackBuffersEXT( VkCommandBuffer  commandBuffer,  uint32_t  firstBinding,  uint32_t  bindingCount, const VkBuffer * pBuffers, const VkDeviceSize * pOffsets, const VkDeviceSize * pSizes)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , uint32_t  , uint32_t  ,const VkBuffer * ,const VkDeviceSize * ,const VkDeviceSize * ))
        _android_vulkan_dlsym("vkCmdBindTransformFeedbackBuffersEXT"))
            (commandBuffer, firstBinding, bindingCount, pBuffers, pOffsets, pSizes);
}

VKAPI_ATTR void VKAPI_CALL vkCmdBeginTransformFeedbackEXT( VkCommandBuffer  commandBuffer,  uint32_t  firstCounterBuffer,  uint32_t  counterBufferCount, const VkBuffer * pCounterBuffers, const VkDeviceSize * pCounterBufferOffsets)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , uint32_t  , uint32_t  ,const VkBuffer * ,const VkDeviceSize * ))
        _android_vulkan_dlsym("vkCmdBeginTransformFeedbackEXT"))
            (commandBuffer, firstCounterBuffer, counterBufferCount, pCounterBuffers, pCounterBufferOffsets);
}

VKAPI_ATTR void VKAPI_CALL vkCmdEndTransformFeedbackEXT( VkCommandBuffer  commandBuffer,  uint32_t  firstCounterBuffer,  uint32_t  counterBufferCount, const VkBuffer * pCounterBuffers, const VkDeviceSize * pCounterBufferOffsets)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , uint32_t  , uint32_t  ,const VkBuffer * ,const VkDeviceSize * ))
        _android_vulkan_dlsym("vkCmdEndTransformFeedbackEXT"))
            (commandBuffer, firstCounterBuffer, counterBufferCount, pCounterBuffers, pCounterBufferOffsets);
}

VKAPI_ATTR void VKAPI_CALL vkCmdBeginQueryIndexedEXT( VkCommandBuffer  commandBuffer,  VkQueryPool  queryPool,  uint32_t  query,  VkQueryControlFlags  flags,  uint32_t  index)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkQueryPool  , uint32_t  , VkQueryControlFlags  , uint32_t  ))
        _android_vulkan_dlsym("vkCmdBeginQueryIndexedEXT"))
            (commandBuffer, queryPool, query, flags, index);
}

VKAPI_ATTR void VKAPI_CALL vkCmdEndQueryIndexedEXT( VkCommandBuffer  commandBuffer,  VkQueryPool  queryPool,  uint32_t  query,  uint32_t  index)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkQueryPool  , uint32_t  , uint32_t  ))
        _android_vulkan_dlsym("vkCmdEndQueryIndexedEXT"))
            (commandBuffer, queryPool, query, index);
}

VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndirectByteCountEXT( VkCommandBuffer  commandBuffer,  uint32_t  instanceCount,  uint32_t  firstInstance,  VkBuffer  counterBuffer,  VkDeviceSize  counterBufferOffset,  uint32_t  counterOffset,  uint32_t  vertexStride)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , uint32_t  , uint32_t  , VkBuffer  , VkDeviceSize  , uint32_t  , uint32_t  ))
        _android_vulkan_dlsym("vkCmdDrawIndirectByteCountEXT"))
            (commandBuffer, instanceCount, firstInstance, counterBuffer, counterBufferOffset, counterOffset, vertexStride);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateCuModuleNVX( VkDevice  device, const VkCuModuleCreateInfoNVX * pCreateInfo, const VkAllocationCallbacks * pAllocator,  VkCuModuleNVX * pModule)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  ,const VkCuModuleCreateInfoNVX * ,const VkAllocationCallbacks * , VkCuModuleNVX * ))
        _android_vulkan_dlsym("vkCreateCuModuleNVX"))
            (device, pCreateInfo, pAllocator, pModule);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateCuFunctionNVX( VkDevice  device, const VkCuFunctionCreateInfoNVX * pCreateInfo, const VkAllocationCallbacks * pAllocator,  VkCuFunctionNVX * pFunction)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  ,const VkCuFunctionCreateInfoNVX * ,const VkAllocationCallbacks * , VkCuFunctionNVX * ))
        _android_vulkan_dlsym("vkCreateCuFunctionNVX"))
            (device, pCreateInfo, pAllocator, pFunction);
}

VKAPI_ATTR void VKAPI_CALL vkDestroyCuModuleNVX( VkDevice  device,  VkCuModuleNVX  module, const VkAllocationCallbacks * pAllocator)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  , VkCuModuleNVX  ,const VkAllocationCallbacks * ))
        _android_vulkan_dlsym("vkDestroyCuModuleNVX"))
            (device, module, pAllocator);
}

VKAPI_ATTR void VKAPI_CALL vkDestroyCuFunctionNVX( VkDevice  device,  VkCuFunctionNVX  function, const VkAllocationCallbacks * pAllocator)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  , VkCuFunctionNVX  ,const VkAllocationCallbacks * ))
        _android_vulkan_dlsym("vkDestroyCuFunctionNVX"))
            (device, function, pAllocator);
}

VKAPI_ATTR void VKAPI_CALL vkCmdCuLaunchKernelNVX( VkCommandBuffer  commandBuffer, const VkCuLaunchInfoNVX * pLaunchInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  ,const VkCuLaunchInfoNVX * ))
        _android_vulkan_dlsym("vkCmdCuLaunchKernelNVX"))
            (commandBuffer, pLaunchInfo);
}

VKAPI_ATTR uint32_t VKAPI_CALL vkGetImageViewHandleNVX( VkDevice  device, const VkImageViewHandleInfoNVX * pInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((uint32_t (*)( VkDevice  ,const VkImageViewHandleInfoNVX * ))
        _android_vulkan_dlsym("vkGetImageViewHandleNVX"))
            (device, pInfo);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetImageViewAddressNVX( VkDevice  device,  VkImageView  imageView,  VkImageViewAddressPropertiesNVX * pProperties)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , VkImageView  , VkImageViewAddressPropertiesNVX * ))
        _android_vulkan_dlsym("vkGetImageViewAddressNVX"))
            (device, imageView, pProperties);
}

VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndirectCountAMD( VkCommandBuffer  commandBuffer,  VkBuffer  buffer,  VkDeviceSize  offset,  VkBuffer  countBuffer,  VkDeviceSize  countBufferOffset,  uint32_t  maxDrawCount,  uint32_t  stride)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkBuffer  , VkDeviceSize  , VkBuffer  , VkDeviceSize  , uint32_t  , uint32_t  ))
        _android_vulkan_dlsym("vkCmdDrawIndirectCountAMD"))
            (commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);
}

VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndexedIndirectCountAMD( VkCommandBuffer  commandBuffer,  VkBuffer  buffer,  VkDeviceSize  offset,  VkBuffer  countBuffer,  VkDeviceSize  countBufferOffset,  uint32_t  maxDrawCount,  uint32_t  stride)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkBuffer  , VkDeviceSize  , VkBuffer  , VkDeviceSize  , uint32_t  , uint32_t  ))
        _android_vulkan_dlsym("vkCmdDrawIndexedIndirectCountAMD"))
            (commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetShaderInfoAMD( VkDevice  device,  VkPipeline  pipeline,  VkShaderStageFlagBits  shaderStage,  VkShaderInfoTypeAMD  infoType,  size_t * pInfoSize,  void * pInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , VkPipeline  , VkShaderStageFlagBits  , VkShaderInfoTypeAMD  , size_t * , void * ))
        _android_vulkan_dlsym("vkGetShaderInfoAMD"))
            (device, pipeline, shaderStage, infoType, pInfoSize, pInfo);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceExternalImageFormatPropertiesNV( VkPhysicalDevice  physicalDevice,  VkFormat  format,  VkImageType  type,  VkImageTiling  tiling,  VkImageUsageFlags  usage,  VkImageCreateFlags  flags,  VkExternalMemoryHandleTypeFlagsNV  externalHandleType,  VkExternalImageFormatPropertiesNV * pExternalImageFormatProperties)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkPhysicalDevice  , VkFormat  , VkImageType  , VkImageTiling  , VkImageUsageFlags  , VkImageCreateFlags  , VkExternalMemoryHandleTypeFlagsNV  , VkExternalImageFormatPropertiesNV * ))
        _android_vulkan_dlsym("vkGetPhysicalDeviceExternalImageFormatPropertiesNV"))
            (physicalDevice, format, type, tiling, usage, flags, externalHandleType, pExternalImageFormatProperties);
}

VKAPI_ATTR void VKAPI_CALL vkCmdBeginConditionalRenderingEXT( VkCommandBuffer  commandBuffer, const VkConditionalRenderingBeginInfoEXT * pConditionalRenderingBegin)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  ,const VkConditionalRenderingBeginInfoEXT * ))
        _android_vulkan_dlsym("vkCmdBeginConditionalRenderingEXT"))
            (commandBuffer, pConditionalRenderingBegin);
}

VKAPI_ATTR void VKAPI_CALL vkCmdEndConditionalRenderingEXT( VkCommandBuffer  commandBuffer)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  ))
        _android_vulkan_dlsym("vkCmdEndConditionalRenderingEXT"))
            (commandBuffer);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetViewportWScalingNV( VkCommandBuffer  commandBuffer,  uint32_t  firstViewport,  uint32_t  viewportCount, const VkViewportWScalingNV * pViewportWScalings)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , uint32_t  , uint32_t  ,const VkViewportWScalingNV * ))
        _android_vulkan_dlsym("vkCmdSetViewportWScalingNV"))
            (commandBuffer, firstViewport, viewportCount, pViewportWScalings);
}

VKAPI_ATTR VkResult VKAPI_CALL vkReleaseDisplayEXT( VkPhysicalDevice  physicalDevice,  VkDisplayKHR  display)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkPhysicalDevice  , VkDisplayKHR  ))
        _android_vulkan_dlsym("vkReleaseDisplayEXT"))
            (physicalDevice, display);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceCapabilities2EXT( VkPhysicalDevice  physicalDevice,  VkSurfaceKHR  surface,  VkSurfaceCapabilities2EXT * pSurfaceCapabilities)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkPhysicalDevice  , VkSurfaceKHR  , VkSurfaceCapabilities2EXT * ))
        _android_vulkan_dlsym("vkGetPhysicalDeviceSurfaceCapabilities2EXT"))
            (physicalDevice, surface, pSurfaceCapabilities);
}

VKAPI_ATTR VkResult VKAPI_CALL vkDisplayPowerControlEXT( VkDevice  device,  VkDisplayKHR  display, const VkDisplayPowerInfoEXT * pDisplayPowerInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , VkDisplayKHR  ,const VkDisplayPowerInfoEXT * ))
        _android_vulkan_dlsym("vkDisplayPowerControlEXT"))
            (device, display, pDisplayPowerInfo);
}

VKAPI_ATTR VkResult VKAPI_CALL vkRegisterDeviceEventEXT( VkDevice  device, const VkDeviceEventInfoEXT * pDeviceEventInfo, const VkAllocationCallbacks * pAllocator,  VkFence * pFence)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  ,const VkDeviceEventInfoEXT * ,const VkAllocationCallbacks * , VkFence * ))
        _android_vulkan_dlsym("vkRegisterDeviceEventEXT"))
            (device, pDeviceEventInfo, pAllocator, pFence);
}

VKAPI_ATTR VkResult VKAPI_CALL vkRegisterDisplayEventEXT( VkDevice  device,  VkDisplayKHR  display, const VkDisplayEventInfoEXT * pDisplayEventInfo, const VkAllocationCallbacks * pAllocator,  VkFence * pFence)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , VkDisplayKHR  ,const VkDisplayEventInfoEXT * ,const VkAllocationCallbacks * , VkFence * ))
        _android_vulkan_dlsym("vkRegisterDisplayEventEXT"))
            (device, display, pDisplayEventInfo, pAllocator, pFence);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetSwapchainCounterEXT( VkDevice  device,  VkSwapchainKHR  swapchain,  VkSurfaceCounterFlagBitsEXT  counter,  uint64_t * pCounterValue)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , VkSwapchainKHR  , VkSurfaceCounterFlagBitsEXT  , uint64_t * ))
        _android_vulkan_dlsym("vkGetSwapchainCounterEXT"))
            (device, swapchain, counter, pCounterValue);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetRefreshCycleDurationGOOGLE( VkDevice  device,  VkSwapchainKHR  swapchain,  VkRefreshCycleDurationGOOGLE * pDisplayTimingProperties)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , VkSwapchainKHR  , VkRefreshCycleDurationGOOGLE * ))
        _android_vulkan_dlsym("vkGetRefreshCycleDurationGOOGLE"))
            (device, swapchain, pDisplayTimingProperties);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPastPresentationTimingGOOGLE( VkDevice  device,  VkSwapchainKHR  swapchain,  uint32_t * pPresentationTimingCount,  VkPastPresentationTimingGOOGLE * pPresentationTimings)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , VkSwapchainKHR  , uint32_t * , VkPastPresentationTimingGOOGLE * ))
        _android_vulkan_dlsym("vkGetPastPresentationTimingGOOGLE"))
            (device, swapchain, pPresentationTimingCount, pPresentationTimings);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetDiscardRectangleEXT( VkCommandBuffer  commandBuffer,  uint32_t  firstDiscardRectangle,  uint32_t  discardRectangleCount, const VkRect2D * pDiscardRectangles)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , uint32_t  , uint32_t  ,const VkRect2D * ))
        _android_vulkan_dlsym("vkCmdSetDiscardRectangleEXT"))
            (commandBuffer, firstDiscardRectangle, discardRectangleCount, pDiscardRectangles);
}

#if VK_HEADER_VERSION >= 241

VKAPI_ATTR void VKAPI_CALL vkCmdSetDiscardRectangleEnableEXT( VkCommandBuffer  commandBuffer,  VkBool32  discardRectangleEnable)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkBool32  ))
        _android_vulkan_dlsym("vkCmdSetDiscardRectangleEnableEXT"))
            (commandBuffer, discardRectangleEnable);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetDiscardRectangleModeEXT( VkCommandBuffer  commandBuffer,  VkDiscardRectangleModeEXT  discardRectangleMode)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkDiscardRectangleModeEXT  ))
        _android_vulkan_dlsym("vkCmdSetDiscardRectangleModeEXT"))
            (commandBuffer, discardRectangleMode);
}

#endif

VKAPI_ATTR void VKAPI_CALL vkSetHdrMetadataEXT( VkDevice  device,  uint32_t  swapchainCount, const VkSwapchainKHR * pSwapchains, const VkHdrMetadataEXT * pMetadata)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  , uint32_t  ,const VkSwapchainKHR * ,const VkHdrMetadataEXT * ))
        _android_vulkan_dlsym("vkSetHdrMetadataEXT"))
            (device, swapchainCount, pSwapchains, pMetadata);
}

VKAPI_ATTR VkResult VKAPI_CALL vkSetDebugUtilsObjectNameEXT( VkDevice  device, const VkDebugUtilsObjectNameInfoEXT * pNameInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  ,const VkDebugUtilsObjectNameInfoEXT * ))
        _android_vulkan_dlsym("vkSetDebugUtilsObjectNameEXT"))
            (device, pNameInfo);
}

VKAPI_ATTR VkResult VKAPI_CALL vkSetDebugUtilsObjectTagEXT( VkDevice  device, const VkDebugUtilsObjectTagInfoEXT * pTagInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  ,const VkDebugUtilsObjectTagInfoEXT * ))
        _android_vulkan_dlsym("vkSetDebugUtilsObjectTagEXT"))
            (device, pTagInfo);
}

VKAPI_ATTR void VKAPI_CALL vkQueueBeginDebugUtilsLabelEXT( VkQueue  queue, const VkDebugUtilsLabelEXT * pLabelInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkQueue  ,const VkDebugUtilsLabelEXT * ))
        _android_vulkan_dlsym("vkQueueBeginDebugUtilsLabelEXT"))
            (queue, pLabelInfo);
}

VKAPI_ATTR void VKAPI_CALL vkQueueEndDebugUtilsLabelEXT( VkQueue  queue)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkQueue  ))
        _android_vulkan_dlsym("vkQueueEndDebugUtilsLabelEXT"))
            (queue);
}

VKAPI_ATTR void VKAPI_CALL vkQueueInsertDebugUtilsLabelEXT( VkQueue  queue, const VkDebugUtilsLabelEXT * pLabelInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkQueue  ,const VkDebugUtilsLabelEXT * ))
        _android_vulkan_dlsym("vkQueueInsertDebugUtilsLabelEXT"))
            (queue, pLabelInfo);
}

VKAPI_ATTR void VKAPI_CALL vkCmdBeginDebugUtilsLabelEXT( VkCommandBuffer  commandBuffer, const VkDebugUtilsLabelEXT * pLabelInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  ,const VkDebugUtilsLabelEXT * ))
        _android_vulkan_dlsym("vkCmdBeginDebugUtilsLabelEXT"))
            (commandBuffer, pLabelInfo);
}

VKAPI_ATTR void VKAPI_CALL vkCmdEndDebugUtilsLabelEXT( VkCommandBuffer  commandBuffer)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  ))
        _android_vulkan_dlsym("vkCmdEndDebugUtilsLabelEXT"))
            (commandBuffer);
}

VKAPI_ATTR void VKAPI_CALL vkCmdInsertDebugUtilsLabelEXT( VkCommandBuffer  commandBuffer, const VkDebugUtilsLabelEXT * pLabelInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  ,const VkDebugUtilsLabelEXT * ))
        _android_vulkan_dlsym("vkCmdInsertDebugUtilsLabelEXT"))
            (commandBuffer, pLabelInfo);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateDebugUtilsMessengerEXT( VkInstance  instance, const VkDebugUtilsMessengerCreateInfoEXT * pCreateInfo, const VkAllocationCallbacks * pAllocator,  VkDebugUtilsMessengerEXT * pMessenger)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkInstance  ,const VkDebugUtilsMessengerCreateInfoEXT * ,const VkAllocationCallbacks * , VkDebugUtilsMessengerEXT * ))
        _android_vulkan_dlsym("vkCreateDebugUtilsMessengerEXT"))
            (instance, pCreateInfo, pAllocator, pMessenger);
}

VKAPI_ATTR void VKAPI_CALL vkDestroyDebugUtilsMessengerEXT( VkInstance  instance,  VkDebugUtilsMessengerEXT  messenger, const VkAllocationCallbacks * pAllocator)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkInstance  , VkDebugUtilsMessengerEXT  ,const VkAllocationCallbacks * ))
        _android_vulkan_dlsym("vkDestroyDebugUtilsMessengerEXT"))
            (instance, messenger, pAllocator);
}

VKAPI_ATTR void VKAPI_CALL vkSubmitDebugUtilsMessageEXT( VkInstance  instance,  VkDebugUtilsMessageSeverityFlagBitsEXT  messageSeverity,  VkDebugUtilsMessageTypeFlagsEXT  messageTypes, const VkDebugUtilsMessengerCallbackDataEXT * pCallbackData)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkInstance  , VkDebugUtilsMessageSeverityFlagBitsEXT  , VkDebugUtilsMessageTypeFlagsEXT  ,const VkDebugUtilsMessengerCallbackDataEXT * ))
        _android_vulkan_dlsym("vkSubmitDebugUtilsMessageEXT"))
            (instance, messageSeverity, messageTypes, pCallbackData);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetSampleLocationsEXT( VkCommandBuffer  commandBuffer, const VkSampleLocationsInfoEXT * pSampleLocationsInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  ,const VkSampleLocationsInfoEXT * ))
        _android_vulkan_dlsym("vkCmdSetSampleLocationsEXT"))
            (commandBuffer, pSampleLocationsInfo);
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMultisamplePropertiesEXT( VkPhysicalDevice  physicalDevice,  VkSampleCountFlagBits  samples,  VkMultisamplePropertiesEXT * pMultisampleProperties)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkPhysicalDevice  , VkSampleCountFlagBits  , VkMultisamplePropertiesEXT * ))
        _android_vulkan_dlsym("vkGetPhysicalDeviceMultisamplePropertiesEXT"))
            (physicalDevice, samples, pMultisampleProperties);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetImageDrmFormatModifierPropertiesEXT( VkDevice  device,  VkImage  image,  VkImageDrmFormatModifierPropertiesEXT * pProperties)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , VkImage  , VkImageDrmFormatModifierPropertiesEXT * ))
        _android_vulkan_dlsym("vkGetImageDrmFormatModifierPropertiesEXT"))
            (device, image, pProperties);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateValidationCacheEXT( VkDevice  device, const VkValidationCacheCreateInfoEXT * pCreateInfo, const VkAllocationCallbacks * pAllocator,  VkValidationCacheEXT * pValidationCache)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  ,const VkValidationCacheCreateInfoEXT * ,const VkAllocationCallbacks * , VkValidationCacheEXT * ))
        _android_vulkan_dlsym("vkCreateValidationCacheEXT"))
            (device, pCreateInfo, pAllocator, pValidationCache);
}

VKAPI_ATTR void VKAPI_CALL vkDestroyValidationCacheEXT( VkDevice  device,  VkValidationCacheEXT  validationCache, const VkAllocationCallbacks * pAllocator)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  , VkValidationCacheEXT  ,const VkAllocationCallbacks * ))
        _android_vulkan_dlsym("vkDestroyValidationCacheEXT"))
            (device, validationCache, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL vkMergeValidationCachesEXT( VkDevice  device,  VkValidationCacheEXT  dstCache,  uint32_t  srcCacheCount, const VkValidationCacheEXT * pSrcCaches)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , VkValidationCacheEXT  , uint32_t  ,const VkValidationCacheEXT * ))
        _android_vulkan_dlsym("vkMergeValidationCachesEXT"))
            (device, dstCache, srcCacheCount, pSrcCaches);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetValidationCacheDataEXT( VkDevice  device,  VkValidationCacheEXT  validationCache,  size_t * pDataSize,  void * pData)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , VkValidationCacheEXT  , size_t * , void * ))
        _android_vulkan_dlsym("vkGetValidationCacheDataEXT"))
            (device, validationCache, pDataSize, pData);
}

VKAPI_ATTR void VKAPI_CALL vkCmdBindShadingRateImageNV( VkCommandBuffer  commandBuffer,  VkImageView  imageView,  VkImageLayout  imageLayout)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkImageView  , VkImageLayout  ))
        _android_vulkan_dlsym("vkCmdBindShadingRateImageNV"))
            (commandBuffer, imageView, imageLayout);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetViewportShadingRatePaletteNV( VkCommandBuffer  commandBuffer,  uint32_t  firstViewport,  uint32_t  viewportCount, const VkShadingRatePaletteNV * pShadingRatePalettes)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , uint32_t  , uint32_t  ,const VkShadingRatePaletteNV * ))
        _android_vulkan_dlsym("vkCmdSetViewportShadingRatePaletteNV"))
            (commandBuffer, firstViewport, viewportCount, pShadingRatePalettes);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetCoarseSampleOrderNV( VkCommandBuffer  commandBuffer,  VkCoarseSampleOrderTypeNV  sampleOrderType,  uint32_t  customSampleOrderCount, const VkCoarseSampleOrderCustomNV * pCustomSampleOrders)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkCoarseSampleOrderTypeNV  , uint32_t  ,const VkCoarseSampleOrderCustomNV * ))
        _android_vulkan_dlsym("vkCmdSetCoarseSampleOrderNV"))
            (commandBuffer, sampleOrderType, customSampleOrderCount, pCustomSampleOrders);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateAccelerationStructureNV( VkDevice  device, const VkAccelerationStructureCreateInfoNV * pCreateInfo, const VkAllocationCallbacks * pAllocator,  VkAccelerationStructureNV * pAccelerationStructure)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  ,const VkAccelerationStructureCreateInfoNV * ,const VkAllocationCallbacks * , VkAccelerationStructureNV * ))
        _android_vulkan_dlsym("vkCreateAccelerationStructureNV"))
            (device, pCreateInfo, pAllocator, pAccelerationStructure);
}

VKAPI_ATTR void VKAPI_CALL vkDestroyAccelerationStructureNV( VkDevice  device,  VkAccelerationStructureNV  accelerationStructure, const VkAllocationCallbacks * pAllocator)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  , VkAccelerationStructureNV  ,const VkAllocationCallbacks * ))
        _android_vulkan_dlsym("vkDestroyAccelerationStructureNV"))
            (device, accelerationStructure, pAllocator);
}

VKAPI_ATTR void VKAPI_CALL vkGetAccelerationStructureMemoryRequirementsNV( VkDevice  device, const VkAccelerationStructureMemoryRequirementsInfoNV * pInfo,  VkMemoryRequirements2KHR * pMemoryRequirements)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  ,const VkAccelerationStructureMemoryRequirementsInfoNV * , VkMemoryRequirements2KHR * ))
        _android_vulkan_dlsym("vkGetAccelerationStructureMemoryRequirementsNV"))
            (device, pInfo, pMemoryRequirements);
}

VKAPI_ATTR VkResult VKAPI_CALL vkBindAccelerationStructureMemoryNV( VkDevice  device,  uint32_t  bindInfoCount, const VkBindAccelerationStructureMemoryInfoNV * pBindInfos)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , uint32_t  ,const VkBindAccelerationStructureMemoryInfoNV * ))
        _android_vulkan_dlsym("vkBindAccelerationStructureMemoryNV"))
            (device, bindInfoCount, pBindInfos);
}

VKAPI_ATTR void VKAPI_CALL vkCmdBuildAccelerationStructureNV( VkCommandBuffer  commandBuffer, const VkAccelerationStructureInfoNV * pInfo,  VkBuffer  instanceData,  VkDeviceSize  instanceOffset,  VkBool32  update,  VkAccelerationStructureNV  dst,  VkAccelerationStructureNV  src,  VkBuffer  scratch,  VkDeviceSize  scratchOffset)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  ,const VkAccelerationStructureInfoNV * , VkBuffer  , VkDeviceSize  , VkBool32  , VkAccelerationStructureNV  , VkAccelerationStructureNV  , VkBuffer  , VkDeviceSize  ))
        _android_vulkan_dlsym("vkCmdBuildAccelerationStructureNV"))
            (commandBuffer, pInfo, instanceData, instanceOffset, update, dst, src, scratch, scratchOffset);
}

VKAPI_ATTR void VKAPI_CALL vkCmdCopyAccelerationStructureNV( VkCommandBuffer  commandBuffer,  VkAccelerationStructureNV  dst,  VkAccelerationStructureNV  src,  VkCopyAccelerationStructureModeKHR  mode)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkAccelerationStructureNV  , VkAccelerationStructureNV  , VkCopyAccelerationStructureModeKHR  ))
        _android_vulkan_dlsym("vkCmdCopyAccelerationStructureNV"))
            (commandBuffer, dst, src, mode);
}

VKAPI_ATTR void VKAPI_CALL vkCmdTraceRaysNV( VkCommandBuffer  commandBuffer,  VkBuffer  raygenShaderBindingTableBuffer,  VkDeviceSize  raygenShaderBindingOffset,  VkBuffer  missShaderBindingTableBuffer,  VkDeviceSize  missShaderBindingOffset,  VkDeviceSize  missShaderBindingStride,  VkBuffer  hitShaderBindingTableBuffer,  VkDeviceSize  hitShaderBindingOffset,  VkDeviceSize  hitShaderBindingStride,  VkBuffer  callableShaderBindingTableBuffer,  VkDeviceSize  callableShaderBindingOffset,  VkDeviceSize  callableShaderBindingStride,  uint32_t  width,  uint32_t  height,  uint32_t  depth)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkBuffer  , VkDeviceSize  , VkBuffer  , VkDeviceSize  , VkDeviceSize  , VkBuffer  , VkDeviceSize  , VkDeviceSize  , VkBuffer  , VkDeviceSize  , VkDeviceSize  , uint32_t  , uint32_t  , uint32_t  ))
        _android_vulkan_dlsym("vkCmdTraceRaysNV"))
            (commandBuffer, raygenShaderBindingTableBuffer, raygenShaderBindingOffset, missShaderBindingTableBuffer, missShaderBindingOffset, missShaderBindingStride, hitShaderBindingTableBuffer, hitShaderBindingOffset, hitShaderBindingStride, callableShaderBindingTableBuffer, callableShaderBindingOffset, callableShaderBindingStride, width, height, depth);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateRayTracingPipelinesNV( VkDevice  device,  VkPipelineCache  pipelineCache,  uint32_t  createInfoCount, const VkRayTracingPipelineCreateInfoNV * pCreateInfos, const VkAllocationCallbacks * pAllocator,  VkPipeline * pPipelines)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , VkPipelineCache  , uint32_t  ,const VkRayTracingPipelineCreateInfoNV * ,const VkAllocationCallbacks * , VkPipeline * ))
        _android_vulkan_dlsym("vkCreateRayTracingPipelinesNV"))
            (device, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetRayTracingShaderGroupHandlesKHR( VkDevice  device,  VkPipeline  pipeline,  uint32_t  firstGroup,  uint32_t  groupCount,  size_t  dataSize,  void * pData)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , VkPipeline  , uint32_t  , uint32_t  , size_t  , void * ))
        _android_vulkan_dlsym("vkGetRayTracingShaderGroupHandlesKHR"))
            (device, pipeline, firstGroup, groupCount, dataSize, pData);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetRayTracingShaderGroupHandlesNV( VkDevice  device,  VkPipeline  pipeline,  uint32_t  firstGroup,  uint32_t  groupCount,  size_t  dataSize,  void * pData)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , VkPipeline  , uint32_t  , uint32_t  , size_t  , void * ))
        _android_vulkan_dlsym("vkGetRayTracingShaderGroupHandlesNV"))
            (device, pipeline, firstGroup, groupCount, dataSize, pData);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetAccelerationStructureHandleNV( VkDevice  device,  VkAccelerationStructureNV  accelerationStructure,  size_t  dataSize,  void * pData)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , VkAccelerationStructureNV  , size_t  , void * ))
        _android_vulkan_dlsym("vkGetAccelerationStructureHandleNV"))
            (device, accelerationStructure, dataSize, pData);
}

VKAPI_ATTR void VKAPI_CALL vkCmdWriteAccelerationStructuresPropertiesNV( VkCommandBuffer  commandBuffer,  uint32_t  accelerationStructureCount, const VkAccelerationStructureNV * pAccelerationStructures,  VkQueryType  queryType,  VkQueryPool  queryPool,  uint32_t  firstQuery)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , uint32_t  ,const VkAccelerationStructureNV * , VkQueryType  , VkQueryPool  , uint32_t  ))
        _android_vulkan_dlsym("vkCmdWriteAccelerationStructuresPropertiesNV"))
            (commandBuffer, accelerationStructureCount, pAccelerationStructures, queryType, queryPool, firstQuery);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCompileDeferredNV( VkDevice  device,  VkPipeline  pipeline,  uint32_t  shader)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , VkPipeline  , uint32_t  ))
        _android_vulkan_dlsym("vkCompileDeferredNV"))
            (device, pipeline, shader);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetMemoryHostPointerPropertiesEXT( VkDevice  device,  VkExternalMemoryHandleTypeFlagBits  handleType, const void * pHostPointer,  VkMemoryHostPointerPropertiesEXT * pMemoryHostPointerProperties)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , VkExternalMemoryHandleTypeFlagBits  ,const void * , VkMemoryHostPointerPropertiesEXT * ))
        _android_vulkan_dlsym("vkGetMemoryHostPointerPropertiesEXT"))
            (device, handleType, pHostPointer, pMemoryHostPointerProperties);
}

VKAPI_ATTR void VKAPI_CALL vkCmdWriteBufferMarkerAMD( VkCommandBuffer  commandBuffer,  VkPipelineStageFlagBits  pipelineStage,  VkBuffer  dstBuffer,  VkDeviceSize  dstOffset,  uint32_t  marker)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkPipelineStageFlagBits  , VkBuffer  , VkDeviceSize  , uint32_t  ))
        _android_vulkan_dlsym("vkCmdWriteBufferMarkerAMD"))
            (commandBuffer, pipelineStage, dstBuffer, dstOffset, marker);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceCalibrateableTimeDomainsEXT( VkPhysicalDevice  physicalDevice,  uint32_t * pTimeDomainCount,  VkTimeDomainKHR * pTimeDomains)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkPhysicalDevice  , uint32_t * , VkTimeDomainKHR * ))
        _android_vulkan_dlsym("vkGetPhysicalDeviceCalibrateableTimeDomainsEXT"))
            (physicalDevice, pTimeDomainCount, pTimeDomains);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetCalibratedTimestampsEXT( VkDevice  device,  uint32_t  timestampCount, const VkCalibratedTimestampInfoKHR * pTimestampInfos,  uint64_t * pTimestamps,  uint64_t * pMaxDeviation)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , uint32_t  ,const VkCalibratedTimestampInfoKHR * , uint64_t * , uint64_t * ))
        _android_vulkan_dlsym("vkGetCalibratedTimestampsEXT"))
            (device, timestampCount, pTimestampInfos, pTimestamps, pMaxDeviation);
}

VKAPI_ATTR void VKAPI_CALL vkCmdDrawMeshTasksNV( VkCommandBuffer  commandBuffer,  uint32_t  taskCount,  uint32_t  firstTask)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , uint32_t  , uint32_t  ))
        _android_vulkan_dlsym("vkCmdDrawMeshTasksNV"))
            (commandBuffer, taskCount, firstTask);
}

VKAPI_ATTR void VKAPI_CALL vkCmdDrawMeshTasksIndirectNV( VkCommandBuffer  commandBuffer,  VkBuffer  buffer,  VkDeviceSize  offset,  uint32_t  drawCount,  uint32_t  stride)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkBuffer  , VkDeviceSize  , uint32_t  , uint32_t  ))
        _android_vulkan_dlsym("vkCmdDrawMeshTasksIndirectNV"))
            (commandBuffer, buffer, offset, drawCount, stride);
}

VKAPI_ATTR void VKAPI_CALL vkCmdDrawMeshTasksIndirectCountNV( VkCommandBuffer  commandBuffer,  VkBuffer  buffer,  VkDeviceSize  offset,  VkBuffer  countBuffer,  VkDeviceSize  countBufferOffset,  uint32_t  maxDrawCount,  uint32_t  stride)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkBuffer  , VkDeviceSize  , VkBuffer  , VkDeviceSize  , uint32_t  , uint32_t  ))
        _android_vulkan_dlsym("vkCmdDrawMeshTasksIndirectCountNV"))
            (commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);
}

#if VK_HEADER_VERSION >= 241

VKAPI_ATTR void VKAPI_CALL vkCmdSetExclusiveScissorEnableNV( VkCommandBuffer  commandBuffer,  uint32_t  firstExclusiveScissor,  uint32_t  exclusiveScissorCount, const VkBool32 * pExclusiveScissorEnables)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , uint32_t  , uint32_t  ,const VkBool32 * ))
        _android_vulkan_dlsym("vkCmdSetExclusiveScissorEnableNV"))
            (commandBuffer, firstExclusiveScissor, exclusiveScissorCount, pExclusiveScissorEnables);
}

#endif

VKAPI_ATTR void VKAPI_CALL vkCmdSetExclusiveScissorNV( VkCommandBuffer  commandBuffer,  uint32_t  firstExclusiveScissor,  uint32_t  exclusiveScissorCount, const VkRect2D * pExclusiveScissors)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , uint32_t  , uint32_t  ,const VkRect2D * ))
        _android_vulkan_dlsym("vkCmdSetExclusiveScissorNV"))
            (commandBuffer, firstExclusiveScissor, exclusiveScissorCount, pExclusiveScissors);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetCheckpointNV( VkCommandBuffer  commandBuffer, const void * pCheckpointMarker)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  ,const void * ))
        _android_vulkan_dlsym("vkCmdSetCheckpointNV"))
            (commandBuffer, pCheckpointMarker);
}

VKAPI_ATTR void VKAPI_CALL vkGetQueueCheckpointDataNV( VkQueue  queue,  uint32_t * pCheckpointDataCount,  VkCheckpointDataNV * pCheckpointData)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkQueue  , uint32_t * , VkCheckpointDataNV * ))
        _android_vulkan_dlsym("vkGetQueueCheckpointDataNV"))
            (queue, pCheckpointDataCount, pCheckpointData);
}

VKAPI_ATTR VkResult VKAPI_CALL vkInitializePerformanceApiINTEL( VkDevice  device, const VkInitializePerformanceApiInfoINTEL * pInitializeInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  ,const VkInitializePerformanceApiInfoINTEL * ))
        _android_vulkan_dlsym("vkInitializePerformanceApiINTEL"))
            (device, pInitializeInfo);
}

VKAPI_ATTR void VKAPI_CALL vkUninitializePerformanceApiINTEL( VkDevice  device)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  ))
        _android_vulkan_dlsym("vkUninitializePerformanceApiINTEL"))
            (device);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCmdSetPerformanceMarkerINTEL( VkCommandBuffer  commandBuffer, const VkPerformanceMarkerInfoINTEL * pMarkerInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkCommandBuffer  ,const VkPerformanceMarkerInfoINTEL * ))
        _android_vulkan_dlsym("vkCmdSetPerformanceMarkerINTEL"))
            (commandBuffer, pMarkerInfo);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCmdSetPerformanceStreamMarkerINTEL( VkCommandBuffer  commandBuffer, const VkPerformanceStreamMarkerInfoINTEL * pMarkerInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkCommandBuffer  ,const VkPerformanceStreamMarkerInfoINTEL * ))
        _android_vulkan_dlsym("vkCmdSetPerformanceStreamMarkerINTEL"))
            (commandBuffer, pMarkerInfo);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCmdSetPerformanceOverrideINTEL( VkCommandBuffer  commandBuffer, const VkPerformanceOverrideInfoINTEL * pOverrideInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkCommandBuffer  ,const VkPerformanceOverrideInfoINTEL * ))
        _android_vulkan_dlsym("vkCmdSetPerformanceOverrideINTEL"))
            (commandBuffer, pOverrideInfo);
}

VKAPI_ATTR VkResult VKAPI_CALL vkAcquirePerformanceConfigurationINTEL( VkDevice  device, const VkPerformanceConfigurationAcquireInfoINTEL * pAcquireInfo,  VkPerformanceConfigurationINTEL * pConfiguration)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  ,const VkPerformanceConfigurationAcquireInfoINTEL * , VkPerformanceConfigurationINTEL * ))
        _android_vulkan_dlsym("vkAcquirePerformanceConfigurationINTEL"))
            (device, pAcquireInfo, pConfiguration);
}

VKAPI_ATTR VkResult VKAPI_CALL vkReleasePerformanceConfigurationINTEL( VkDevice  device,  VkPerformanceConfigurationINTEL  configuration)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , VkPerformanceConfigurationINTEL  ))
        _android_vulkan_dlsym("vkReleasePerformanceConfigurationINTEL"))
            (device, configuration);
}

VKAPI_ATTR VkResult VKAPI_CALL vkQueueSetPerformanceConfigurationINTEL( VkQueue  queue,  VkPerformanceConfigurationINTEL  configuration)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkQueue  , VkPerformanceConfigurationINTEL  ))
        _android_vulkan_dlsym("vkQueueSetPerformanceConfigurationINTEL"))
            (queue, configuration);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPerformanceParameterINTEL( VkDevice  device,  VkPerformanceParameterTypeINTEL  parameter,  VkPerformanceValueINTEL * pValue)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , VkPerformanceParameterTypeINTEL  , VkPerformanceValueINTEL * ))
        _android_vulkan_dlsym("vkGetPerformanceParameterINTEL"))
            (device, parameter, pValue);
}

VKAPI_ATTR void VKAPI_CALL vkSetLocalDimmingAMD( VkDevice  device,  VkSwapchainKHR  swapChain,  VkBool32  localDimmingEnable)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  , VkSwapchainKHR  , VkBool32  ))
        _android_vulkan_dlsym("vkSetLocalDimmingAMD"))
            (device, swapChain, localDimmingEnable);
}

VKAPI_ATTR VkDeviceAddress VKAPI_CALL vkGetBufferDeviceAddressEXT( VkDevice  device, const VkBufferDeviceAddressInfo * pInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkDeviceAddress (*)( VkDevice  ,const VkBufferDeviceAddressInfo * ))
        _android_vulkan_dlsym("vkGetBufferDeviceAddressEXT"))
            (device, pInfo);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceToolPropertiesEXT( VkPhysicalDevice  physicalDevice,  uint32_t * pToolCount,  VkPhysicalDeviceToolProperties * pToolProperties)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkPhysicalDevice  , uint32_t * , VkPhysicalDeviceToolProperties * ))
        _android_vulkan_dlsym("vkGetPhysicalDeviceToolPropertiesEXT"))
            (physicalDevice, pToolCount, pToolProperties);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceCooperativeMatrixPropertiesNV( VkPhysicalDevice  physicalDevice,  uint32_t * pPropertyCount,  VkCooperativeMatrixPropertiesNV * pProperties)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkPhysicalDevice  , uint32_t * , VkCooperativeMatrixPropertiesNV * ))
        _android_vulkan_dlsym("vkGetPhysicalDeviceCooperativeMatrixPropertiesNV"))
            (physicalDevice, pPropertyCount, pProperties);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSupportedFramebufferMixedSamplesCombinationsNV( VkPhysicalDevice  physicalDevice,  uint32_t * pCombinationCount,  VkFramebufferMixedSamplesCombinationNV * pCombinations)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkPhysicalDevice  , uint32_t * , VkFramebufferMixedSamplesCombinationNV * ))
        _android_vulkan_dlsym("vkGetPhysicalDeviceSupportedFramebufferMixedSamplesCombinationsNV"))
            (physicalDevice, pCombinationCount, pCombinations);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateHeadlessSurfaceEXT( VkInstance  instance, const VkHeadlessSurfaceCreateInfoEXT * pCreateInfo, const VkAllocationCallbacks * pAllocator,  VkSurfaceKHR * pSurface)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkInstance  ,const VkHeadlessSurfaceCreateInfoEXT * ,const VkAllocationCallbacks * , VkSurfaceKHR * ))
        _android_vulkan_dlsym("vkCreateHeadlessSurfaceEXT"))
            (instance, pCreateInfo, pAllocator, pSurface);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetLineStippleEXT( VkCommandBuffer  commandBuffer,  uint32_t  lineStippleFactor,  uint16_t  lineStipplePattern)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , uint32_t  , uint16_t  ))
        _android_vulkan_dlsym("vkCmdSetLineStippleEXT"))
            (commandBuffer, lineStippleFactor, lineStipplePattern);
}

VKAPI_ATTR void VKAPI_CALL vkResetQueryPoolEXT( VkDevice  device,  VkQueryPool  queryPool,  uint32_t  firstQuery,  uint32_t  queryCount)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  , VkQueryPool  , uint32_t  , uint32_t  ))
        _android_vulkan_dlsym("vkResetQueryPoolEXT"))
            (device, queryPool, firstQuery, queryCount);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetCullModeEXT( VkCommandBuffer  commandBuffer,  VkCullModeFlags  cullMode)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkCullModeFlags  ))
        _android_vulkan_dlsym("vkCmdSetCullModeEXT"))
            (commandBuffer, cullMode);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetFrontFaceEXT( VkCommandBuffer  commandBuffer,  VkFrontFace  frontFace)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkFrontFace  ))
        _android_vulkan_dlsym("vkCmdSetFrontFaceEXT"))
            (commandBuffer, frontFace);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetPrimitiveTopologyEXT( VkCommandBuffer  commandBuffer,  VkPrimitiveTopology  primitiveTopology)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkPrimitiveTopology  ))
        _android_vulkan_dlsym("vkCmdSetPrimitiveTopologyEXT"))
            (commandBuffer, primitiveTopology);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetViewportWithCountEXT( VkCommandBuffer  commandBuffer,  uint32_t  viewportCount, const VkViewport * pViewports)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , uint32_t  ,const VkViewport * ))
        _android_vulkan_dlsym("vkCmdSetViewportWithCountEXT"))
            (commandBuffer, viewportCount, pViewports);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetScissorWithCountEXT( VkCommandBuffer  commandBuffer,  uint32_t  scissorCount, const VkRect2D * pScissors)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , uint32_t  ,const VkRect2D * ))
        _android_vulkan_dlsym("vkCmdSetScissorWithCountEXT"))
            (commandBuffer, scissorCount, pScissors);
}

VKAPI_ATTR void VKAPI_CALL vkCmdBindVertexBuffers2EXT( VkCommandBuffer  commandBuffer,  uint32_t  firstBinding,  uint32_t  bindingCount, const VkBuffer * pBuffers, const VkDeviceSize * pOffsets, const VkDeviceSize * pSizes, const VkDeviceSize * pStrides)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , uint32_t  , uint32_t  ,const VkBuffer * ,const VkDeviceSize * ,const VkDeviceSize * ,const VkDeviceSize * ))
        _android_vulkan_dlsym("vkCmdBindVertexBuffers2EXT"))
            (commandBuffer, firstBinding, bindingCount, pBuffers, pOffsets, pSizes, pStrides);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthTestEnableEXT( VkCommandBuffer  commandBuffer,  VkBool32  depthTestEnable)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkBool32  ))
        _android_vulkan_dlsym("vkCmdSetDepthTestEnableEXT"))
            (commandBuffer, depthTestEnable);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthWriteEnableEXT( VkCommandBuffer  commandBuffer,  VkBool32  depthWriteEnable)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkBool32  ))
        _android_vulkan_dlsym("vkCmdSetDepthWriteEnableEXT"))
            (commandBuffer, depthWriteEnable);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthCompareOpEXT( VkCommandBuffer  commandBuffer,  VkCompareOp  depthCompareOp)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkCompareOp  ))
        _android_vulkan_dlsym("vkCmdSetDepthCompareOpEXT"))
            (commandBuffer, depthCompareOp);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthBoundsTestEnableEXT( VkCommandBuffer  commandBuffer,  VkBool32  depthBoundsTestEnable)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkBool32  ))
        _android_vulkan_dlsym("vkCmdSetDepthBoundsTestEnableEXT"))
            (commandBuffer, depthBoundsTestEnable);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetStencilTestEnableEXT( VkCommandBuffer  commandBuffer,  VkBool32  stencilTestEnable)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkBool32  ))
        _android_vulkan_dlsym("vkCmdSetStencilTestEnableEXT"))
            (commandBuffer, stencilTestEnable);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetStencilOpEXT( VkCommandBuffer  commandBuffer,  VkStencilFaceFlags  faceMask,  VkStencilOp  failOp,  VkStencilOp  passOp,  VkStencilOp  depthFailOp,  VkCompareOp  compareOp)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkStencilFaceFlags  , VkStencilOp  , VkStencilOp  , VkStencilOp  , VkCompareOp  ))
        _android_vulkan_dlsym("vkCmdSetStencilOpEXT"))
            (commandBuffer, faceMask, failOp, passOp, depthFailOp, compareOp);
}

#if VK_HEADER_VERSION >= 258

VKAPI_ATTR VkResult VKAPI_CALL vkCopyMemoryToImageEXT( VkDevice  device, const VkCopyMemoryToImageInfoEXT * pCopyMemoryToImageInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  ,const VkCopyMemoryToImageInfoEXT * ))
        _android_vulkan_dlsym("vkCopyMemoryToImageEXT"))
            (device, pCopyMemoryToImageInfo);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCopyImageToMemoryEXT( VkDevice  device, const VkCopyImageToMemoryInfoEXT * pCopyImageToMemoryInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  ,const VkCopyImageToMemoryInfoEXT * ))
        _android_vulkan_dlsym("vkCopyImageToMemoryEXT"))
            (device, pCopyImageToMemoryInfo);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCopyImageToImageEXT( VkDevice  device, const VkCopyImageToImageInfoEXT * pCopyImageToImageInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  ,const VkCopyImageToImageInfoEXT * ))
        _android_vulkan_dlsym("vkCopyImageToImageEXT"))
            (device, pCopyImageToImageInfo);
}

VKAPI_ATTR VkResult VKAPI_CALL vkTransitionImageLayoutEXT( VkDevice  device,  uint32_t  transitionCount, const VkHostImageLayoutTransitionInfoEXT * pTransitions)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , uint32_t  ,const VkHostImageLayoutTransitionInfoEXT * ))
        _android_vulkan_dlsym("vkTransitionImageLayoutEXT"))
            (device, transitionCount, pTransitions);
}

#endif

#if VK_HEADER_VERSION >= 213

VKAPI_ATTR void VKAPI_CALL vkGetImageSubresourceLayout2EXT( VkDevice  device,  VkImage  image, const VkImageSubresource2KHR * pSubresource,  VkSubresourceLayout2KHR * pLayout)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  , VkImage  ,const VkImageSubresource2KHR * , VkSubresourceLayout2KHR * ))
        _android_vulkan_dlsym("vkGetImageSubresourceLayout2EXT"))
            (device, image, pSubresource, pLayout);
}

#endif

#if VK_HEADER_VERSION >= 237

VKAPI_ATTR VkResult VKAPI_CALL vkReleaseSwapchainImagesEXT( VkDevice  device, const VkReleaseSwapchainImagesInfoEXT * pReleaseInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  ,const VkReleaseSwapchainImagesInfoEXT * ))
        _android_vulkan_dlsym("vkReleaseSwapchainImagesEXT"))
            (device, pReleaseInfo);
}

#endif

VKAPI_ATTR void VKAPI_CALL vkGetGeneratedCommandsMemoryRequirementsNV( VkDevice  device, const VkGeneratedCommandsMemoryRequirementsInfoNV * pInfo,  VkMemoryRequirements2 * pMemoryRequirements)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  ,const VkGeneratedCommandsMemoryRequirementsInfoNV * , VkMemoryRequirements2 * ))
        _android_vulkan_dlsym("vkGetGeneratedCommandsMemoryRequirementsNV"))
            (device, pInfo, pMemoryRequirements);
}

VKAPI_ATTR void VKAPI_CALL vkCmdPreprocessGeneratedCommandsNV( VkCommandBuffer  commandBuffer, const VkGeneratedCommandsInfoNV * pGeneratedCommandsInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  ,const VkGeneratedCommandsInfoNV * ))
        _android_vulkan_dlsym("vkCmdPreprocessGeneratedCommandsNV"))
            (commandBuffer, pGeneratedCommandsInfo);
}

VKAPI_ATTR void VKAPI_CALL vkCmdExecuteGeneratedCommandsNV( VkCommandBuffer  commandBuffer,  VkBool32  isPreprocessed, const VkGeneratedCommandsInfoNV * pGeneratedCommandsInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkBool32  ,const VkGeneratedCommandsInfoNV * ))
        _android_vulkan_dlsym("vkCmdExecuteGeneratedCommandsNV"))
            (commandBuffer, isPreprocessed, pGeneratedCommandsInfo);
}

VKAPI_ATTR void VKAPI_CALL vkCmdBindPipelineShaderGroupNV( VkCommandBuffer  commandBuffer,  VkPipelineBindPoint  pipelineBindPoint,  VkPipeline  pipeline,  uint32_t  groupIndex)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkPipelineBindPoint  , VkPipeline  , uint32_t  ))
        _android_vulkan_dlsym("vkCmdBindPipelineShaderGroupNV"))
            (commandBuffer, pipelineBindPoint, pipeline, groupIndex);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateIndirectCommandsLayoutNV( VkDevice  device, const VkIndirectCommandsLayoutCreateInfoNV * pCreateInfo, const VkAllocationCallbacks * pAllocator,  VkIndirectCommandsLayoutNV * pIndirectCommandsLayout)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  ,const VkIndirectCommandsLayoutCreateInfoNV * ,const VkAllocationCallbacks * , VkIndirectCommandsLayoutNV * ))
        _android_vulkan_dlsym("vkCreateIndirectCommandsLayoutNV"))
            (device, pCreateInfo, pAllocator, pIndirectCommandsLayout);
}

VKAPI_ATTR void VKAPI_CALL vkDestroyIndirectCommandsLayoutNV( VkDevice  device,  VkIndirectCommandsLayoutNV  indirectCommandsLayout, const VkAllocationCallbacks * pAllocator)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  , VkIndirectCommandsLayoutNV  ,const VkAllocationCallbacks * ))
        _android_vulkan_dlsym("vkDestroyIndirectCommandsLayoutNV"))
            (device, indirectCommandsLayout, pAllocator);
}

#if VK_HEADER_VERSION >= 254

VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthBias2EXT( VkCommandBuffer  commandBuffer, const VkDepthBiasInfoEXT * pDepthBiasInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  ,const VkDepthBiasInfoEXT * ))
        _android_vulkan_dlsym("vkCmdSetDepthBias2EXT"))
            (commandBuffer, pDepthBiasInfo);
}

#endif

VKAPI_ATTR VkResult VKAPI_CALL vkAcquireDrmDisplayEXT( VkPhysicalDevice  physicalDevice,  int32_t  drmFd,  VkDisplayKHR  display)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkPhysicalDevice  , int32_t  , VkDisplayKHR  ))
        _android_vulkan_dlsym("vkAcquireDrmDisplayEXT"))
            (physicalDevice, drmFd, display);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetDrmDisplayEXT( VkPhysicalDevice  physicalDevice,  int32_t  drmFd,  uint32_t  connectorId,  VkDisplayKHR * display)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkPhysicalDevice  , int32_t  , uint32_t  , VkDisplayKHR * ))
        _android_vulkan_dlsym("vkGetDrmDisplayEXT"))
            (physicalDevice, drmFd, connectorId, display);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreatePrivateDataSlotEXT( VkDevice  device, const VkPrivateDataSlotCreateInfo * pCreateInfo, const VkAllocationCallbacks * pAllocator,  VkPrivateDataSlot * pPrivateDataSlot)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  ,const VkPrivateDataSlotCreateInfo * ,const VkAllocationCallbacks * , VkPrivateDataSlot * ))
        _android_vulkan_dlsym("vkCreatePrivateDataSlotEXT"))
            (device, pCreateInfo, pAllocator, pPrivateDataSlot);
}

VKAPI_ATTR void VKAPI_CALL vkDestroyPrivateDataSlotEXT( VkDevice  device,  VkPrivateDataSlot  privateDataSlot, const VkAllocationCallbacks * pAllocator)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  , VkPrivateDataSlot  ,const VkAllocationCallbacks * ))
        _android_vulkan_dlsym("vkDestroyPrivateDataSlotEXT"))
            (device, privateDataSlot, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL vkSetPrivateDataEXT( VkDevice  device,  VkObjectType  objectType,  uint64_t  objectHandle,  VkPrivateDataSlot  privateDataSlot,  uint64_t  data)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , VkObjectType  , uint64_t  , VkPrivateDataSlot  , uint64_t  ))
        _android_vulkan_dlsym("vkSetPrivateDataEXT"))
            (device, objectType, objectHandle, privateDataSlot, data);
}

VKAPI_ATTR void VKAPI_CALL vkGetPrivateDataEXT( VkDevice  device,  VkObjectType  objectType,  uint64_t  objectHandle,  VkPrivateDataSlot  privateDataSlot,  uint64_t * pData)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  , VkObjectType  , uint64_t  , VkPrivateDataSlot  , uint64_t * ))
        _android_vulkan_dlsym("vkGetPrivateDataEXT"))
            (device, objectType, objectHandle, privateDataSlot, pData);
}

#if VK_HEADER_VERSION >= 269 && defined(VK_ENABLE_BETA_EXTENSIONS)

VKAPI_ATTR VkResult VKAPI_CALL vkCreateCudaModuleNV( VkDevice  device, const VkCudaModuleCreateInfoNV * pCreateInfo, const VkAllocationCallbacks * pAllocator,  VkCudaModuleNV * pModule)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  ,const VkCudaModuleCreateInfoNV * ,const VkAllocationCallbacks * , VkCudaModuleNV * ))
        _android_vulkan_dlsym("vkCreateCudaModuleNV"))
            (device, pCreateInfo, pAllocator, pModule);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetCudaModuleCacheNV( VkDevice  device,  VkCudaModuleNV  module,  size_t * pCacheSize,  void * pCacheData)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , VkCudaModuleNV  , size_t * , void * ))
        _android_vulkan_dlsym("vkGetCudaModuleCacheNV"))
            (device, module, pCacheSize, pCacheData);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateCudaFunctionNV( VkDevice  device, const VkCudaFunctionCreateInfoNV * pCreateInfo, const VkAllocationCallbacks * pAllocator,  VkCudaFunctionNV * pFunction)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  ,const VkCudaFunctionCreateInfoNV * ,const VkAllocationCallbacks * , VkCudaFunctionNV * ))
        _android_vulkan_dlsym("vkCreateCudaFunctionNV"))
            (device, pCreateInfo, pAllocator, pFunction);
}

VKAPI_ATTR void VKAPI_CALL vkDestroyCudaModuleNV( VkDevice  device,  VkCudaModuleNV  module, const VkAllocationCallbacks * pAllocator)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  , VkCudaModuleNV  ,const VkAllocationCallbacks * ))
        _android_vulkan_dlsym("vkDestroyCudaModuleNV"))
            (device, module, pAllocator);
}

VKAPI_ATTR void VKAPI_CALL vkDestroyCudaFunctionNV( VkDevice  device,  VkCudaFunctionNV  function, const VkAllocationCallbacks * pAllocator)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  , VkCudaFunctionNV  ,const VkAllocationCallbacks * ))
        _android_vulkan_dlsym("vkDestroyCudaFunctionNV"))
            (device, function, pAllocator);
}

VKAPI_ATTR void VKAPI_CALL vkCmdCudaLaunchKernelNV( VkCommandBuffer  commandBuffer, const VkCudaLaunchInfoNV * pLaunchInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  ,const VkCudaLaunchInfoNV * ))
        _android_vulkan_dlsym("vkCmdCudaLaunchKernelNV"))
            (commandBuffer, pLaunchInfo);
}

#endif

#if VK_HEADER_VERSION >= 235

VKAPI_ATTR void VKAPI_CALL vkGetDescriptorSetLayoutSizeEXT( VkDevice  device,  VkDescriptorSetLayout  layout,  VkDeviceSize * pLayoutSizeInBytes)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  , VkDescriptorSetLayout  , VkDeviceSize * ))
        _android_vulkan_dlsym("vkGetDescriptorSetLayoutSizeEXT"))
            (device, layout, pLayoutSizeInBytes);
}

VKAPI_ATTR void VKAPI_CALL vkGetDescriptorSetLayoutBindingOffsetEXT( VkDevice  device,  VkDescriptorSetLayout  layout,  uint32_t  binding,  VkDeviceSize * pOffset)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  , VkDescriptorSetLayout  , uint32_t  , VkDeviceSize * ))
        _android_vulkan_dlsym("vkGetDescriptorSetLayoutBindingOffsetEXT"))
            (device, layout, binding, pOffset);
}

VKAPI_ATTR void VKAPI_CALL vkGetDescriptorEXT( VkDevice  device, const VkDescriptorGetInfoEXT * pDescriptorInfo,  size_t  dataSize,  void * pDescriptor)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  ,const VkDescriptorGetInfoEXT * , size_t  , void * ))
        _android_vulkan_dlsym("vkGetDescriptorEXT"))
            (device, pDescriptorInfo, dataSize, pDescriptor);
}

VKAPI_ATTR void VKAPI_CALL vkCmdBindDescriptorBuffersEXT( VkCommandBuffer  commandBuffer,  uint32_t  bufferCount, const VkDescriptorBufferBindingInfoEXT * pBindingInfos)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , uint32_t  ,const VkDescriptorBufferBindingInfoEXT * ))
        _android_vulkan_dlsym("vkCmdBindDescriptorBuffersEXT"))
            (commandBuffer, bufferCount, pBindingInfos);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetDescriptorBufferOffsetsEXT( VkCommandBuffer  commandBuffer,  VkPipelineBindPoint  pipelineBindPoint,  VkPipelineLayout  layout,  uint32_t  firstSet,  uint32_t  setCount, const uint32_t * pBufferIndices, const VkDeviceSize * pOffsets)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkPipelineBindPoint  , VkPipelineLayout  , uint32_t  , uint32_t  ,const uint32_t * ,const VkDeviceSize * ))
        _android_vulkan_dlsym("vkCmdSetDescriptorBufferOffsetsEXT"))
            (commandBuffer, pipelineBindPoint, layout, firstSet, setCount, pBufferIndices, pOffsets);
}

VKAPI_ATTR void VKAPI_CALL vkCmdBindDescriptorBufferEmbeddedSamplersEXT( VkCommandBuffer  commandBuffer,  VkPipelineBindPoint  pipelineBindPoint,  VkPipelineLayout  layout,  uint32_t  set)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkPipelineBindPoint  , VkPipelineLayout  , uint32_t  ))
        _android_vulkan_dlsym("vkCmdBindDescriptorBufferEmbeddedSamplersEXT"))
            (commandBuffer, pipelineBindPoint, layout, set);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetBufferOpaqueCaptureDescriptorDataEXT( VkDevice  device, const VkBufferCaptureDescriptorDataInfoEXT * pInfo,  void * pData)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  ,const VkBufferCaptureDescriptorDataInfoEXT * , void * ))
        _android_vulkan_dlsym("vkGetBufferOpaqueCaptureDescriptorDataEXT"))
            (device, pInfo, pData);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetImageOpaqueCaptureDescriptorDataEXT( VkDevice  device, const VkImageCaptureDescriptorDataInfoEXT * pInfo,  void * pData)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  ,const VkImageCaptureDescriptorDataInfoEXT * , void * ))
        _android_vulkan_dlsym("vkGetImageOpaqueCaptureDescriptorDataEXT"))
            (device, pInfo, pData);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetImageViewOpaqueCaptureDescriptorDataEXT( VkDevice  device, const VkImageViewCaptureDescriptorDataInfoEXT * pInfo,  void * pData)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  ,const VkImageViewCaptureDescriptorDataInfoEXT * , void * ))
        _android_vulkan_dlsym("vkGetImageViewOpaqueCaptureDescriptorDataEXT"))
            (device, pInfo, pData);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetSamplerOpaqueCaptureDescriptorDataEXT( VkDevice  device, const VkSamplerCaptureDescriptorDataInfoEXT * pInfo,  void * pData)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  ,const VkSamplerCaptureDescriptorDataInfoEXT * , void * ))
        _android_vulkan_dlsym("vkGetSamplerOpaqueCaptureDescriptorDataEXT"))
            (device, pInfo, pData);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetAccelerationStructureOpaqueCaptureDescriptorDataEXT( VkDevice  device, const VkAccelerationStructureCaptureDescriptorDataInfoEXT * pInfo,  void * pData)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  ,const VkAccelerationStructureCaptureDescriptorDataInfoEXT * , void * ))
        _android_vulkan_dlsym("vkGetAccelerationStructureOpaqueCaptureDescriptorDataEXT"))
            (device, pInfo, pData);
}

#endif

VKAPI_ATTR void VKAPI_CALL vkCmdSetFragmentShadingRateEnumNV( VkCommandBuffer  commandBuffer,  VkFragmentShadingRateNV  shadingRate, const VkFragmentShadingRateCombinerOpKHR  combinerOps[2])
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkFragmentShadingRateNV  ,const VkFragmentShadingRateCombinerOpKHR  [2]))
        _android_vulkan_dlsym("vkCmdSetFragmentShadingRateEnumNV"))
            (commandBuffer, shadingRate, combinerOps);
}

#if VK_HEADER_VERSION >= 230

VKAPI_ATTR VkResult VKAPI_CALL vkGetDeviceFaultInfoEXT( VkDevice  device,  VkDeviceFaultCountsEXT * pFaultCounts,  VkDeviceFaultInfoEXT * pFaultInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , VkDeviceFaultCountsEXT * , VkDeviceFaultInfoEXT * ))
        _android_vulkan_dlsym("vkGetDeviceFaultInfoEXT"))
            (device, pFaultCounts, pFaultInfo);
}

#endif

VKAPI_ATTR void VKAPI_CALL vkCmdSetVertexInputEXT( VkCommandBuffer  commandBuffer,  uint32_t  vertexBindingDescriptionCount, const VkVertexInputBindingDescription2EXT * pVertexBindingDescriptions,  uint32_t  vertexAttributeDescriptionCount, const VkVertexInputAttributeDescription2EXT * pVertexAttributeDescriptions)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , uint32_t  ,const VkVertexInputBindingDescription2EXT * , uint32_t  ,const VkVertexInputAttributeDescription2EXT * ))
        _android_vulkan_dlsym("vkCmdSetVertexInputEXT"))
            (commandBuffer, vertexBindingDescriptionCount, pVertexBindingDescriptions, vertexAttributeDescriptionCount, pVertexAttributeDescriptions);
}

#if VK_HEADER_VERSION >= 184

VKAPI_ATTR VkResult VKAPI_CALL vkGetDeviceSubpassShadingMaxWorkgroupSizeHUAWEI( VkDevice  device,  VkRenderPass  renderpass,  VkExtent2D * pMaxWorkgroupSize)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , VkRenderPass  , VkExtent2D * ))
        _android_vulkan_dlsym("vkGetDeviceSubpassShadingMaxWorkgroupSizeHUAWEI"))
            (device, renderpass, pMaxWorkgroupSize);
}

#endif

VKAPI_ATTR void VKAPI_CALL vkCmdSubpassShadingHUAWEI( VkCommandBuffer  commandBuffer)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  ))
        _android_vulkan_dlsym("vkCmdSubpassShadingHUAWEI"))
            (commandBuffer);
}

#if VK_HEADER_VERSION >= 185

VKAPI_ATTR void VKAPI_CALL vkCmdBindInvocationMaskHUAWEI( VkCommandBuffer  commandBuffer,  VkImageView  imageView,  VkImageLayout  imageLayout)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkImageView  , VkImageLayout  ))
        _android_vulkan_dlsym("vkCmdBindInvocationMaskHUAWEI"))
            (commandBuffer, imageView, imageLayout);
}

#endif

#if VK_HEADER_VERSION >= 184

VKAPI_ATTR VkResult VKAPI_CALL vkGetMemoryRemoteAddressNV( VkDevice  device, const VkMemoryGetRemoteAddressInfoNV * pMemoryGetRemoteAddressInfo,  VkRemoteAddressNV * pAddress)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  ,const VkMemoryGetRemoteAddressInfoNV * , VkRemoteAddressNV * ))
        _android_vulkan_dlsym("vkGetMemoryRemoteAddressNV"))
            (device, pMemoryGetRemoteAddressInfo, pAddress);
}

#endif

#if VK_HEADER_VERSION >= 213

VKAPI_ATTR VkResult VKAPI_CALL vkGetPipelinePropertiesEXT( VkDevice  device, const VkPipelineInfoEXT * pPipelineInfo,  VkBaseOutStructure * pPipelineProperties)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  ,const VkPipelineInfoEXT * , VkBaseOutStructure * ))
        _android_vulkan_dlsym("vkGetPipelinePropertiesEXT"))
            (device, pPipelineInfo, pPipelineProperties);
}

#endif

VKAPI_ATTR void VKAPI_CALL vkCmdSetPatchControlPointsEXT( VkCommandBuffer  commandBuffer,  uint32_t  patchControlPoints)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , uint32_t  ))
        _android_vulkan_dlsym("vkCmdSetPatchControlPointsEXT"))
            (commandBuffer, patchControlPoints);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetRasterizerDiscardEnableEXT( VkCommandBuffer  commandBuffer,  VkBool32  rasterizerDiscardEnable)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkBool32  ))
        _android_vulkan_dlsym("vkCmdSetRasterizerDiscardEnableEXT"))
            (commandBuffer, rasterizerDiscardEnable);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthBiasEnableEXT( VkCommandBuffer  commandBuffer,  VkBool32  depthBiasEnable)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkBool32  ))
        _android_vulkan_dlsym("vkCmdSetDepthBiasEnableEXT"))
            (commandBuffer, depthBiasEnable);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetLogicOpEXT( VkCommandBuffer  commandBuffer,  VkLogicOp  logicOp)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkLogicOp  ))
        _android_vulkan_dlsym("vkCmdSetLogicOpEXT"))
            (commandBuffer, logicOp);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetPrimitiveRestartEnableEXT( VkCommandBuffer  commandBuffer,  VkBool32  primitiveRestartEnable)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkBool32  ))
        _android_vulkan_dlsym("vkCmdSetPrimitiveRestartEnableEXT"))
            (commandBuffer, primitiveRestartEnable);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetColorWriteEnableEXT( VkCommandBuffer  commandBuffer,  uint32_t  attachmentCount, const VkBool32 * pColorWriteEnables)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , uint32_t  ,const VkBool32 * ))
        _android_vulkan_dlsym("vkCmdSetColorWriteEnableEXT"))
            (commandBuffer, attachmentCount, pColorWriteEnables);
}

VKAPI_ATTR void VKAPI_CALL vkCmdDrawMultiEXT( VkCommandBuffer  commandBuffer,  uint32_t  drawCount, const VkMultiDrawInfoEXT * pVertexInfo,  uint32_t  instanceCount,  uint32_t  firstInstance,  uint32_t  stride)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , uint32_t  ,const VkMultiDrawInfoEXT * , uint32_t  , uint32_t  , uint32_t  ))
        _android_vulkan_dlsym("vkCmdDrawMultiEXT"))
            (commandBuffer, drawCount, pVertexInfo, instanceCount, firstInstance, stride);
}

VKAPI_ATTR void VKAPI_CALL vkCmdDrawMultiIndexedEXT( VkCommandBuffer  commandBuffer,  uint32_t  drawCount, const VkMultiDrawIndexedInfoEXT * pIndexInfo,  uint32_t  instanceCount,  uint32_t  firstInstance,  uint32_t  stride, const int32_t * pVertexOffset)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , uint32_t  ,const VkMultiDrawIndexedInfoEXT * , uint32_t  , uint32_t  , uint32_t  ,const int32_t * ))
        _android_vulkan_dlsym("vkCmdDrawMultiIndexedEXT"))
            (commandBuffer, drawCount, pIndexInfo, instanceCount, firstInstance, stride, pVertexOffset);
}

#if VK_HEADER_VERSION >= 230

VKAPI_ATTR VkResult VKAPI_CALL vkCreateMicromapEXT( VkDevice  device, const VkMicromapCreateInfoEXT * pCreateInfo, const VkAllocationCallbacks * pAllocator,  VkMicromapEXT * pMicromap)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  ,const VkMicromapCreateInfoEXT * ,const VkAllocationCallbacks * , VkMicromapEXT * ))
        _android_vulkan_dlsym("vkCreateMicromapEXT"))
            (device, pCreateInfo, pAllocator, pMicromap);
}

VKAPI_ATTR void VKAPI_CALL vkDestroyMicromapEXT( VkDevice  device,  VkMicromapEXT  micromap, const VkAllocationCallbacks * pAllocator)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  , VkMicromapEXT  ,const VkAllocationCallbacks * ))
        _android_vulkan_dlsym("vkDestroyMicromapEXT"))
            (device, micromap, pAllocator);
}

VKAPI_ATTR void VKAPI_CALL vkCmdBuildMicromapsEXT( VkCommandBuffer  commandBuffer,  uint32_t  infoCount, const VkMicromapBuildInfoEXT * pInfos)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , uint32_t  ,const VkMicromapBuildInfoEXT * ))
        _android_vulkan_dlsym("vkCmdBuildMicromapsEXT"))
            (commandBuffer, infoCount, pInfos);
}

VKAPI_ATTR VkResult VKAPI_CALL vkBuildMicromapsEXT( VkDevice  device,  VkDeferredOperationKHR  deferredOperation,  uint32_t  infoCount, const VkMicromapBuildInfoEXT * pInfos)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , VkDeferredOperationKHR  , uint32_t  ,const VkMicromapBuildInfoEXT * ))
        _android_vulkan_dlsym("vkBuildMicromapsEXT"))
            (device, deferredOperation, infoCount, pInfos);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCopyMicromapEXT( VkDevice  device,  VkDeferredOperationKHR  deferredOperation, const VkCopyMicromapInfoEXT * pInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , VkDeferredOperationKHR  ,const VkCopyMicromapInfoEXT * ))
        _android_vulkan_dlsym("vkCopyMicromapEXT"))
            (device, deferredOperation, pInfo);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCopyMicromapToMemoryEXT( VkDevice  device,  VkDeferredOperationKHR  deferredOperation, const VkCopyMicromapToMemoryInfoEXT * pInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , VkDeferredOperationKHR  ,const VkCopyMicromapToMemoryInfoEXT * ))
        _android_vulkan_dlsym("vkCopyMicromapToMemoryEXT"))
            (device, deferredOperation, pInfo);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCopyMemoryToMicromapEXT( VkDevice  device,  VkDeferredOperationKHR  deferredOperation, const VkCopyMemoryToMicromapInfoEXT * pInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , VkDeferredOperationKHR  ,const VkCopyMemoryToMicromapInfoEXT * ))
        _android_vulkan_dlsym("vkCopyMemoryToMicromapEXT"))
            (device, deferredOperation, pInfo);
}

VKAPI_ATTR VkResult VKAPI_CALL vkWriteMicromapsPropertiesEXT( VkDevice  device,  uint32_t  micromapCount, const VkMicromapEXT * pMicromaps,  VkQueryType  queryType,  size_t  dataSize,  void * pData,  size_t  stride)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , uint32_t  ,const VkMicromapEXT * , VkQueryType  , size_t  , void * , size_t  ))
        _android_vulkan_dlsym("vkWriteMicromapsPropertiesEXT"))
            (device, micromapCount, pMicromaps, queryType, dataSize, pData, stride);
}

VKAPI_ATTR void VKAPI_CALL vkCmdCopyMicromapEXT( VkCommandBuffer  commandBuffer, const VkCopyMicromapInfoEXT * pInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  ,const VkCopyMicromapInfoEXT * ))
        _android_vulkan_dlsym("vkCmdCopyMicromapEXT"))
            (commandBuffer, pInfo);
}

VKAPI_ATTR void VKAPI_CALL vkCmdCopyMicromapToMemoryEXT( VkCommandBuffer  commandBuffer, const VkCopyMicromapToMemoryInfoEXT * pInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  ,const VkCopyMicromapToMemoryInfoEXT * ))
        _android_vulkan_dlsym("vkCmdCopyMicromapToMemoryEXT"))
            (commandBuffer, pInfo);
}

VKAPI_ATTR void VKAPI_CALL vkCmdCopyMemoryToMicromapEXT( VkCommandBuffer  commandBuffer, const VkCopyMemoryToMicromapInfoEXT * pInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  ,const VkCopyMemoryToMicromapInfoEXT * ))
        _android_vulkan_dlsym("vkCmdCopyMemoryToMicromapEXT"))
            (commandBuffer, pInfo);
}

VKAPI_ATTR void VKAPI_CALL vkCmdWriteMicromapsPropertiesEXT( VkCommandBuffer  commandBuffer,  uint32_t  micromapCount, const VkMicromapEXT * pMicromaps,  VkQueryType  queryType,  VkQueryPool  queryPool,  uint32_t  firstQuery)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , uint32_t  ,const VkMicromapEXT * , VkQueryType  , VkQueryPool  , uint32_t  ))
        _android_vulkan_dlsym("vkCmdWriteMicromapsPropertiesEXT"))
            (commandBuffer, micromapCount, pMicromaps, queryType, queryPool, firstQuery);
}

VKAPI_ATTR void VKAPI_CALL vkGetDeviceMicromapCompatibilityEXT( VkDevice  device, const VkMicromapVersionInfoEXT * pVersionInfo,  VkAccelerationStructureCompatibilityKHR * pCompatibility)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  ,const VkMicromapVersionInfoEXT * , VkAccelerationStructureCompatibilityKHR * ))
        _android_vulkan_dlsym("vkGetDeviceMicromapCompatibilityEXT"))
            (device, pVersionInfo, pCompatibility);
}

VKAPI_ATTR void VKAPI_CALL vkGetMicromapBuildSizesEXT( VkDevice  device,  VkAccelerationStructureBuildTypeKHR  buildType, const VkMicromapBuildInfoEXT * pBuildInfo,  VkMicromapBuildSizesInfoEXT * pSizeInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  , VkAccelerationStructureBuildTypeKHR  ,const VkMicromapBuildInfoEXT * , VkMicromapBuildSizesInfoEXT * ))
        _android_vulkan_dlsym("vkGetMicromapBuildSizesEXT"))
            (device, buildType, pBuildInfo, pSizeInfo);
}

#endif

#if VK_HEADER_VERSION >= 239

VKAPI_ATTR void VKAPI_CALL vkCmdDrawClusterHUAWEI( VkCommandBuffer  commandBuffer,  uint32_t  groupCountX,  uint32_t  groupCountY,  uint32_t  groupCountZ)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , uint32_t  , uint32_t  , uint32_t  ))
        _android_vulkan_dlsym("vkCmdDrawClusterHUAWEI"))
            (commandBuffer, groupCountX, groupCountY, groupCountZ);
}

VKAPI_ATTR void VKAPI_CALL vkCmdDrawClusterIndirectHUAWEI( VkCommandBuffer  commandBuffer,  VkBuffer  buffer,  VkDeviceSize  offset)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkBuffer  , VkDeviceSize  ))
        _android_vulkan_dlsym("vkCmdDrawClusterIndirectHUAWEI"))
            (commandBuffer, buffer, offset);
}

#endif

#if VK_HEADER_VERSION >= 191

VKAPI_ATTR void VKAPI_CALL vkSetDeviceMemoryPriorityEXT( VkDevice  device,  VkDeviceMemory  memory,  float  priority)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  , VkDeviceMemory  , float  ))
        _android_vulkan_dlsym("vkSetDeviceMemoryPriorityEXT"))
            (device, memory, priority);
}

#endif

#if VK_HEADER_VERSION >= 207

VKAPI_ATTR void VKAPI_CALL vkGetDescriptorSetLayoutHostMappingInfoVALVE( VkDevice  device, const VkDescriptorSetBindingReferenceVALVE * pBindingReference,  VkDescriptorSetLayoutHostMappingInfoVALVE * pHostMapping)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  ,const VkDescriptorSetBindingReferenceVALVE * , VkDescriptorSetLayoutHostMappingInfoVALVE * ))
        _android_vulkan_dlsym("vkGetDescriptorSetLayoutHostMappingInfoVALVE"))
            (device, pBindingReference, pHostMapping);
}

VKAPI_ATTR void VKAPI_CALL vkGetDescriptorSetHostMappingVALVE( VkDevice  device,  VkDescriptorSet  descriptorSet,  void ** ppData)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  , VkDescriptorSet  , void ** ))
        _android_vulkan_dlsym("vkGetDescriptorSetHostMappingVALVE"))
            (device, descriptorSet, ppData);
}

#endif

#if VK_HEADER_VERSION >= 233

VKAPI_ATTR void VKAPI_CALL vkCmdCopyMemoryIndirectNV( VkCommandBuffer  commandBuffer,  VkDeviceAddress  copyBufferAddress,  uint32_t  copyCount,  uint32_t  stride)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkDeviceAddress  , uint32_t  , uint32_t  ))
        _android_vulkan_dlsym("vkCmdCopyMemoryIndirectNV"))
            (commandBuffer, copyBufferAddress, copyCount, stride);
}

VKAPI_ATTR void VKAPI_CALL vkCmdCopyMemoryToImageIndirectNV( VkCommandBuffer  commandBuffer,  VkDeviceAddress  copyBufferAddress,  uint32_t  copyCount,  uint32_t  stride,  VkImage  dstImage,  VkImageLayout  dstImageLayout, const VkImageSubresourceLayers * pImageSubresources)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkDeviceAddress  , uint32_t  , uint32_t  , VkImage  , VkImageLayout  ,const VkImageSubresourceLayers * ))
        _android_vulkan_dlsym("vkCmdCopyMemoryToImageIndirectNV"))
            (commandBuffer, copyBufferAddress, copyCount, stride, dstImage, dstImageLayout, pImageSubresources);
}

VKAPI_ATTR void VKAPI_CALL vkCmdDecompressMemoryNV( VkCommandBuffer  commandBuffer,  uint32_t  decompressRegionCount, const VkDecompressMemoryRegionNV * pDecompressMemoryRegions)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , uint32_t  ,const VkDecompressMemoryRegionNV * ))
        _android_vulkan_dlsym("vkCmdDecompressMemoryNV"))
            (commandBuffer, decompressRegionCount, pDecompressMemoryRegions);
}

VKAPI_ATTR void VKAPI_CALL vkCmdDecompressMemoryIndirectCountNV( VkCommandBuffer  commandBuffer,  VkDeviceAddress  indirectCommandsAddress,  VkDeviceAddress  indirectCommandsCountAddress,  uint32_t  stride)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkDeviceAddress  , VkDeviceAddress  , uint32_t  ))
        _android_vulkan_dlsym("vkCmdDecompressMemoryIndirectCountNV"))
            (commandBuffer, indirectCommandsAddress, indirectCommandsCountAddress, stride);
}

#endif

#if VK_HEADER_VERSION >= 258

VKAPI_ATTR void VKAPI_CALL vkGetPipelineIndirectMemoryRequirementsNV( VkDevice  device, const VkComputePipelineCreateInfo * pCreateInfo,  VkMemoryRequirements2 * pMemoryRequirements)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  ,const VkComputePipelineCreateInfo * , VkMemoryRequirements2 * ))
        _android_vulkan_dlsym("vkGetPipelineIndirectMemoryRequirementsNV"))
            (device, pCreateInfo, pMemoryRequirements);
}

#endif

#if VK_HEADER_VERSION == 258

#endif

#if VK_HEADER_VERSION >= 259

VKAPI_ATTR void VKAPI_CALL vkCmdUpdatePipelineIndirectBufferNV( VkCommandBuffer  commandBuffer,  VkPipelineBindPoint  pipelineBindPoint,  VkPipeline  pipeline)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkPipelineBindPoint  , VkPipeline  ))
        _android_vulkan_dlsym("vkCmdUpdatePipelineIndirectBufferNV"))
            (commandBuffer, pipelineBindPoint, pipeline);
}

#endif

#if VK_HEADER_VERSION >= 258

VKAPI_ATTR VkDeviceAddress VKAPI_CALL vkGetPipelineIndirectDeviceAddressNV( VkDevice  device, const VkPipelineIndirectDeviceAddressInfoNV * pInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkDeviceAddress (*)( VkDevice  ,const VkPipelineIndirectDeviceAddressInfoNV * ))
        _android_vulkan_dlsym("vkGetPipelineIndirectDeviceAddressNV"))
            (device, pInfo);
}

#endif

#if VK_HEADER_VERSION >= 230

VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthClampEnableEXT( VkCommandBuffer  commandBuffer,  VkBool32  depthClampEnable)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkBool32  ))
        _android_vulkan_dlsym("vkCmdSetDepthClampEnableEXT"))
            (commandBuffer, depthClampEnable);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetPolygonModeEXT( VkCommandBuffer  commandBuffer,  VkPolygonMode  polygonMode)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkPolygonMode  ))
        _android_vulkan_dlsym("vkCmdSetPolygonModeEXT"))
            (commandBuffer, polygonMode);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetRasterizationSamplesEXT( VkCommandBuffer  commandBuffer,  VkSampleCountFlagBits  rasterizationSamples)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkSampleCountFlagBits  ))
        _android_vulkan_dlsym("vkCmdSetRasterizationSamplesEXT"))
            (commandBuffer, rasterizationSamples);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetSampleMaskEXT( VkCommandBuffer  commandBuffer,  VkSampleCountFlagBits  samples, const VkSampleMask * pSampleMask)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkSampleCountFlagBits  ,const VkSampleMask * ))
        _android_vulkan_dlsym("vkCmdSetSampleMaskEXT"))
            (commandBuffer, samples, pSampleMask);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetAlphaToCoverageEnableEXT( VkCommandBuffer  commandBuffer,  VkBool32  alphaToCoverageEnable)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkBool32  ))
        _android_vulkan_dlsym("vkCmdSetAlphaToCoverageEnableEXT"))
            (commandBuffer, alphaToCoverageEnable);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetAlphaToOneEnableEXT( VkCommandBuffer  commandBuffer,  VkBool32  alphaToOneEnable)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkBool32  ))
        _android_vulkan_dlsym("vkCmdSetAlphaToOneEnableEXT"))
            (commandBuffer, alphaToOneEnable);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetLogicOpEnableEXT( VkCommandBuffer  commandBuffer,  VkBool32  logicOpEnable)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkBool32  ))
        _android_vulkan_dlsym("vkCmdSetLogicOpEnableEXT"))
            (commandBuffer, logicOpEnable);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetColorBlendEnableEXT( VkCommandBuffer  commandBuffer,  uint32_t  firstAttachment,  uint32_t  attachmentCount, const VkBool32 * pColorBlendEnables)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , uint32_t  , uint32_t  ,const VkBool32 * ))
        _android_vulkan_dlsym("vkCmdSetColorBlendEnableEXT"))
            (commandBuffer, firstAttachment, attachmentCount, pColorBlendEnables);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetColorBlendEquationEXT( VkCommandBuffer  commandBuffer,  uint32_t  firstAttachment,  uint32_t  attachmentCount, const VkColorBlendEquationEXT * pColorBlendEquations)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , uint32_t  , uint32_t  ,const VkColorBlendEquationEXT * ))
        _android_vulkan_dlsym("vkCmdSetColorBlendEquationEXT"))
            (commandBuffer, firstAttachment, attachmentCount, pColorBlendEquations);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetColorWriteMaskEXT( VkCommandBuffer  commandBuffer,  uint32_t  firstAttachment,  uint32_t  attachmentCount, const VkColorComponentFlags * pColorWriteMasks)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , uint32_t  , uint32_t  ,const VkColorComponentFlags * ))
        _android_vulkan_dlsym("vkCmdSetColorWriteMaskEXT"))
            (commandBuffer, firstAttachment, attachmentCount, pColorWriteMasks);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetTessellationDomainOriginEXT( VkCommandBuffer  commandBuffer,  VkTessellationDomainOrigin  domainOrigin)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkTessellationDomainOrigin  ))
        _android_vulkan_dlsym("vkCmdSetTessellationDomainOriginEXT"))
            (commandBuffer, domainOrigin);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetRasterizationStreamEXT( VkCommandBuffer  commandBuffer,  uint32_t  rasterizationStream)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , uint32_t  ))
        _android_vulkan_dlsym("vkCmdSetRasterizationStreamEXT"))
            (commandBuffer, rasterizationStream);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetConservativeRasterizationModeEXT( VkCommandBuffer  commandBuffer,  VkConservativeRasterizationModeEXT  conservativeRasterizationMode)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkConservativeRasterizationModeEXT  ))
        _android_vulkan_dlsym("vkCmdSetConservativeRasterizationModeEXT"))
            (commandBuffer, conservativeRasterizationMode);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetExtraPrimitiveOverestimationSizeEXT( VkCommandBuffer  commandBuffer,  float  extraPrimitiveOverestimationSize)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , float  ))
        _android_vulkan_dlsym("vkCmdSetExtraPrimitiveOverestimationSizeEXT"))
            (commandBuffer, extraPrimitiveOverestimationSize);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthClipEnableEXT( VkCommandBuffer  commandBuffer,  VkBool32  depthClipEnable)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkBool32  ))
        _android_vulkan_dlsym("vkCmdSetDepthClipEnableEXT"))
            (commandBuffer, depthClipEnable);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetSampleLocationsEnableEXT( VkCommandBuffer  commandBuffer,  VkBool32  sampleLocationsEnable)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkBool32  ))
        _android_vulkan_dlsym("vkCmdSetSampleLocationsEnableEXT"))
            (commandBuffer, sampleLocationsEnable);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetColorBlendAdvancedEXT( VkCommandBuffer  commandBuffer,  uint32_t  firstAttachment,  uint32_t  attachmentCount, const VkColorBlendAdvancedEXT * pColorBlendAdvanced)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , uint32_t  , uint32_t  ,const VkColorBlendAdvancedEXT * ))
        _android_vulkan_dlsym("vkCmdSetColorBlendAdvancedEXT"))
            (commandBuffer, firstAttachment, attachmentCount, pColorBlendAdvanced);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetProvokingVertexModeEXT( VkCommandBuffer  commandBuffer,  VkProvokingVertexModeEXT  provokingVertexMode)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkProvokingVertexModeEXT  ))
        _android_vulkan_dlsym("vkCmdSetProvokingVertexModeEXT"))
            (commandBuffer, provokingVertexMode);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetLineRasterizationModeEXT( VkCommandBuffer  commandBuffer,  VkLineRasterizationModeEXT  lineRasterizationMode)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkLineRasterizationModeEXT  ))
        _android_vulkan_dlsym("vkCmdSetLineRasterizationModeEXT"))
            (commandBuffer, lineRasterizationMode);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetLineStippleEnableEXT( VkCommandBuffer  commandBuffer,  VkBool32  stippledLineEnable)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkBool32  ))
        _android_vulkan_dlsym("vkCmdSetLineStippleEnableEXT"))
            (commandBuffer, stippledLineEnable);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthClipNegativeOneToOneEXT( VkCommandBuffer  commandBuffer,  VkBool32  negativeOneToOne)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkBool32  ))
        _android_vulkan_dlsym("vkCmdSetDepthClipNegativeOneToOneEXT"))
            (commandBuffer, negativeOneToOne);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetViewportWScalingEnableNV( VkCommandBuffer  commandBuffer,  VkBool32  viewportWScalingEnable)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkBool32  ))
        _android_vulkan_dlsym("vkCmdSetViewportWScalingEnableNV"))
            (commandBuffer, viewportWScalingEnable);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetViewportSwizzleNV( VkCommandBuffer  commandBuffer,  uint32_t  firstViewport,  uint32_t  viewportCount, const VkViewportSwizzleNV * pViewportSwizzles)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , uint32_t  , uint32_t  ,const VkViewportSwizzleNV * ))
        _android_vulkan_dlsym("vkCmdSetViewportSwizzleNV"))
            (commandBuffer, firstViewport, viewportCount, pViewportSwizzles);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetCoverageToColorEnableNV( VkCommandBuffer  commandBuffer,  VkBool32  coverageToColorEnable)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkBool32  ))
        _android_vulkan_dlsym("vkCmdSetCoverageToColorEnableNV"))
            (commandBuffer, coverageToColorEnable);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetCoverageToColorLocationNV( VkCommandBuffer  commandBuffer,  uint32_t  coverageToColorLocation)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , uint32_t  ))
        _android_vulkan_dlsym("vkCmdSetCoverageToColorLocationNV"))
            (commandBuffer, coverageToColorLocation);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetCoverageModulationModeNV( VkCommandBuffer  commandBuffer,  VkCoverageModulationModeNV  coverageModulationMode)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkCoverageModulationModeNV  ))
        _android_vulkan_dlsym("vkCmdSetCoverageModulationModeNV"))
            (commandBuffer, coverageModulationMode);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetCoverageModulationTableEnableNV( VkCommandBuffer  commandBuffer,  VkBool32  coverageModulationTableEnable)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkBool32  ))
        _android_vulkan_dlsym("vkCmdSetCoverageModulationTableEnableNV"))
            (commandBuffer, coverageModulationTableEnable);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetCoverageModulationTableNV( VkCommandBuffer  commandBuffer,  uint32_t  coverageModulationTableCount, const float * pCoverageModulationTable)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , uint32_t  ,const float * ))
        _android_vulkan_dlsym("vkCmdSetCoverageModulationTableNV"))
            (commandBuffer, coverageModulationTableCount, pCoverageModulationTable);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetShadingRateImageEnableNV( VkCommandBuffer  commandBuffer,  VkBool32  shadingRateImageEnable)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkBool32  ))
        _android_vulkan_dlsym("vkCmdSetShadingRateImageEnableNV"))
            (commandBuffer, shadingRateImageEnable);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetRepresentativeFragmentTestEnableNV( VkCommandBuffer  commandBuffer,  VkBool32  representativeFragmentTestEnable)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkBool32  ))
        _android_vulkan_dlsym("vkCmdSetRepresentativeFragmentTestEnableNV"))
            (commandBuffer, representativeFragmentTestEnable);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetCoverageReductionModeNV( VkCommandBuffer  commandBuffer,  VkCoverageReductionModeNV  coverageReductionMode)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkCoverageReductionModeNV  ))
        _android_vulkan_dlsym("vkCmdSetCoverageReductionModeNV"))
            (commandBuffer, coverageReductionMode);
}

#endif

#if VK_HEADER_VERSION >= 219

VKAPI_ATTR void VKAPI_CALL vkGetShaderModuleIdentifierEXT( VkDevice  device,  VkShaderModule  shaderModule,  VkShaderModuleIdentifierEXT * pIdentifier)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  , VkShaderModule  , VkShaderModuleIdentifierEXT * ))
        _android_vulkan_dlsym("vkGetShaderModuleIdentifierEXT"))
            (device, shaderModule, pIdentifier);
}

VKAPI_ATTR void VKAPI_CALL vkGetShaderModuleCreateInfoIdentifierEXT( VkDevice  device, const VkShaderModuleCreateInfo * pCreateInfo,  VkShaderModuleIdentifierEXT * pIdentifier)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  ,const VkShaderModuleCreateInfo * , VkShaderModuleIdentifierEXT * ))
        _android_vulkan_dlsym("vkGetShaderModuleCreateInfoIdentifierEXT"))
            (device, pCreateInfo, pIdentifier);
}

#endif

#if VK_HEADER_VERSION >= 230

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceOpticalFlowImageFormatsNV( VkPhysicalDevice  physicalDevice, const VkOpticalFlowImageFormatInfoNV * pOpticalFlowImageFormatInfo,  uint32_t * pFormatCount,  VkOpticalFlowImageFormatPropertiesNV * pImageFormatProperties)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkPhysicalDevice  ,const VkOpticalFlowImageFormatInfoNV * , uint32_t * , VkOpticalFlowImageFormatPropertiesNV * ))
        _android_vulkan_dlsym("vkGetPhysicalDeviceOpticalFlowImageFormatsNV"))
            (physicalDevice, pOpticalFlowImageFormatInfo, pFormatCount, pImageFormatProperties);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateOpticalFlowSessionNV( VkDevice  device, const VkOpticalFlowSessionCreateInfoNV * pCreateInfo, const VkAllocationCallbacks * pAllocator,  VkOpticalFlowSessionNV * pSession)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  ,const VkOpticalFlowSessionCreateInfoNV * ,const VkAllocationCallbacks * , VkOpticalFlowSessionNV * ))
        _android_vulkan_dlsym("vkCreateOpticalFlowSessionNV"))
            (device, pCreateInfo, pAllocator, pSession);
}

VKAPI_ATTR void VKAPI_CALL vkDestroyOpticalFlowSessionNV( VkDevice  device,  VkOpticalFlowSessionNV  session, const VkAllocationCallbacks * pAllocator)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  , VkOpticalFlowSessionNV  ,const VkAllocationCallbacks * ))
        _android_vulkan_dlsym("vkDestroyOpticalFlowSessionNV"))
            (device, session, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL vkBindOpticalFlowSessionImageNV( VkDevice  device,  VkOpticalFlowSessionNV  session,  VkOpticalFlowSessionBindingPointNV  bindingPoint,  VkImageView  view,  VkImageLayout  layout)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , VkOpticalFlowSessionNV  , VkOpticalFlowSessionBindingPointNV  , VkImageView  , VkImageLayout  ))
        _android_vulkan_dlsym("vkBindOpticalFlowSessionImageNV"))
            (device, session, bindingPoint, view, layout);
}

VKAPI_ATTR void VKAPI_CALL vkCmdOpticalFlowExecuteNV( VkCommandBuffer  commandBuffer,  VkOpticalFlowSessionNV  session, const VkOpticalFlowExecuteInfoNV * pExecuteInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkOpticalFlowSessionNV  ,const VkOpticalFlowExecuteInfoNV * ))
        _android_vulkan_dlsym("vkCmdOpticalFlowExecuteNV"))
            (commandBuffer, session, pExecuteInfo);
}

#endif

#if VK_HEADER_VERSION >= 246

VKAPI_ATTR VkResult VKAPI_CALL vkCreateShadersEXT( VkDevice  device,  uint32_t  createInfoCount, const VkShaderCreateInfoEXT * pCreateInfos, const VkAllocationCallbacks * pAllocator,  VkShaderEXT * pShaders)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , uint32_t  ,const VkShaderCreateInfoEXT * ,const VkAllocationCallbacks * , VkShaderEXT * ))
        _android_vulkan_dlsym("vkCreateShadersEXT"))
            (device, createInfoCount, pCreateInfos, pAllocator, pShaders);
}

VKAPI_ATTR void VKAPI_CALL vkDestroyShaderEXT( VkDevice  device,  VkShaderEXT  shader, const VkAllocationCallbacks * pAllocator)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  , VkShaderEXT  ,const VkAllocationCallbacks * ))
        _android_vulkan_dlsym("vkDestroyShaderEXT"))
            (device, shader, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetShaderBinaryDataEXT( VkDevice  device,  VkShaderEXT  shader,  size_t * pDataSize,  void * pData)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , VkShaderEXT  , size_t * , void * ))
        _android_vulkan_dlsym("vkGetShaderBinaryDataEXT"))
            (device, shader, pDataSize, pData);
}

VKAPI_ATTR void VKAPI_CALL vkCmdBindShadersEXT( VkCommandBuffer  commandBuffer,  uint32_t  stageCount, const VkShaderStageFlagBits * pStages, const VkShaderEXT * pShaders)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , uint32_t  ,const VkShaderStageFlagBits * ,const VkShaderEXT * ))
        _android_vulkan_dlsym("vkCmdBindShadersEXT"))
            (commandBuffer, stageCount, pStages, pShaders);
}

#endif

#if VK_HEADER_VERSION >= 222

VKAPI_ATTR VkResult VKAPI_CALL vkGetFramebufferTilePropertiesQCOM( VkDevice  device,  VkFramebuffer  framebuffer,  uint32_t * pPropertiesCount,  VkTilePropertiesQCOM * pProperties)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , VkFramebuffer  , uint32_t * , VkTilePropertiesQCOM * ))
        _android_vulkan_dlsym("vkGetFramebufferTilePropertiesQCOM"))
            (device, framebuffer, pPropertiesCount, pProperties);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetDynamicRenderingTilePropertiesQCOM( VkDevice  device, const VkRenderingInfo * pRenderingInfo,  VkTilePropertiesQCOM * pProperties)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  ,const VkRenderingInfo * , VkTilePropertiesQCOM * ))
        _android_vulkan_dlsym("vkGetDynamicRenderingTilePropertiesQCOM"))
            (device, pRenderingInfo, pProperties);
}

#endif

#if VK_HEADER_VERSION >= 266

VKAPI_ATTR VkResult VKAPI_CALL vkSetLatencySleepModeNV( VkDevice  device,  VkSwapchainKHR  swapchain, const VkLatencySleepModeInfoNV * pSleepModeInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , VkSwapchainKHR  ,const VkLatencySleepModeInfoNV * ))
        _android_vulkan_dlsym("vkSetLatencySleepModeNV"))
            (device, swapchain, pSleepModeInfo);
}

VKAPI_ATTR VkResult VKAPI_CALL vkLatencySleepNV( VkDevice  device,  VkSwapchainKHR  swapchain, const VkLatencySleepInfoNV * pSleepInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , VkSwapchainKHR  ,const VkLatencySleepInfoNV * ))
        _android_vulkan_dlsym("vkLatencySleepNV"))
            (device, swapchain, pSleepInfo);
}

VKAPI_ATTR void VKAPI_CALL vkSetLatencyMarkerNV( VkDevice  device,  VkSwapchainKHR  swapchain, const VkSetLatencyMarkerInfoNV * pLatencyMarkerInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  , VkSwapchainKHR  ,const VkSetLatencyMarkerInfoNV * ))
        _android_vulkan_dlsym("vkSetLatencyMarkerNV"))
            (device, swapchain, pLatencyMarkerInfo);
}

VKAPI_ATTR void VKAPI_CALL vkGetLatencyTimingsNV( VkDevice  device,  VkSwapchainKHR  swapchain,  VkGetLatencyMarkerInfoNV * pLatencyMarkerInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  , VkSwapchainKHR  , VkGetLatencyMarkerInfoNV * ))
        _android_vulkan_dlsym("vkGetLatencyTimingsNV"))
            (device, swapchain, pLatencyMarkerInfo);
}

VKAPI_ATTR void VKAPI_CALL vkQueueNotifyOutOfBandNV( VkQueue  queue, const VkOutOfBandQueueTypeInfoNV * pQueueTypeInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkQueue  ,const VkOutOfBandQueueTypeInfoNV * ))
        _android_vulkan_dlsym("vkQueueNotifyOutOfBandNV"))
            (queue, pQueueTypeInfo);
}

#endif

#if VK_HEADER_VERSION >= 250

VKAPI_ATTR void VKAPI_CALL vkCmdSetAttachmentFeedbackLoopEnableEXT( VkCommandBuffer  commandBuffer,  VkImageAspectFlags  aspectMask)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkImageAspectFlags  ))
        _android_vulkan_dlsym("vkCmdSetAttachmentFeedbackLoopEnableEXT"))
            (commandBuffer, aspectMask);
}

#endif

VKAPI_ATTR VkResult VKAPI_CALL vkCreateAccelerationStructureKHR( VkDevice  device, const VkAccelerationStructureCreateInfoKHR * pCreateInfo, const VkAllocationCallbacks * pAllocator,  VkAccelerationStructureKHR * pAccelerationStructure)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  ,const VkAccelerationStructureCreateInfoKHR * ,const VkAllocationCallbacks * , VkAccelerationStructureKHR * ))
        _android_vulkan_dlsym("vkCreateAccelerationStructureKHR"))
            (device, pCreateInfo, pAllocator, pAccelerationStructure);
}

VKAPI_ATTR void VKAPI_CALL vkDestroyAccelerationStructureKHR( VkDevice  device,  VkAccelerationStructureKHR  accelerationStructure, const VkAllocationCallbacks * pAllocator)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  , VkAccelerationStructureKHR  ,const VkAllocationCallbacks * ))
        _android_vulkan_dlsym("vkDestroyAccelerationStructureKHR"))
            (device, accelerationStructure, pAllocator);
}

VKAPI_ATTR void VKAPI_CALL vkCmdBuildAccelerationStructuresKHR( VkCommandBuffer  commandBuffer,  uint32_t  infoCount, const VkAccelerationStructureBuildGeometryInfoKHR * pInfos, const VkAccelerationStructureBuildRangeInfoKHR * const* ppBuildRangeInfos)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , uint32_t  ,const VkAccelerationStructureBuildGeometryInfoKHR * ,const VkAccelerationStructureBuildRangeInfoKHR * const* ))
        _android_vulkan_dlsym("vkCmdBuildAccelerationStructuresKHR"))
            (commandBuffer, infoCount, pInfos, ppBuildRangeInfos);
}

VKAPI_ATTR void VKAPI_CALL vkCmdBuildAccelerationStructuresIndirectKHR( VkCommandBuffer  commandBuffer,  uint32_t  infoCount, const VkAccelerationStructureBuildGeometryInfoKHR * pInfos, const VkDeviceAddress * pIndirectDeviceAddresses, const uint32_t * pIndirectStrides, const uint32_t * const* ppMaxPrimitiveCounts)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , uint32_t  ,const VkAccelerationStructureBuildGeometryInfoKHR * ,const VkDeviceAddress * ,const uint32_t * ,const uint32_t * const* ))
        _android_vulkan_dlsym("vkCmdBuildAccelerationStructuresIndirectKHR"))
            (commandBuffer, infoCount, pInfos, pIndirectDeviceAddresses, pIndirectStrides, ppMaxPrimitiveCounts);
}

VKAPI_ATTR VkResult VKAPI_CALL vkBuildAccelerationStructuresKHR( VkDevice  device,  VkDeferredOperationKHR  deferredOperation,  uint32_t  infoCount, const VkAccelerationStructureBuildGeometryInfoKHR * pInfos, const VkAccelerationStructureBuildRangeInfoKHR * const* ppBuildRangeInfos)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , VkDeferredOperationKHR  , uint32_t  ,const VkAccelerationStructureBuildGeometryInfoKHR * ,const VkAccelerationStructureBuildRangeInfoKHR * const* ))
        _android_vulkan_dlsym("vkBuildAccelerationStructuresKHR"))
            (device, deferredOperation, infoCount, pInfos, ppBuildRangeInfos);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCopyAccelerationStructureKHR( VkDevice  device,  VkDeferredOperationKHR  deferredOperation, const VkCopyAccelerationStructureInfoKHR * pInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , VkDeferredOperationKHR  ,const VkCopyAccelerationStructureInfoKHR * ))
        _android_vulkan_dlsym("vkCopyAccelerationStructureKHR"))
            (device, deferredOperation, pInfo);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCopyAccelerationStructureToMemoryKHR( VkDevice  device,  VkDeferredOperationKHR  deferredOperation, const VkCopyAccelerationStructureToMemoryInfoKHR * pInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , VkDeferredOperationKHR  ,const VkCopyAccelerationStructureToMemoryInfoKHR * ))
        _android_vulkan_dlsym("vkCopyAccelerationStructureToMemoryKHR"))
            (device, deferredOperation, pInfo);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCopyMemoryToAccelerationStructureKHR( VkDevice  device,  VkDeferredOperationKHR  deferredOperation, const VkCopyMemoryToAccelerationStructureInfoKHR * pInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , VkDeferredOperationKHR  ,const VkCopyMemoryToAccelerationStructureInfoKHR * ))
        _android_vulkan_dlsym("vkCopyMemoryToAccelerationStructureKHR"))
            (device, deferredOperation, pInfo);
}

VKAPI_ATTR VkResult VKAPI_CALL vkWriteAccelerationStructuresPropertiesKHR( VkDevice  device,  uint32_t  accelerationStructureCount, const VkAccelerationStructureKHR * pAccelerationStructures,  VkQueryType  queryType,  size_t  dataSize,  void * pData,  size_t  stride)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , uint32_t  ,const VkAccelerationStructureKHR * , VkQueryType  , size_t  , void * , size_t  ))
        _android_vulkan_dlsym("vkWriteAccelerationStructuresPropertiesKHR"))
            (device, accelerationStructureCount, pAccelerationStructures, queryType, dataSize, pData, stride);
}

VKAPI_ATTR void VKAPI_CALL vkCmdCopyAccelerationStructureKHR( VkCommandBuffer  commandBuffer, const VkCopyAccelerationStructureInfoKHR * pInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  ,const VkCopyAccelerationStructureInfoKHR * ))
        _android_vulkan_dlsym("vkCmdCopyAccelerationStructureKHR"))
            (commandBuffer, pInfo);
}

VKAPI_ATTR void VKAPI_CALL vkCmdCopyAccelerationStructureToMemoryKHR( VkCommandBuffer  commandBuffer, const VkCopyAccelerationStructureToMemoryInfoKHR * pInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  ,const VkCopyAccelerationStructureToMemoryInfoKHR * ))
        _android_vulkan_dlsym("vkCmdCopyAccelerationStructureToMemoryKHR"))
            (commandBuffer, pInfo);
}

VKAPI_ATTR void VKAPI_CALL vkCmdCopyMemoryToAccelerationStructureKHR( VkCommandBuffer  commandBuffer, const VkCopyMemoryToAccelerationStructureInfoKHR * pInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  ,const VkCopyMemoryToAccelerationStructureInfoKHR * ))
        _android_vulkan_dlsym("vkCmdCopyMemoryToAccelerationStructureKHR"))
            (commandBuffer, pInfo);
}

VKAPI_ATTR VkDeviceAddress VKAPI_CALL vkGetAccelerationStructureDeviceAddressKHR( VkDevice  device, const VkAccelerationStructureDeviceAddressInfoKHR * pInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkDeviceAddress (*)( VkDevice  ,const VkAccelerationStructureDeviceAddressInfoKHR * ))
        _android_vulkan_dlsym("vkGetAccelerationStructureDeviceAddressKHR"))
            (device, pInfo);
}

VKAPI_ATTR void VKAPI_CALL vkCmdWriteAccelerationStructuresPropertiesKHR( VkCommandBuffer  commandBuffer,  uint32_t  accelerationStructureCount, const VkAccelerationStructureKHR * pAccelerationStructures,  VkQueryType  queryType,  VkQueryPool  queryPool,  uint32_t  firstQuery)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , uint32_t  ,const VkAccelerationStructureKHR * , VkQueryType  , VkQueryPool  , uint32_t  ))
        _android_vulkan_dlsym("vkCmdWriteAccelerationStructuresPropertiesKHR"))
            (commandBuffer, accelerationStructureCount, pAccelerationStructures, queryType, queryPool, firstQuery);
}

VKAPI_ATTR void VKAPI_CALL vkGetDeviceAccelerationStructureCompatibilityKHR( VkDevice  device, const VkAccelerationStructureVersionInfoKHR * pVersionInfo,  VkAccelerationStructureCompatibilityKHR * pCompatibility)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  ,const VkAccelerationStructureVersionInfoKHR * , VkAccelerationStructureCompatibilityKHR * ))
        _android_vulkan_dlsym("vkGetDeviceAccelerationStructureCompatibilityKHR"))
            (device, pVersionInfo, pCompatibility);
}

VKAPI_ATTR void VKAPI_CALL vkGetAccelerationStructureBuildSizesKHR( VkDevice  device,  VkAccelerationStructureBuildTypeKHR  buildType, const VkAccelerationStructureBuildGeometryInfoKHR * pBuildInfo, const uint32_t * pMaxPrimitiveCounts,  VkAccelerationStructureBuildSizesInfoKHR * pSizeInfo)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkDevice  , VkAccelerationStructureBuildTypeKHR  ,const VkAccelerationStructureBuildGeometryInfoKHR * ,const uint32_t * , VkAccelerationStructureBuildSizesInfoKHR * ))
        _android_vulkan_dlsym("vkGetAccelerationStructureBuildSizesKHR"))
            (device, buildType, pBuildInfo, pMaxPrimitiveCounts, pSizeInfo);
}

VKAPI_ATTR void VKAPI_CALL vkCmdTraceRaysKHR( VkCommandBuffer  commandBuffer, const VkStridedDeviceAddressRegionKHR * pRaygenShaderBindingTable, const VkStridedDeviceAddressRegionKHR * pMissShaderBindingTable, const VkStridedDeviceAddressRegionKHR * pHitShaderBindingTable, const VkStridedDeviceAddressRegionKHR * pCallableShaderBindingTable,  uint32_t  width,  uint32_t  height,  uint32_t  depth)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  ,const VkStridedDeviceAddressRegionKHR * ,const VkStridedDeviceAddressRegionKHR * ,const VkStridedDeviceAddressRegionKHR * ,const VkStridedDeviceAddressRegionKHR * , uint32_t  , uint32_t  , uint32_t  ))
        _android_vulkan_dlsym("vkCmdTraceRaysKHR"))
            (commandBuffer, pRaygenShaderBindingTable, pMissShaderBindingTable, pHitShaderBindingTable, pCallableShaderBindingTable, width, height, depth);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateRayTracingPipelinesKHR( VkDevice  device,  VkDeferredOperationKHR  deferredOperation,  VkPipelineCache  pipelineCache,  uint32_t  createInfoCount, const VkRayTracingPipelineCreateInfoKHR * pCreateInfos, const VkAllocationCallbacks * pAllocator,  VkPipeline * pPipelines)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , VkDeferredOperationKHR  , VkPipelineCache  , uint32_t  ,const VkRayTracingPipelineCreateInfoKHR * ,const VkAllocationCallbacks * , VkPipeline * ))
        _android_vulkan_dlsym("vkCreateRayTracingPipelinesKHR"))
            (device, deferredOperation, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetRayTracingCaptureReplayShaderGroupHandlesKHR( VkDevice  device,  VkPipeline  pipeline,  uint32_t  firstGroup,  uint32_t  groupCount,  size_t  dataSize,  void * pData)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkResult (*)( VkDevice  , VkPipeline  , uint32_t  , uint32_t  , size_t  , void * ))
        _android_vulkan_dlsym("vkGetRayTracingCaptureReplayShaderGroupHandlesKHR"))
            (device, pipeline, firstGroup, groupCount, dataSize, pData);
}

VKAPI_ATTR void VKAPI_CALL vkCmdTraceRaysIndirectKHR( VkCommandBuffer  commandBuffer, const VkStridedDeviceAddressRegionKHR * pRaygenShaderBindingTable, const VkStridedDeviceAddressRegionKHR * pMissShaderBindingTable, const VkStridedDeviceAddressRegionKHR * pHitShaderBindingTable, const VkStridedDeviceAddressRegionKHR * pCallableShaderBindingTable,  VkDeviceAddress  indirectDeviceAddress)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  ,const VkStridedDeviceAddressRegionKHR * ,const VkStridedDeviceAddressRegionKHR * ,const VkStridedDeviceAddressRegionKHR * ,const VkStridedDeviceAddressRegionKHR * , VkDeviceAddress  ))
        _android_vulkan_dlsym("vkCmdTraceRaysIndirectKHR"))
            (commandBuffer, pRaygenShaderBindingTable, pMissShaderBindingTable, pHitShaderBindingTable, pCallableShaderBindingTable, indirectDeviceAddress);
}

VKAPI_ATTR VkDeviceSize VKAPI_CALL vkGetRayTracingShaderGroupStackSizeKHR( VkDevice  device,  VkPipeline  pipeline,  uint32_t  group,  VkShaderGroupShaderKHR  groupShader)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    return ((VkDeviceSize (*)( VkDevice  , VkPipeline  , uint32_t  , VkShaderGroupShaderKHR  ))
        _android_vulkan_dlsym("vkGetRayTracingShaderGroupStackSizeKHR"))
            (device, pipeline, group, groupShader);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetRayTracingPipelineStackSizeKHR( VkCommandBuffer  commandBuffer,  uint32_t  pipelineStackSize)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , uint32_t  ))
        _android_vulkan_dlsym("vkCmdSetRayTracingPipelineStackSizeKHR"))
            (commandBuffer, pipelineStackSize);
}

#if VK_HEADER_VERSION >= 226

VKAPI_ATTR void VKAPI_CALL vkCmdDrawMeshTasksEXT( VkCommandBuffer  commandBuffer,  uint32_t  groupCountX,  uint32_t  groupCountY,  uint32_t  groupCountZ)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , uint32_t  , uint32_t  , uint32_t  ))
        _android_vulkan_dlsym("vkCmdDrawMeshTasksEXT"))
            (commandBuffer, groupCountX, groupCountY, groupCountZ);
}

VKAPI_ATTR void VKAPI_CALL vkCmdDrawMeshTasksIndirectEXT( VkCommandBuffer  commandBuffer,  VkBuffer  buffer,  VkDeviceSize  offset,  uint32_t  drawCount,  uint32_t  stride)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkBuffer  , VkDeviceSize  , uint32_t  , uint32_t  ))
        _android_vulkan_dlsym("vkCmdDrawMeshTasksIndirectEXT"))
            (commandBuffer, buffer, offset, drawCount, stride);
}

VKAPI_ATTR void VKAPI_CALL vkCmdDrawMeshTasksIndirectCountEXT( VkCommandBuffer  commandBuffer,  VkBuffer  buffer,  VkDeviceSize  offset,  VkBuffer  countBuffer,  VkDeviceSize  countBufferOffset,  uint32_t  maxDrawCount,  uint32_t  stride)
{
    if (!vulkan_hal_device) _init_androidvulkan();

    ((void (*)( VkCommandBuffer  , VkBuffer  , VkDeviceSize  , VkBuffer  , VkDeviceSize  , uint32_t  , uint32_t  ))
        _android_vulkan_dlsym("vkCmdDrawMeshTasksIndirectCountEXT"))
            (commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);
}

#endif

/* X11 stubs */

#ifdef WANT_VULKAN_X11_STUBS

typedef VkFlags VkXlibSurfaceCreateFlagsKHR;
typedef struct VkXlibSurfaceCreateInfoKHR {
    VkStructureType                sType;
    const void*                    pNext;
    VkXlibSurfaceCreateFlagsKHR    flags;
    Display*                       dpy;
    Window                         window;
} VkXlibSurfaceCreateInfoKHR;

VKAPI_ATTR VkResult VKAPI_CALL vkCreateXlibSurfaceKHR(
    VkInstance                                  instance,
    const VkXlibSurfaceCreateInfoKHR*           pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkSurfaceKHR*                               pSurface)
{
    return VK_ERROR_EXTENSION_NOT_PRESENT;
}

VKAPI_ATTR VkBool32 VKAPI_CALL vkGetPhysicalDeviceXlibPresentationSupportKHR(
    VkPhysicalDevice                            physicalDevice,
    uint32_t                                    queueFamilyIndex,
    Display*                                    dpy,
    VisualID                                    visualID)
{
    return VK_FALSE;
}

typedef VkFlags VkXcbSurfaceCreateFlagsKHR;
typedef struct VkXcbSurfaceCreateInfoKHR {
    VkStructureType               sType;
    const void*                   pNext;
    VkXcbSurfaceCreateFlagsKHR    flags;
    xcb_connection_t*             connection;
    xcb_window_t                  window;
} VkXcbSurfaceCreateInfoKHR;

VKAPI_ATTR VkResult VKAPI_CALL vkCreateXcbSurfaceKHR(
    VkInstance                                  instance,
    const VkXcbSurfaceCreateInfoKHR*            pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkSurfaceKHR*                               pSurface)
{
    return VK_ERROR_EXTENSION_NOT_PRESENT;
}

VKAPI_ATTR VkBool32 VKAPI_CALL vkGetPhysicalDeviceXcbPresentationSupportKHR(
    VkPhysicalDevice                            physicalDevice,
    uint32_t                                    queueFamilyIndex,
    xcb_connection_t*                           connection,
    xcb_visualid_t                              visual_id)
{
    return VK_FALSE;
}

VKAPI_ATTR VkResult VKAPI_CALL vkAcquireXlibDisplayEXT(
    VkPhysicalDevice                            physicalDevice,
    Display*                                    dpy,
    VkDisplayKHR                                display)
{
    return VK_ERROR_EXTENSION_NOT_PRESENT;
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetRandROutputDisplayEXT(
    VkPhysicalDevice                            physicalDevice,
    Display*                                    dpy,
    RROutput                                    rrOutput,
    VkDisplayKHR*                               pDisplay)
{
    return VK_ERROR_EXTENSION_NOT_PRESENT;
}

#endif

// vim:ts=4:sw=4:noexpandtab

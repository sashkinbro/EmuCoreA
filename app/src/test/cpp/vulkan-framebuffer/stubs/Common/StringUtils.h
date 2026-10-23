#pragma once
#include "Common/GPU/Vulkan/VulkanContext.h"
#include <cstdarg>
#include <cstdio>
#include <string>
inline std::string StringFromFormat(const char *format, ...) {
    char buffer[256]; va_list args; va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args); va_end(args); return buffer;
}
inline const char *VulkanResultToString(VkResult) { return "injected failure"; }

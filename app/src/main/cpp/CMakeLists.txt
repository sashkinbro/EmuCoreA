# SPDX-FileCopyrightText: 2026 SBRO
# SPDX-License-Identifier: LicenseRef-EmuCoreA-Proprietary
#
# Discord Social SDK bridge (libemucorea_discord.so), loaded only inside the
# isolated :discord process by DiscordNative. app/build.gradle.kts wires this
# project in only when the Discord Social SDK directory is configured.
cmake_minimum_required(VERSION 3.22)
project(EmuCoreADiscord LANGUAGES CXX)

# Google Play requires 16 KiB page size support.
add_link_options("-Wl,-z,max-page-size=16384" "-Wl,-z,common-page-size=16384")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT DISCORD_SDK_DIR)
    message(FATAL_ERROR "DISCORD_SDK_DIR is required to build emucorea_discord")
endif()

set(DISCORD_SDK_SO "${DISCORD_SDK_DIR}/${ANDROID_ABI}/libdiscord_partner_sdk.so")
set(DISCORD_SDK_INCLUDE "${DISCORD_SDK_DIR}/include")
if(NOT EXISTS "${DISCORD_SDK_SO}" OR NOT EXISTS "${DISCORD_SDK_INCLUDE}/discordpp.h")
    message(FATAL_ERROR "Discord Social SDK not found in ${DISCORD_SDK_DIR}")
endif()

add_library(discord_partner_sdk SHARED IMPORTED)
set_target_properties(discord_partner_sdk PROPERTIES IMPORTED_LOCATION "${DISCORD_SDK_SO}")

add_library(emucorea_discord SHARED discord_bridge.cpp)
target_include_directories(emucorea_discord PRIVATE "${DISCORD_SDK_INCLUDE}")
target_link_libraries(emucorea_discord PRIVATE discord_partner_sdk log)

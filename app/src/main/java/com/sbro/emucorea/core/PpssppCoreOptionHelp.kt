// SPDX-FileCopyrightText: 2026 SBRO
// SPDX-License-Identifier: LicenseRef-EmuCoreA-Proprietary
package com.sbro.emucorea.core

import androidx.annotation.StringRes
import com.sbro.emucorea.R

/**
 * Help hints for the built-in PPSSPP core options. The hint text itself lives
 * in the strings.xml resources of every supported locale like every other
 * setting hint; this table only maps the stable option key to its resource.
 */
@StringRes
fun ppssppCoreOptionHelpRes(key: String): Int? = when {
    key.startsWith("ppsspp_change_mac_address") -> R.string.settings_help_core_mac_address
    key.startsWith("ppsspp_pro_ad_hoc_server_address") -> R.string.settings_help_core_pro_ad_hoc_server_address
    key == "ppsspp_frameskip" -> R.string.settings_help_core_frameskip
    key == "ppsspp_auto_frameskip" -> R.string.settings_help_core_auto_frameskip
    key == "ppsspp_gpu_hardware_transform" -> R.string.settings_help_core_gpu_hardware_transform
    key == "ppsspp_texture_scaling_type" -> R.string.settings_help_core_texture_scaling_type
    key == "ppsspp_texture_anisotropic_filtering" -> R.string.settings_help_core_texture_anisotropic_filtering
    key == "ppsspp_texture_filtering" -> R.string.settings_help_core_texture_filtering
    key == "ppsspp_texture_replacement" -> R.string.settings_help_core_texture_replacement
    key == "ppsspp_cpu_core" -> R.string.settings_help_core_cpu_core
    key == "ppsspp_ignore_bad_memory_access" -> R.string.settings_help_core_ignore_bad_memory_access
    key == "ppsspp_io_timing_method" -> R.string.settings_help_core_io_timing_method
    key == "ppsspp_locked_cpu_speed" -> R.string.settings_help_core_locked_cpu_speed
    key == "ppsspp_memstick_size" -> R.string.settings_help_core_memstick_size
    key == "ppsspp_cache_iso" -> R.string.settings_help_core_cache_iso
    key == "ppsspp_psp_model" -> R.string.settings_help_core_psp_model
    key == "ppsspp_button_preference" -> R.string.settings_help_core_button_preference
    key == "ppsspp_analog_is_circular" -> R.string.settings_help_core_analog_is_circular
    key == "ppsspp_enable_wlan" -> R.string.settings_help_core_enable_wlan
    key == "ppsspp_wlan_channel" -> R.string.settings_help_core_wlan_channel
    key == "ppsspp_enable_builtin_pro_ad_hoc_server" -> R.string.settings_help_core_enable_builtin_pro_ad_hoc_server
    key == "ppsspp_change_pro_ad_hoc_server_address" -> R.string.settings_help_core_change_pro_ad_hoc_server_address
    key == "ppsspp_enable_upnp" -> R.string.settings_help_core_enable_upnp
    key == "ppsspp_upnp_use_original_port" -> R.string.settings_help_core_upnp_use_original_port
    key == "ppsspp_port_offset" -> R.string.settings_help_core_port_offset
    key == "ppsspp_minimum_timeout" -> R.string.settings_help_core_minimum_timeout
    key == "ppsspp_forced_first_connect" -> R.string.settings_help_core_forced_first_connect
    key == "ppsspp_fast_memory" -> R.string.settings_help_core_fast_memory
    key == "ppsspp_force_lag_sync" -> R.string.settings_help_core_force_lag_sync
    key == "ppsspp_memstick_inserted" -> R.string.settings_help_core_memstick_inserted
    key == "ppsspp_language" -> R.string.settings_help_core_language
    key == "ppsspp_backend" -> R.string.settings_help_core_backend
    key == "ppsspp_software_rendering" -> R.string.settings_help_core_software_rendering
    key == "ppsspp_internal_resolution" -> R.string.settings_help_core_internal_resolution
    key == "ppsspp_mulitsample_level" -> R.string.settings_help_core_mulitsample_level
    key == "ppsspp_cropto16x9" -> R.string.settings_help_core_cropto16x9
    key == "ppsspp_frame_duplication" -> R.string.settings_help_core_frame_duplication
    key == "ppsspp_detect_vsync_swap_interval" -> R.string.settings_help_core_detect_vsync_swap_interval
    key == "ppsspp_inflight_frames" -> R.string.settings_help_core_inflight_frames
    key == "ppsspp_analog_deadzone" -> R.string.settings_help_core_analog_deadzone
    key == "ppsspp_analog_sensitivity" -> R.string.settings_help_core_analog_sensitivity
    key == "ppsspp_skip_buffer_effects" -> R.string.settings_help_core_skip_buffer_effects
    key == "ppsspp_skip_gpu_readbacks" -> R.string.settings_help_core_skip_gpu_readbacks
    key == "ppsspp_spline_quality" -> R.string.settings_help_core_spline_quality
    key == "ppsspp_lower_resolution_for_effects" -> R.string.settings_help_core_lower_resolution_for_effects
    key == "ppsspp_texture_scaling_level" -> R.string.settings_help_core_texture_scaling_level
    key == "ppsspp_texture_deposterize" -> R.string.settings_help_core_texture_deposterize
    key == "ppsspp_texture_shader" -> R.string.settings_help_core_texture_shader
    key == "ppsspp_smart_2d_texture_filtering" -> R.string.settings_help_core_smart_2d_texture_filtering
    else -> null
}

#include "xmb_renderer.h"

#include <android/log.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>

#include "png_image.h"

namespace xmb {
namespace {

constexpr const char *kTag = "EmuCoreXmb";
constexpr float kScreenWidth = 480.0f;
constexpr float kScreenHeight = 272.0f;

// Measured from the 6.60 shell. The category anchors are absolute x positions
// with Game selected; the row moves by the difference between anchors.
constexpr float kCategoryY = 71.0f;
constexpr float kCategoryAnchorX = 109.5f;
constexpr float kCategoryX[8] = {-318.0f, -232.5f, -147.0f, -61.5f, 24.0f, 109.5f, 195.5f, 274.0f};
const char *kCategoryKeys[8] = {
	"msgshare_settings", "msgtop_extras", "msgtop_photo", "msgtop_music",
	"msgtop_video", "msgshare_game", "msgtop_network", "msg_psn",
};
const char *kCategoryFallbacks[8] = {
	"Settings", "Extras", "Photo", "Music", "Video", "Game", "Network", "PlayStation\u00aeNetwork",
};
constexpr float kItemIconX = 110.0f;
constexpr float kSelectedItemY = 136.0f;
constexpr float kItemSpacing = 66.0f;
constexpr float kCategoryTau = 0.045f;
constexpr float kItemTau = 0.060f;

float Lerp(float a, float b, float t) {
	return a + (b - a) * t;
}

float AdvanceTowardsOne(float value, float dt, float tau) {
	if (value >= 1.0f) {
		return 1.0f;
	}
	value = 1.0f - (1.0f - value) * std::exp(-dt / tau);
	return value > 0.995f ? 1.0f : value;
}

const char *kVertexShader = R"(#version 300 es
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUv;
uniform vec2 uScreen;
out vec2 vUv;
void main() {
	vUv = aUv;
	vec2 p = vec2(aPos.x / uScreen.x * 2.0 - 1.0, 1.0 - aPos.y / uScreen.y * 2.0);
	gl_Position = vec4(p, 0.0, 1.0);
}
)";

const char *kFragmentShader = R"(#version 300 es
precision mediump float;
in vec2 vUv;
uniform sampler2D uTexture;
uniform vec4 uColor;
out vec4 fragColor;
void main() {
	fragColor = texture(uTexture, vUv) * uColor;
}
)";

const char *kWaveVertexShader = R"(#version 300 es
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
uniform mat4 uMvp;
out vec3 vNormal;
void main() {
	vNormal = aNormal;
	gl_Position = uMvp * vec4(aPos, 1.0);
}
)";

const char *kWaveFragmentShader = R"(#version 300 es
precision mediump float;
in vec3 vNormal;
uniform sampler2D uTexture;
uniform vec4 uColor;
out vec4 fragColor;
void main() {
	vec3 n = normalize(vNormal);
	vec2 uv = vec2(n.x, -n.y) * 0.5 + 0.5;
	vec3 tex = texture(uTexture, uv).rgb;
	fragColor = vec4(tex * uColor.rgb, 0.0);
}
)";

constexpr float kWaveYOffsetPixels = 0.0f;
constexpr float kWaveBrightness = 0.45f;
constexpr float kWaveNdcScaleY = 0.68f;
constexpr float kWaveNdcOffsetY = -0.40f;

// Row medians measured from the 6.60 default theme.
const uint8_t kBackgroundRows[17][3] = {
	{107, 46, 27}, {99, 39, 23}, {96, 37, 22}, {90, 33, 20}, {86, 30, 18},
	{81, 27, 16}, {77, 24, 14}, {74, 21, 12}, {96, 29, 12}, {144, 49, 12},
	{153, 52, 12}, {169, 58, 12}, {182, 62, 12}, {185, 63, 12}, {183, 62, 11},
	{182, 62, 10}, {182, 62, 10},
};

// 12 authentic PSP monthly theme swatches (sampled from 01-12_03g.bmp)
const float kMonthGradients[12][3] = {
	{0.729f, 0.745f, 0.800f}, // Jan (Silver)
	{0.922f, 0.718f, 0.078f}, // Feb (Gold)
	{0.494f, 0.757f, 0.188f}, // Mar (Spring Green)
	{0.973f, 0.592f, 0.698f}, // Apr (Sakura Pink)
	{0.071f, 0.467f, 0.106f}, // May (Emerald)
	{0.600f, 0.439f, 0.855f}, // Jun (Violet)
	{0.039f, 0.671f, 0.788f}, // Jul (Turquoise)
	{0.039f, 0.408f, 0.733f}, // Aug (Ocean Blue)
	{0.420f, 0.063f, 0.569f}, // Sep (Royal Purple)
	{1.000f, 0.616f, 0.180f}, // Oct (Amber)
	{0.725f, 0.247f, 0.047f}, // Nov (PSP 6.60 Orange-Red)
	{0.812f, 0.094f, 0.027f}, // Dec (Crimson)
};

// 22 authentic PSP-2000/3000 Slim extended theme swatches (sampled from 13-27.bmp)
const float kExtColorSwatches[22][3] = {
	{0.996f, 0.514f, 0.647f}, // Rose Pink
	{1.000f, 0.243f, 0.529f}, // Hot Pink
	{0.922f, 0.000f, 0.275f}, // Crimson
	{0.835f, 0.200f, 0.655f}, // Magenta
	{0.667f, 0.004f, 0.494f}, // Deep Magenta
	{0.525f, 0.000f, 0.314f}, // Wine Red
	{0.620f, 0.600f, 0.824f}, // Lavender
	{0.412f, 0.424f, 0.737f}, // Periwinkle
	{0.318f, 0.306f, 0.667f}, // Slate Blue
	{0.000f, 0.886f, 0.427f}, // Mint Green
	{0.008f, 0.780f, 0.365f}, // Emerald
	{0.000f, 0.616f, 0.247f}, // Forest Green
	{0.278f, 0.267f, 0.420f}, // Steel Gray
	{0.196f, 0.176f, 0.282f}, // Charcoal
	{0.059f, 0.039f, 0.133f}, // Midnight Blue
	{0.063f, 0.063f, 0.118f}, // Piano Black
	{0.753f, 0.702f, 0.839f}, // Pearl Lilac
	{0.518f, 0.533f, 0.565f}, // Silver
	{0.376f, 0.102f, 0.824f}, // Electric Violet
	{0.827f, 0.169f, 0.478f}, // Berry
	{0.863f, 0.694f, 0.106f}, // Gold
	{0.604f, 0.498f, 0.322f}, // Bronze
};

const char *kExtColorKeys[22] = {
	"xmb_color_ext_00", "xmb_color_ext_01", "xmb_color_ext_02", "xmb_color_ext_03",
	"xmb_color_ext_04", "xmb_color_ext_05", "xmb_color_ext_06", "xmb_color_ext_07",
	"xmb_color_ext_08", "xmb_color_ext_09", "xmb_color_ext_10", "xmb_color_ext_11",
	"xmb_color_ext_12", "xmb_color_ext_13", "xmb_color_ext_14", "xmb_color_ext_15",
	"xmb_color_ext_16", "xmb_color_ext_17", "xmb_color_ext_18", "xmb_color_ext_19",
	"xmb_color_ext_20", "xmb_color_ext_21",
};

const char *kExtColorFallbacks[22] = {
	"Rose Pink", "Hot Pink", "Crimson", "Magenta", "Deep Magenta",
	"Wine Red", "Lavender", "Periwinkle", "Slate Blue", "Mint Green",
	"Emerald", "Forest Green", "Steel Gray", "Charcoal", "Midnight Blue",
	"Piano Black", "Pearl Lilac", "Silver", "Electric Violet", "Berry",
	"Gold", "Bronze",
};

const float kClassicBlueTint[3] = {0.12f, 0.42f, 0.88f};

const char *kMonthKeys[12] = {
	"xmb_month_jan", "xmb_month_feb", "xmb_month_mar", "xmb_month_apr",
	"xmb_month_may", "xmb_month_jun", "xmb_month_jul", "xmb_month_aug",
	"xmb_month_sep", "xmb_month_oct", "xmb_month_nov", "xmb_month_dec",
};
const char *kMonthFallbacks[12] = {
	"January", "February", "March", "April", "May", "June",
	"July", "August", "September", "October", "November", "December",
};

void MultiplyMatrix(const float a[16], const float b[16], float out[16]) {
	for (int column = 0; column < 4; ++column) {
		for (int row = 0; row < 4; ++row) {
			float value = 0.0f;
			for (int k = 0; k < 4; ++k) {
				value += a[k * 4 + row] * b[column * 4 + k];
			}
			out[column * 4 + row] = value;
		}
	}
}

void BuildWaveMvp(float out[16], float screen_width) {
	constexpr float kFovY = 30.0f;
	constexpr float kNearFactor = 0.1f;
	constexpr float kFarFactor = 4.0f;
	const float aspect = screen_width / kScreenHeight;
	const float distance = (kScreenHeight * 0.5f) / std::tan(kFovY * 0.5f * 3.14159265358979323846f / 180.0f);
	const float f = 1.0f / std::tan(kFovY * 0.5f * 3.14159265358979323846f / 180.0f);
	const float near_plane = distance * kNearFactor;
	const float far_plane = distance * kFarFactor;
	float projection[16] = {0.0f};
	projection[0] = f / aspect;
	projection[5] = f;
	projection[10] = (far_plane + near_plane) / (near_plane - far_plane);
	projection[11] = -1.0f;
	projection[14] = (2.0f * far_plane * near_plane) / (near_plane - far_plane);
	float view[16] = {1.0f, 0.0f, 0.0f, 0.0f,
	                  0.0f, 1.0f, 0.0f, 0.0f,
	                  0.0f, 0.0f, 1.0f, 0.0f,
	                  0.0f, 0.0f, -distance, 1.0f};
	MultiplyMatrix(projection, view, out);
	for (int column = 0; column < 4; ++column) {
		out[column * 4 + 1] *= kWaveNdcScaleY;
	}
	out[13] = out[13] * kWaveNdcScaleY + kWaveNdcOffsetY;
	out[13] -= 2.0f * kWaveYOffsetPixels / kScreenHeight;
}

GLuint CompileShader(GLenum type, const char *source) {
	GLuint shader = glCreateShader(type);
	glShaderSource(shader, 1, &source, nullptr);
	glCompileShader(shader);
	GLint compiled = 0;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
	if (!compiled) {
		GLint length = 0;
		glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
		std::string log((size_t)length + 1, '\0');
		glGetShaderInfoLog(shader, length, nullptr, log.data());
		__android_log_print(ANDROID_LOG_ERROR, kTag, "shader compile: %s", log.c_str());
		glDeleteShader(shader);
		return 0;
	}
	return shader;
}

}  // namespace

int XmbRenderer::EffectiveColorIndex() const {
	if (settings_.color_index >= 1 && settings_.color_index <= 12) {
		return settings_.color_index - 1;
	}
	const std::time_t now = std::time(nullptr);
	std::tm local{};
	localtime_r(&now, &local);
	return std::max(0, std::min(11, local.tm_mon));
}

bool XmbRenderer::IsConfirmKey(Key key) const {
	return key == Key::Confirm;
}

bool XmbRenderer::IsBackKey(Key key) const {
	return key == Key::Back;
}

bool XmbRenderer::Initialize(const std::string &resource_root) {
	std::lock_guard<std::recursive_mutex> lock(mutex_);
	// onSurfaceCreated may be called again when EGL context is recreated; drop all
	// stale GL handles so new allocations cannot alias old texture/program names.
	textures_.clear();
	image_textures_.clear();
	glyph_textures_.clear();
	shadow_glyph_textures_.clear();
	fallback_glyphs_.clear();
	pic1_texture_ = 0;
	pic0_texture_ = 0;
	pic1_loaded_path_.clear();
	pic0_loaded_path_.clear();
	white_texture_ = 0;
	background_texture_ = 0;
	wave_texture_ = 0;
	wave_program_ = 0;
	wave_vao_ = 0;
	wave_vbo_ = 0;
	wave_ebo_ = 0;
	program_ = 0;
	vertex_array_ = 0;
	vertex_buffer_ = 0;
	logged_gl_error_ = false;
	const bool first_init = !initialized_once_;

	root_ = resource_root;
	LoadTextCatalog();
	if (!BuildProgram()) {
		return false;
	}

	glGenVertexArrays(1, &vertex_array_);
	glGenBuffers(1, &vertex_buffer_);
	glBindVertexArray(vertex_array_);
	glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer_);
	glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 6 * 4, nullptr, GL_DYNAMIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 4, (const void *)0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 4, (const void *)(sizeof(float) * 2));
	glBindVertexArray(0);

	const uint32_t white_pixel = 0xFFFFFFFF;
	glGenTextures(1, &white_texture_);
	glBindTexture(GL_TEXTURE_2D, white_texture_);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &white_pixel);

	glDisable(GL_DEPTH_TEST);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	const struct {
		const char *name;
		const char *path;
	} assets[] = {
		{"settings", "images/topmenu_icon/AJ.png"},
		{"extras", "images/topmenu_icon/AK.png"},
		{"photo", "images/topmenu_icon/AL.png"},
		{"music", "images/topmenu_icon/AM.png"},
		{"video", "images/topmenu_icon/AN.png"},
		{"game", "images/topmenu_icon/AO.png"},
		{"network", "images/topmenu_icon/AP.png"},
		{"psn", "images/topmenu_icon/AQ.png"},
		// Sharp foreground XMB icons from topmenu_icon.rco
		{"item_update", "images/topmenu_icon/BT.png"},
		{"item_usb", "images/topmenu_icon/BU.png"},
		{"item_video_settings", "images/topmenu_icon/BV.png"},
		{"item_photo_settings", "images/topmenu_icon/BW.png"},
		{"item_system_settings", "images/topmenu_icon/BX.png"},
		{"item_theme_settings", "images/topmenu_icon/BY.png"},
		{"item_date_settings", "images/topmenu_icon/BZ.png"},
		{"item_power_settings", "images/topmenu_icon/CA.png"},
		{"item_sound_settings", "images/topmenu_icon/CB.png"},
		{"item_security_settings", "images/topmenu_icon/CC.png"},
		{"item_network_settings", "images/topmenu_icon/CD.png"},
		{"item_rss_settings", "images/topmenu_icon/CE.png"},
		{"item_display_settings", "images/topmenu_icon/CF.png"},
		{"item_bluetooth_settings", "images/topmenu_icon/CG.png"},
		{"item_comics", "images/topmenu_icon/DV.png"},
		{"item_1seg", "images/topmenu_icon/DS.png"},
		{"item_camera", "images/topmenu_icon/DJ.png"},
		{"item_sensme", "images/topmenu_icon/DN.png"},
		{"item_playlists", "images/topmenu_icon/DM.png"},
		{"item_game_sharing", "images/topmenu_icon/EI.png"},
		{"item_savedata", "images/topmenu_icon/EJ.png"},
		{"item_memstick", "images/topmenu_icon/BI.png"},
		{"item_manuals", "images/topmenu_icon/EX.png"},
		{"item_locationfree", "images/topmenu_icon/ET.png"},
		{"item_remoteplay", "images/topmenu_icon/EW.png"},
		{"item_rss", "images/topmenu_icon/EV.png"},
		{"item_internet_browser", "images/topmenu_icon/EU.png"},
		{"item_internet_search", "images/topmenu_icon/FB.png"},
		{"item_internet_radio", "images/topmenu_icon/FA.png"},
		{"item_skype", "images/topmenu_icon/EZ.png"},
		{"item_psn_signup", "images/topmenu_icon/FW.png"},
		{"item_psn_store", "images/topmenu_icon/FV.png"},
		{"item_info_board", "images/topmenu_icon/FX.png"},
		{"battery", "images/system_plugin_fg/tex_battery.png"},
		{"battery_shadow", "images/system_plugin_fg/tex_battery_shadow.png"},
		{"status_bt", "images/impose_plugin/tex_bt_mark.png"},
		{"status_wifi", "images/topmenu_icon/EV.png"},
		{"status_net", "images/netconf_dialog/tex_netconf_icon.png"},
		{"status_avls", "images/impose_plugin/tex_avls.png"},
		{"status_mute", "images/system_plugin_fg/tex_mute.png"},
		{"status_mute_shadow", "images/system_plugin_fg/tex_mute_shadow.png"},
		{"status_ms", "images/sysconf_plugin/tex_ms.png"},
		{"status_busy", "images/system_plugin_fg/tex_busy.png"},
		{"status_busy_shadow", "images/system_plugin_fg/tex_busy_shadow.png"},
		{"open_psp_logo", "images/opening_plugin/tex_psp_logo.png"},
		{"open_sce_logo", "images/opening_plugin/tex_logo.png"},
		{"open_glow", "images/opening_plugin/tex_glow.png"},
		{"open_glow_a", "images/opening_plugin/tex_glow_A.png"},
		{"open_glow_b_top", "images/opening_plugin/tex_glow_B_top.png"},
		{"open_glow_b_mid", "images/opening_plugin/tex_glow_B_mid.png"},
		{"open_glow_b_bot", "images/opening_plugin/tex_glow_B_buttom.png"},
		{"preview_orig", "images/sysconf_plugin/tex_original_preview.png"},
		{"preview_classic", "images/sysconf_plugin/tex_classic_preview.png"},
		{"gameboot_bg", "images/themes/ext_10.png"},
		{"focus_glow", "images/system_plugin/tex_3x3_focus.png"},
		{"item_game", "images/system_plugin/tex_default_game_icon.png"},
		{"frame_subs", "images/game_plugin/tex_subs_frame.png"},
		{"arrow_l", "images/game_plugin/tex_arrow_l.png"},
		{"arrow_l_shadow", "images/game_plugin/tex_arrow_l_shadow.png"},
		{"arrow_r", "images/game_plugin/tex_arrow_r.png"},
		{"arrow_r_shadow", "images/game_plugin/tex_arrow_r_shadow.png"},
		{"btn_cross", "images/system_plugin_fg/tex_cross.png"},
		{"btn_circle", "images/system_plugin_fg/tex_circle.png"},
		{"btn_triangle", "images/system_plugin_fg/tex_triangle.png"},
		{"btn_square", "images/system_plugin_fg/tex_square.png"},
		{"item_loading", "images/savedata_plugin/icon_loading.png"},
		{"cookies_bg", "images/themes/cookies_bg.png"},
		{"cookies_cat_0", "images/themes/cookies_cat_0.png"},
		{"cookies_cat_1", "images/themes/cookies_cat_1.png"},
		{"cookies_cat_2", "images/themes/cookies_cat_2.png"},
		{"cookies_cat_3", "images/themes/cookies_cat_3.png"},
		{"cookies_cat_4", "images/themes/cookies_cat_4.png"},
		{"cookies_cat_5", "images/themes/cookies_cat_5.png"},
		{"cookies_cat_6", "images/themes/cookies_cat_6.png"},
		{"cookies_cat_7", "images/themes/cookies_cat_7.png"},
	};
	for (const auto &asset : assets) {
		LoadTexture(asset.name, asset.path);
	}

	RebuildCategories();

	if (first_init) {
		category_selected_ = {0, 0, 0, 0, 0, 2, 0, 0}; // Game -> Memory Stick (index 2)
		items_ = categories_[5];
		item_ = 2;
		item_from_ = 2;
		item_t_ = 1.0f;
		category_ = 5;
		category_from_ = 5;
		category_t_ = 1.0f;
		opening_t_ = 0.0f;
	} else {
		items_ = categories_[category_];
		item_ = items_.empty() ? 0 : std::max(0, std::min((int)items_.size() - 1, item_));
		item_from_ = item_;
		item_t_ = 1.0f;
		category_from_ = category_;
		category_t_ = 1.0f;
		previous_items_.clear();
		items_fade_t_ = 1.0f;
	}
	initialized_once_ = true;
	launching_ = false;
	launch_dispatched_ = false;
	launch_t_ = 0.0f;
	boot_t_ = 0.0f;
	font_loaded_ = font_.Load(root_ + "/fonts/ltn0.pgf");
	if (font_loaded_) {
		__android_log_print(ANDROID_LOG_INFO, kTag, "font loaded: %s", font_.name().c_str());
	} else {
		__android_log_print(ANDROID_LOG_WARN, kTag, "fonts/ltn0.pgf could not be loaded");
	}
	wave_loaded_ = LoadWave();
	UpdateThemeColor();
	theme_dirty_ = false;
	__android_log_print(ANDROID_LOG_INFO, kTag, "scene initialized with %zu textures", textures_.size());
	return true;
}

void XmbRenderer::RebuildCategories() {
	const std::string ms = TextOr("msgshare_ms", "Memory Stick\u2122");
	const std::string free_sub = TextOr("msgshare_info_space", "Free Space") + " " + free_space_text_;

	// 0: Settings (authentic PSP 6.60 order)
	categories_[0] = {
		{ TextOr("msgtop_sysconf_update", "System Update"), "", "item_update" },
		{ TextOr("msgtop_sysconf_usb", "USB Connection"), "", "item_usb" },
		{ TextOr("msgtop_sysconf_video", "Video Settings"), "", "item_video_settings" },
		{ TextOr("msgtop_sysconf_photo", "Photo Settings"), "", "item_photo_settings" },
		{ TextOr("msgtop_sysconf_console", "System Settings"), "", "item_system_settings" },
		{ TextOr("msgtop_sysconf_theme", "Theme Settings"), "", "item_theme_settings" },
		{ TextOr("msgtop_sysconf_date", "Date & Time Settings"), "", "item_date_settings" },
		{ TextOr("msgtop_sysconf_powersave", "Power Save Settings"), "", "item_power_settings" },
		{ TextOr("msgtop_sysconf_sound", "Sound Settings"), "", "item_sound_settings" },
		{ TextOr("msgtop_sysconf_security", "Security Settings"), "", "item_security_settings" },
		{ TextOr("msgtop_sysconf_network", "Network Settings"), "", "item_network_settings" },
		{ TextOr("msgtop_sysconf_rss", "RSS Channel Settings"), "", "item_rss_settings" },
		{ TextOr("msg_display_setting", "Connected Display Settings"), "", "item_display_settings" },
		{ TextOr("xmb_setting_bt_devices", "Bluetooth\u00ae Device Settings"), "", "item_bluetooth_settings" },
	};
	// 1: Extras
	categories_[1] = {
		{ TextOr("msg_digitalcomics", "Digital Comics"), "", "item_comics" },
		{ TextOr("msg_1seg", "1Seg"), "", "item_1seg" },
	};
	// 2: Photo
	categories_[2] = {
		{ TextOr("msgtop_camera", "Camera"), "", "item_camera" },
		{ ms, free_sub, "item_memstick" },
	};
	// 3: Music
	categories_[3] = {
		{ TextOr("msg_sensme_channels", "SensMe\u2122 channels"), "", "item_sensme" },
		{ ms, free_sub, "item_memstick" },
	};
	// 4: Video
	categories_[4] = {
		{ ms, free_sub, "item_memstick" },
	};
	// 5: Game
	categories_[5] = {
		{ TextOr("msgtop_game_gamedl", "Game Sharing"), "", "item_game_sharing" },
		{ TextOr("msgtop_game_savedata", "Saved Data Utility"), "", "item_savedata" },
		{ ms, free_sub, "item_memstick" },
	};
	// 6: Network
	categories_[6] = {
		{ TextOr("xmb_manuals", "Online Instruction Manuals"), "", "item_manuals" },
		{ TextOr("msgtop_network_lftv", "LocationFree\u2122 Player"), "", "item_locationfree" },
		{ TextOr("xmb_remote_play", "Remote Play"), "", "item_remoteplay" },
		{ TextOr("msgtop_network_rss", "RSS Channel"), "", "item_rss" },
		{ TextOr("msgtop_network_browser", "Internet Browser"), "", "item_internet_browser" },
		{ TextOr("msg_internet_search", "Internet Search"), "", "item_internet_search" },
		{ TextOr("xmb_internet_radio", "Internet Radio"), "", "item_internet_radio" },
		{ TextOr("xmb_skype", "Skype\u2122"), "", "item_skype" },
	};
	// 7: PlayStation Network
	categories_[7] = {
		{ TextOr("msg_signup", "Sign Up for PlayStation\u00aeNetwork"), "", "item_psn_signup" },
		{ TextOr("msg_ps_store", "PlayStation\u00aeStore"), "", "item_psn_store" },
		{ TextOr("msg_information_board", "Information Board"), "", "item_info_board" },
	};
	if (category_ >= 0 && category_ < 8) {
		items_ = categories_[category_];
		if (items_.empty()) {
			item_ = 0;
			item_from_ = 0;
		} else if (item_ >= (int)items_.size()) {
			item_ = (int)items_.size() - 1;
			item_from_ = item_;
		}
	}
}

bool XmbRenderer::BuildProgram() {
	GLuint vertex = CompileShader(GL_VERTEX_SHADER, kVertexShader);
	GLuint fragment = CompileShader(GL_FRAGMENT_SHADER, kFragmentShader);
	if (!vertex || !fragment) {
		return false;
	}
	program_ = glCreateProgram();
	glAttachShader(program_, vertex);
	glAttachShader(program_, fragment);
	glLinkProgram(program_);
	glDeleteShader(vertex);
	glDeleteShader(fragment);
	GLint linked = 0;
	glGetProgramiv(program_, GL_LINK_STATUS, &linked);
	if (!linked) {
		GLint length = 0;
		glGetProgramiv(program_, GL_INFO_LOG_LENGTH, &length);
		std::string log((size_t)length + 1, '\0');
		glGetProgramInfoLog(program_, length, nullptr, log.data());
		__android_log_print(ANDROID_LOG_ERROR, kTag, "link: %s", log.c_str());
		glDeleteProgram(program_);
		program_ = 0;
		return false;
	}
	u_screen_ = glGetUniformLocation(program_, "uScreen");
	u_color_ = glGetUniformLocation(program_, "uColor");
	u_texture_ = glGetUniformLocation(program_, "uTexture");
	return true;
}

bool XmbRenderer::BuildWaveProgram() {
	GLuint vertex = CompileShader(GL_VERTEX_SHADER, kWaveVertexShader);
	GLuint fragment = CompileShader(GL_FRAGMENT_SHADER, kWaveFragmentShader);
	if (!vertex || !fragment) {
		return false;
	}
	wave_program_ = glCreateProgram();
	glAttachShader(wave_program_, vertex);
	glAttachShader(wave_program_, fragment);
	glLinkProgram(wave_program_);
	glDeleteShader(vertex);
	glDeleteShader(fragment);
	GLint linked = 0;
	glGetProgramiv(wave_program_, GL_LINK_STATUS, &linked);
	if (!linked) {
		GLint length = 0;
		glGetProgramiv(wave_program_, GL_INFO_LOG_LENGTH, &length);
		std::string log((size_t)length + 1, '\0');
		glGetProgramInfoLog(wave_program_, length, nullptr, log.data());
		__android_log_print(ANDROID_LOG_ERROR, kTag, "wave link: %s", log.c_str());
		glDeleteProgram(wave_program_);
		wave_program_ = 0;
		return false;
	}
	wave_mvp_ = glGetUniformLocation(wave_program_, "uMvp");
	wave_color_ = glGetUniformLocation(wave_program_, "uColor");
	wave_sampler_ = glGetUniformLocation(wave_program_, "uTexture");
	return true;
}

bool XmbRenderer::LoadTextCatalog() {
	strings_.clear();
	std::vector<uint8_t> bytes;
	if (!ReadFile(root_ + "/text/en.txt", &bytes)) {
		__android_log_print(ANDROID_LOG_WARN, kTag, "text/en.txt missing; using fallbacks");
	} else {
		std::string content(bytes.begin(), bytes.end());
		size_t line_start = 0;
		while (line_start < content.size()) {
			size_t line_end = content.find('\n', line_start);
			if (line_end == std::string::npos) {
				line_end = content.size();
			}
			std::string line = content.substr(line_start, line_end - line_start);
			if (!line.empty() && line.back() == '\r') {
				line.pop_back();
			}
			const size_t tab = line.find('\t');
			if (tab != std::string::npos) {
				std::string value = line.substr(tab + 1);
				std::string unescaped;
				for (size_t i = 0; i < value.size(); ++i) {
					if (value[i] == '\\' && i + 1 < value.size() && value[i + 1] == 'n') {
						unescaped.push_back('\n');
						++i;
					} else {
						unescaped.push_back(value[i]);
					}
				}
				strings_[line.substr(0, tab)] = std::move(unescaped);
			}
			line_start = line_end + 1;
		}
	}
	for (const auto &kv : string_overrides_) {
		strings_[kv.first] = kv.second;
	}
	__android_log_print(ANDROID_LOG_INFO, kTag, "firmware text catalog: %zu strings", strings_.size());
	return !strings_.empty();
}

void XmbRenderer::SetStrings(const std::unordered_map<std::string, std::string> &strings) {
	std::lock_guard<std::recursive_mutex> lock(mutex_);
	string_overrides_ = strings;
	for (const auto &kv : strings) {
		strings_[kv.first] = kv.second;
	}
	RebuildCategories();
}

void XmbRenderer::SetStorageInfo(const std::string &free_space_text, const std::string &device_name) {
	std::lock_guard<std::recursive_mutex> lock(mutex_);
	if (!free_space_text.empty()) {
		free_space_text_ = free_space_text;
	}
	if (!device_name.empty()) {
		device_name_ = device_name;
	}
	RebuildCategories();
}

void XmbRenderer::ApplySettings(const XmbSettings &settings) {
	std::lock_guard<std::recursive_mutex> lock(mutex_);
	settings_ = settings;
	settings_.theme_style = std::max(0, std::min(11, settings_.theme_style));
	settings_.color_index = std::max(0, std::min(34, settings_.color_index));
	settings_.bg_mode = std::max(0, std::min(2, settings_.bg_mode));
	settings_.wave_mode = std::max(0, std::min(3, settings_.wave_mode));
	settings_.date_format = std::max(0, std::min(2, settings_.date_format));
	settings_.keep_screen_mode = std::max(0, std::min(2, settings_.keep_screen_mode));
	settings_.aspect_mode = std::max(0, std::min(2, settings_.aspect_mode));
	settings_.slideshow_speed = std::max(0, std::min(2, settings_.slideshow_speed));
	settings_.slideshow_effect = std::max(0, std::min(2, settings_.slideshow_effect));
	settings_.parental_level = std::max(0, std::min(9, settings_.parental_level));
	settings_.adhoc_channel = std::max(0, std::min(3, settings_.adhoc_channel));
	theme_dirty_ = true;
}

void XmbRenderer::SetGamesLoading(bool loading) {
	std::lock_guard<std::recursive_mutex> lock(mutex_);
	games_loading_ = loading;
}

void XmbRenderer::SetSavesLoading(bool loading) {
	std::lock_guard<std::recursive_mutex> lock(mutex_);
	saves_loading_ = loading;
}

void XmbRenderer::SetNetworkStatus(bool connected) {
	std::lock_guard<std::recursive_mutex> lock(mutex_);
	network_connected_ = connected;
}

void XmbRenderer::SetWirelessStatus(bool wifi_connected, bool bluetooth_enabled, bool bluetooth_connected, bool audio_muted) {
	std::lock_guard<std::recursive_mutex> lock(mutex_);
	network_connected_ = wifi_connected;
	bluetooth_enabled_ = bluetooth_enabled;
	bluetooth_connected_ = bluetooth_connected;
	audio_muted_ = audio_muted;
}

void XmbRenderer::TriggerTransition(int kind) {
	std::lock_guard<std::recursive_mutex> lock(mutex_);
	launching_ = false;
	launch_dispatched_ = false;
	launch_t_ = 0.0f;
	if (kind == 0) {
		opening_t_ = 0.0f;
		resume_t_ = 1.0f;
	} else if (kind == 1) {
		resume_t_ = 0.0f;
		opening_t_ = 1.0f;
	}
}

void XmbRenderer::SetTouchMode(bool active, bool show_tutorial) {
	std::lock_guard<std::recursive_mutex> lock(mutex_);
	touch_mode_active_ = active;
	if (show_tutorial) {
		touch_tutorial_active_ = true;
	}
}

void XmbRenderer::TouchTap(float norm_x, float norm_y) {
	{
		std::lock_guard<std::recursive_mutex> lock(mutex_);
		if (!settings_.touch_controls_enabled || launching_) {
			return;
		}
		if (opening_t_ < 0.72f) {
			opening_t_ = 0.72f;
		}
		if (touch_tutorial_active_) {
			touch_tutorial_active_ = false;
			PlaySound(1);
			return;
		}

		const float vx = std::clamp(norm_x, 0.0f, 1.0f) * screen_width_;
		const float vy = std::clamp(norm_y, 0.0f, 1.0f) * kScreenHeight;

		// 1. Direct tap on Yes / No buttons in any PSP confirmation dialog
		if (exit_dialog_active_ || delete_dialog_active_ || restore_confirm_active_ || browser_confirm_active_) {
			const float cx = screen_width_ * 0.5f;
			if (vy >= 130.0f && vy <= 185.0f) {
				const int choice = (vx < cx) ? 0 : 1;
				if (exit_dialog_active_) exit_dialog_selected_ = choice;
				else if (delete_dialog_active_) delete_dialog_selected_ = choice;
				else if (restore_confirm_active_) restore_confirm_selected_ = choice;
				else if (browser_confirm_active_) browser_confirm_selected_ = choice;
			}
		} else if (options_menu_active_) {
			const float panel_x = screen_width_ - 130.0f;
			if (vx < panel_x) {
				KeyPressed(Key::Back);
				return;
			}
			const float first_y = 106.0f;
			for (int i = 0; i < 3; ++i) {
				const float item_y = first_y + i * 30.0f;
				if (vy >= item_y - 18.0f && vy <= item_y + 10.0f) {
					options_selected_ = i;
					break;
				}
			}
		} else if (settings_sub_active_ && settings_picker_open_) {
			const float panel_x = screen_width_ - 138.0f;
			if (vx < panel_x) {
				KeyPressed(Key::Back);
				return;
			}
			const int max_c = SettingsPickerCount();
			for (int i = 0; i < max_c; ++i) {
				const float y = kSelectedItemY + (float)(i - settings_picker_index_) * 26.0f;
				if (vy >= y - 16.0f && vy <= y + 8.0f) {
					settings_picker_index_ = i;
					break;
				}
			}
		} else if (!system_info_dialog_ && !info_notice_active_ && !info_dialog_active_) {
			// Tapping the left '<' back arrow on sub-pages returns to main XMB
			if ((memory_stick_page_ || saved_data_page_ || settings_sub_active_ || empty_media_kind_ != EmptyMediaKind::None) &&
			    vx <= 54.0f && vy >= 100.0f && vy <= 172.0f) {
				KeyPressed(Key::Back);
				return;
			}
			// Tapping the bottom-right touch hint bar triggers Options (Triangle) or Back (Circle)
			if (vy >= 236.0f) {
				if (vx >= screen_width_ - 92.0f) {
					KeyPressed(Key::Back);
					return;
				}
				if ((memory_stick_page_ || saved_data_page_) &&
				    vx >= screen_width_ - 185.0f && vx < screen_width_ - 92.0f) {
					KeyPressed(Key::Options);
					return;
				}
			}
		}
	}
	KeyPressed(Key::Confirm);
}

std::string XmbRenderer::TextOr(const std::string &key, const std::string &fallback) const {
	auto found = strings_.find(key);
	return found != strings_.end() ? found->second : fallback;
}

bool XmbRenderer::LoadWave() {
	PspWaveLoadResult result = loadPspWaveRco(root_ + "/rco/system_plugin_bg.rco", wave_);
	if (!result.ok) {
		__android_log_print(ANDROID_LOG_WARN, kTag, "system_plugin_bg.rco missing (%s)", result.error.c_str());
		std::vector<uint8_t> gmo;
		if (!ReadFile(root_ + "/models/system_plugin_bg/mdl_bg.gmo", &gmo)) {
			__android_log_print(ANDROID_LOG_WARN, kTag, "missing mdl_bg.gmo");
			return false;
		}
		result = parsePspWaveGmo(gmo, wave_);
		__android_log_print(ANDROID_LOG_INFO, kTag, "wave: ok=%d %s", result.ok ? 1 : 0, result.error.c_str());
		if (!result.ok) {
			return false;
		}
	} else {
		__android_log_print(ANDROID_LOG_INFO, kTag, "wave rco loaded");
	}
	wave_tessellator_.setup(wave_, 128, 48);
	if (!wave_tessellator_.valid()) {
		return false;
	}
	wave_vertices_.resize((size_t)wave_tessellator_.uSamples() * wave_tessellator_.vSamples() * 6);
	if (!BuildWaveProgram()) {
		return false;
	}

	glGenVertexArrays(1, &wave_vao_);
	glGenBuffers(1, &wave_vbo_);
	glGenBuffers(1, &wave_ebo_);
	glBindVertexArray(wave_vao_);
	glBindBuffer(GL_ARRAY_BUFFER, wave_vbo_);
	glBufferData(GL_ARRAY_BUFFER, wave_vertices_.size() * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(float) * 6, (const void *)0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(float) * 6, (const void *)(sizeof(float) * 3));
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, wave_ebo_);
	const auto &indices = wave_tessellator_.indices();
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(uint32_t), indices.data(), GL_STATIC_DRAW);
	glBindVertexArray(0);

	if (!wave_.rgba.empty() && wave_.texW > 0 && wave_.texH > 0) {
		glGenTextures(1, &wave_texture_);
		glBindTexture(GL_TEXTURE_2D, wave_texture_);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, wave_.texW, wave_.texH, 0, GL_RGBA, GL_UNSIGNED_BYTE, wave_.rgba.data());
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	}
	return true;
}

void XmbRenderer::DrawBackground() {
	if (settings_.bg_mode == 2) {
		return;
	}
	if (settings_.theme_style == 2 && settings_.color_index == 0 && settings_.bg_mode == 0) {
		auto cookie_bg = textures_.find("cookies_bg");
		if (cookie_bg != textures_.end() && cookie_bg->second.id != 0) {
			const float color[4] = {0.84f, 0.82f, 0.80f, 1.0f};
			glBindTexture(GL_TEXTURE_2D, cookie_bg->second.id);
			DrawQuad(0.0f, 0.0f, screen_width_, kScreenHeight, color);
			return;
		}
	}
	if (background_texture_ == 0) {
		return;
	}
	const float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
	glBindTexture(GL_TEXTURE_2D, background_texture_);
	DrawQuad(0.0f, 0.0f, screen_width_, kScreenHeight, color);
}

void XmbRenderer::DrawStatusBar() {
	if (!font_loaded_) {
		return;
	}
	float bar_alpha = 0.95f;
	if (opening_t_ < 1.0f) {
		const float u = std::clamp((opening_t_ - 0.55f) / 0.45f, 0.0f, 1.0f);
		bar_alpha *= u * u * (3.0f - 2.0f * u);
	}
	if (bar_alpha <= 0.01f) {
		return;
	}

	// Top-left transient Memory Stick access indicator (authentic PSP boot & loading behavior)
	if (opening_t_ < 0.95f || resume_t_ < 0.85f || games_loading_ || saves_loading_) {
		float ms_slide = 0.0f;
		float ms_alpha = bar_alpha;
		if (!games_loading_ && !saves_loading_) {
			if (opening_t_ < 0.95f && opening_t_ > 0.65f) {
				const float t = (opening_t_ - 0.65f) / 0.30f;
				ms_slide = t * 28.0f;
				ms_alpha *= (1.0f - t);
			} else if (resume_t_ < 0.85f && resume_t_ > 0.50f) {
				const float t = (resume_t_ - 0.50f) / 0.35f;
				ms_slide = t * 28.0f;
				ms_alpha *= (1.0f - t);
			}
		}
		auto ms_tex = textures_.find("status_ms");
		if (ms_tex != textures_.end() && ms_alpha > 0.02f) {
			const float msc[4] = {1.0f, 1.0f, 1.0f, ms_alpha * 0.92f};
			const float mss[4] = {0.0f, 0.0f, 0.0f, ms_alpha * 0.50f};
			DrawTexture(ms_tex->second, 18.0f - ms_slide, 13.0f, 0.95f, mss);
			DrawTexture(ms_tex->second, 17.0f - ms_slide, 12.0f, 0.95f, msc);
			const float led_pulse = 0.45f + 0.55f * std::abs(std::sin(time_elapsed_ * 9.0f));
			const float led_col[4] = {1.0f, 0.58f, 0.14f, ms_alpha * led_pulse};
			DrawSolidQuad(27.0f - ms_slide, 10.0f, 3.5f, 4.0f, led_col);
		}
	}

	char clock[40] = {0};
	const std::time_t now = std::time(nullptr);
	std::tm local{};
	localtime_r(&now, &local);
	const char *fmt = nullptr;
	if (settings_.date_format == 1) {
		fmt = settings_.time_24h ? "%m/%d %H:%M" : "%m/%d %I:%M %p";
	} else if (settings_.date_format == 2) {
		fmt = settings_.time_24h ? "%Y/%m/%d %H:%M" : "%Y/%m/%d %I:%M %p";
	} else {
		fmt = settings_.time_24h ? "%d/%m %H:%M" : "%d/%m %I:%M %p";
	}
	std::strftime(clock, sizeof(clock), fmt, &local);
	const float scale = 0.755f;
	const float width = MeasureText(clock, scale);
	const float color[4] = {1.0f, 1.0f, 1.0f, bar_alpha};
	const float shadow_col[4] = {0.0f, 0.0f, 0.0f, bar_alpha * 0.55f};
	const float clock_x = (screen_width_ - 38.0f) - width;
	DrawText(clock, clock_x, 18.0f, scale, color, true);

	// Authentic PSP top status bar indicators to the left of the clock (Bluetooth, Wi-Fi, AVLS, Mute)
	float icon_x = clock_x - 10.0f;
	if (bluetooth_enabled_) {
		auto bt = textures_.find("status_bt");
		if (bt != textures_.end()) {
			DrawTexture(bt->second, icon_x + 1.0f, 13.0f, 0.52f, shadow_col);
			DrawTexture(bt->second, icon_x, 12.0f, 0.52f, color);
			if (bluetooth_connected_) {
				const float dot_col[4] = {0.55f, 0.85f, 1.0f, bar_alpha};
				DrawSolidQuad(icon_x + 4.5f, 10.5f, 2.2f, 2.2f, dot_col);
			}
			icon_x -= 14.0f;
		}
	}
	if (network_connected_) {
		auto wifi = textures_.find("status_wifi");
		if (wifi != textures_.end() && wifi->second.height > 0) {
			const float wscale = 14.5f / (float)wifi->second.height;
			DrawTexture(wifi->second, icon_x + 1.0f, 13.0f, wscale, shadow_col);
			DrawTexture(wifi->second, icon_x, 12.0f, wscale, color);
			icon_x -= 17.0f;
		}
	}
	if (settings_.avls_enabled) {
		auto avls = textures_.find("status_avls");
		if (avls != textures_.end()) {
			DrawTexture(avls->second, icon_x - 3.0f, 12.5f, 0.42f, color);
			icon_x -= 26.0f;
		}
	}
	if (audio_muted_ || (!settings_.key_tone && !settings_.bgm_enabled)) {
		auto mute = textures_.find("status_mute");
		auto mute_sh = textures_.find("status_mute_shadow");
		if (mute_sh != textures_.end()) {
			DrawTexture(mute_sh->second, icon_x, 12.5f, 1.0f, color);
		}
		if (mute != textures_.end()) {
			DrawTexture(mute->second, icon_x, 12.0f, 1.0f, color);
			icon_x -= 15.0f;
		}
	}

	auto battery = textures_.find("battery");
	if (battery != textures_.end()) {
		int frame = 0;
		if (battery_charging_) {
			static const int charging_frames[] = {3, 2, 1, 0};
			const int step = static_cast<int>(time_elapsed_ * 2.0f) % 4;
			frame = charging_frames[step];
		} else {
			if (battery_level_ >= 70) {
				frame = 0;
			} else if (battery_level_ >= 40) {
				frame = 1;
			} else if (battery_level_ >= 15) {
				frame = 2;
			} else {
				frame = 3;
				if (battery_level_ < 10 && (static_cast<int>(time_elapsed_ * 2.0f) % 2 == 1)) {
					return;
				}
			}
		}
		DrawTextureCrop(battery->second, screen_width_ - 16.0f, 12.0f, 1.0f, color, 0, frame * 16, 44, 16);
	}
}

void XmbRenderer::DrawWave(float dt) {
	if (!wave_loaded_ || wave_program_ == 0 || settings_.wave_mode == 3) {
		return;
	}
	float speed = 1.0f;
	if (settings_.wave_mode == 1) speed = 0.35f;
	else if (settings_.wave_mode == 2) speed = 0.0f;

	const float loop = wave_.loopFrames();
	if (loop > 0.0f && speed > 0.0f) {
		wave_frame_ = std::fmod(wave_frame_ + dt * wave_.frameRate * speed, loop);
	}
	wave_tessellator_.evaluate(wave_, wave_.frameStart + wave_frame_, wave_vertices_);
	if (wave_vertices_.empty()) {
		return;
	}

	glUseProgram(wave_program_);
	glBindVertexArray(wave_vao_);
	glBindBuffer(GL_ARRAY_BUFFER, wave_vbo_);
	glBufferSubData(GL_ARRAY_BUFFER, 0, wave_vertices_.size() * sizeof(float), wave_vertices_.data());
	float mvp[16];
	BuildWaveMvp(mvp, screen_width_);
	glUniformMatrix4fv(wave_mvp_, 1, GL_FALSE, mvp);
	const float alpha = wave_.alpha * wave_.tint[3] * kWaveBrightness * (0.2f + 0.8f * boot_t_);
	const float wr = std::max(0.24f, wave_tint_[0]);
	const float wg = std::max(0.24f, wave_tint_[1]);
	const float wb = std::max(0.24f, wave_tint_[2]);
	glUniform4f(wave_color_, wave_.diffuse[0] * wave_.tint[0] * alpha * wr * 1.45f,
	            wave_.diffuse[1] * wave_.tint[1] * alpha * wg * 1.45f,
	            wave_.diffuse[2] * wave_.tint[2] * alpha * wb * 1.45f, 1.0f);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, wave_texture_);
	glUniform1i(wave_sampler_, 0);
	glEnable(GL_BLEND);
	glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ZERO, GL_ONE);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glDrawElements(GL_TRIANGLES, (GLsizei)wave_tessellator_.indices().size(), GL_UNSIGNED_INT, nullptr);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glBindVertexArray(0);
}

bool XmbRenderer::LoadTexture(const std::string &name, const std::string &relative_path) {
	std::vector<uint8_t> file;
	if (!ReadFile(root_ + "/" + relative_path, &file)) {
		__android_log_print(ANDROID_LOG_WARN, kTag, "missing texture %s", relative_path.c_str());
		return false;
	}
	int width = 0;
	int height = 0;
	std::vector<uint8_t> rgba;
	if (!DecodePng(file, &width, &height, &rgba)) {
		__android_log_print(ANDROID_LOG_WARN, kTag, "cannot decode %s", relative_path.c_str());
		return false;
	}
	Texture texture;
	texture.width = width;
	texture.height = height;
	glGenTextures(1, &texture.id);
	glBindTexture(GL_TEXTURE_2D, texture.id);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	textures_[name] = texture;
	return true;
}

void XmbRenderer::Resize(int width, int height) {
	surface_width_ = width;
	surface_height_ = height;
}

void XmbRenderer::DrawQuad(float x, float y, float width, float height, const float color[4]) {
	DrawQuadUv(x, y, width, height, color, 0.0f, 0.0f, 1.0f, 1.0f);
}

void XmbRenderer::DrawSolidQuad(float x, float y, float width, float height, const float color[4]) {
	glBindTexture(GL_TEXTURE_2D, white_texture_);
	DrawQuadUv(x, y, width, height, color, 0.0f, 0.0f, 1.0f, 1.0f);
}

void XmbRenderer::DrawQuadUv(float x, float y, float width, float height, const float color[4],
                             float u0, float v0, float u1, float v1) {
	const float x1 = x + width;
	const float y1 = y + height;
	const float vertices[24] = {
		x, y, u0, v0,
		x1, y, u1, v0,
		x, y1, u0, v1,
		x1, y, u1, v0,
		x1, y1, u1, v1,
		x, y1, u0, v1,
	};
	glUniform4fv(u_color_, 1, color);
	glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer_);
	glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(vertices), vertices);
	glDrawArrays(GL_TRIANGLES, 0, 6);
}

void XmbRenderer::DrawTexture(const Texture &texture, float center_x, float center_y, float scale,
                              const float color[4]) {
	DrawTextureUv(texture, center_x, center_y, scale, color, 0.0f, 0.0f, 1.0f, 1.0f);
}

void XmbRenderer::DrawTextureUv(const Texture &texture, float center_x, float center_y, float scale,
                                const float color[4], float u0, float v0, float u1, float v1) {
	if (texture.id == 0) {
		return;
	}
	const float width = texture.width * scale;
	const float height = texture.height * scale;
	glBindTexture(GL_TEXTURE_2D, texture.id);
	DrawQuadUv(center_x - width * 0.5f, center_y - height * 0.5f, width, height, color, u0, v0, u1, v1);
}

void XmbRenderer::DrawTextureCrop(const Texture &texture, float center_x, float center_y, float scale,
                                  const float color[4], int x, int y, int width, int height) {
	if (texture.id == 0 || width <= 0 || height <= 0) {
		return;
	}
	const float u0 = (float)x / texture.width;
	const float v0 = (float)y / texture.height;
	const float u1 = (float)(x + width) / texture.width;
	const float v1 = (float)(y + height) / texture.height;
	glBindTexture(GL_TEXTURE_2D, texture.id);
	DrawQuadUv(center_x - width * scale * 0.5f, center_y - height * scale * 0.5f,
	           width * scale, height * scale, color, u0, v0, u1, v1);
}

const XmbRenderer::Texture *XmbRenderer::TextureForPath(const std::string &path) {
	auto found = image_textures_.find(path);
	if (found != image_textures_.end()) {
		return found->second.id != 0 ? &found->second : nullptr;
	}
	Texture texture;
	std::vector<uint8_t> file;
	int width = 0;
	int height = 0;
	std::vector<uint8_t> rgba;
	if (ReadFile(path, &file) && DecodePng(file, &width, &height, &rgba)) {
		texture.width = width;
		texture.height = height;
		glGenTextures(1, &texture.id);
		glBindTexture(GL_TEXTURE_2D, texture.id);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	} else {
		__android_log_print(ANDROID_LOG_WARN, kTag, "cannot load image %s", path.c_str());
	}
	const auto inserted = image_textures_.emplace(path, texture);
	return inserted.first->second.id != 0 ? &inserted.first->second : nullptr;
}

const XmbRenderer::Texture *XmbRenderer::GlyphTexture(uint32_t codepoint) {
	auto found = glyph_textures_.find(codepoint);
	if (found != glyph_textures_.end()) {
		return found->second.id != 0 ? &found->second : nullptr;
	}
	const FontGlyph *glyph = font_.Glyph(codepoint);
	if (!glyph || glyph->width <= 0 || glyph->height <= 0 || glyph->pixels.empty()) {
		return nullptr;
	}
	std::vector<uint8_t> rgba((size_t)glyph->width * glyph->height * 4);
	for (size_t i = 0; i < glyph->pixels.size(); ++i) {
		rgba[i * 4 + 0] = 255;
		rgba[i * 4 + 1] = 255;
		rgba[i * 4 + 2] = 255;
		rgba[i * 4 + 3] = (uint8_t)(glyph->pixels[i] * 17);
	}
	Texture texture;
	texture.width = glyph->width;
	texture.height = glyph->height;
	glGenTextures(1, &texture.id);
	glBindTexture(GL_TEXTURE_2D, texture.id);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, texture.width, texture.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	const auto inserted = glyph_textures_.emplace(codepoint, texture);
	return &inserted.first->second;
}

const XmbRenderer::Texture *XmbRenderer::ShadowGlyphTexture(uint32_t codepoint) {
	auto found = shadow_glyph_textures_.find(codepoint);
	if (found != shadow_glyph_textures_.end()) {
		return &found->second;
	}
	const FontGlyph *glyph = font_.Glyph(codepoint, true);
	if (!glyph || glyph->width <= 0 || glyph->height <= 0 || glyph->pixels.empty()) {
		return nullptr;
	}
	std::vector<uint8_t> rgba((size_t)glyph->width * glyph->height * 4);
	for (size_t i = 0; i < glyph->pixels.size(); ++i) {
		rgba[i * 4 + 0] = 255;
		rgba[i * 4 + 1] = 255;
		rgba[i * 4 + 2] = 255;
		rgba[i * 4 + 3] = (uint8_t)(glyph->pixels[i] * 17);
	}
	Texture texture;
	texture.width = glyph->width;
	texture.height = glyph->height;
	glGenTextures(1, &texture.id);
	glBindTexture(GL_TEXTURE_2D, texture.id);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, texture.width, texture.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	const auto inserted = shadow_glyph_textures_.emplace(codepoint, texture);
	return &inserted.first->second;
}

bool XmbRenderer::EnsureFallbackGlyph(uint32_t codepoint, FallbackGlyph *out_metrics, const Texture **out_tex) {
	auto found = fallback_glyphs_.find(codepoint);
	if (found != fallback_glyphs_.end()) {
		*out_metrics = found->second;
		auto tf = glyph_textures_.find(codepoint);
		*out_tex = (tf != glyph_textures_.end() && tf->second.id != 0) ? &tf->second : nullptr;
		return true;
	}
	FallbackGlyph metrics;
	std::vector<uint8_t> alpha;
	if (!RasterizeFallbackGlyph(codepoint, &metrics, &alpha)) {
		return false;
	}
	fallback_glyphs_[codepoint] = metrics;
	*out_metrics = metrics;
	*out_tex = nullptr;

	if (metrics.width > 0 && metrics.height > 0 && alpha.size() == (size_t)metrics.width * metrics.height) {
		std::vector<uint8_t> rgba(alpha.size() * 4);
		for (size_t i = 0; i < alpha.size(); ++i) {
			rgba[i * 4 + 0] = 255;
			rgba[i * 4 + 1] = 255;
			rgba[i * 4 + 2] = 255;
			rgba[i * 4 + 3] = alpha[i];
		}
		Texture texture;
		texture.width = metrics.width;
		texture.height = metrics.height;
		glGenTextures(1, &texture.id);
		glBindTexture(GL_TEXTURE_2D, texture.id);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, texture.width, texture.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		const auto inserted = glyph_textures_.emplace(codepoint, texture);
		*out_tex = &inserted.first->second;
	}
	return true;
}

float XmbRenderer::MeasureText(const std::string &text, float scale) {
	if (!font_loaded_) {
		return 0.0f;
	}
	size_t position = 0;
	float width = 0.0f;
	while (true) {
		const uint32_t codepoint = NextCodepoint(text, position);
		if (codepoint == 0) {
			break;
		}
		if (const FontGlyph *glyph = font_.Glyph(codepoint)) {
			width += glyph->advance_h / 64.0f * scale;
		} else {
			FallbackGlyph fb;
			const Texture *fb_tex = nullptr;
			if (EnsureFallbackGlyph(codepoint, &fb, &fb_tex)) {
				width += fb.advance_h / 64.0f * scale;
			}
		}
	}
	return width;
}

float XmbRenderer::DrawText(const std::string &text, float x, float baseline, float scale,
                            const float color[4], bool shadow) {
	if (!font_loaded_) {
		return 0.0f;
	}
	const float start = x;
	size_t position = 0;
	const float shadow_color[4] = {0.0f, 0.0f, 0.0f, color[3] * 0.62f};
	while (true) {
		const uint32_t codepoint = NextCodepoint(text, position);
		if (codepoint == 0) {
			break;
		}
		int g_left = 0, g_top = 0, g_w = 0, g_h = 0, g_adv = 0;
		const Texture *texture = nullptr;
		if (const FontGlyph *glyph = font_.Glyph(codepoint)) {
			g_left = glyph->left;
			g_top = glyph->top;
			g_w = glyph->width;
			g_h = glyph->height;
			g_adv = glyph->advance_h;
			texture = GlyphTexture(codepoint);
		} else {
			FallbackGlyph fb;
			if (!EnsureFallbackGlyph(codepoint, &fb, &texture)) {
				continue;
			}
			g_left = fb.left;
			g_top = fb.top;
			g_w = fb.width;
			g_h = fb.height;
			g_adv = fb.advance_h;
		}
		if (texture && g_w > 0 && g_h > 0) {
			const float glyph_x = x + g_left * scale;
			const float glyph_y = baseline - g_top * scale;
			const float width = g_w * scale;
			const float height = g_h * scale;
			glBindTexture(GL_TEXTURE_2D, texture->id);
			if (shadow) {
				DrawQuad(glyph_x + 1.0f, glyph_y + 1.0f, width, height, shadow_color);
			}
			DrawQuad(glyph_x, glyph_y, width, height, color);
		}
		x += g_adv / 64.0f * scale;
	}
	return x - start;
}

std::array<float, 3> XmbRenderer::ColorSwatchForChoice(int choice_index) const {
	if (choice_index >= 13 && choice_index <= 34) {
		const float *s = kExtColorSwatches[choice_index - 13];
		return {s[0], s[1], s[2]};
	}
	const int m = (choice_index >= 1 && choice_index <= 12) ? (choice_index - 1) : EffectiveColorIndex();
	const float *s = kMonthGradients[m];
	return {s[0], s[1], s[2]};
}

std::vector<std::string> XmbRenderer::WrapText(const std::string &text, float scale, float max_width) {
	std::vector<std::string> lines;
	if (text.empty()) {
		return lines;
	}
	size_t start = 0;
	while (start <= text.size()) {
		size_t nl = text.find('\n', start);
		if (nl == std::string::npos) {
			nl = text.size();
		}
		const std::string para = text.substr(start, nl - start);
		if (para.empty()) {
			lines.emplace_back();
		} else {
			std::string cur;
			size_t pos = 0;
			while (pos < para.size()) {
				while (pos < para.size() && para[pos] == ' ') ++pos;
				if (pos >= para.size()) break;
				size_t end = para.find(' ', pos);
				if (end == std::string::npos) end = para.size();
				const std::string word = para.substr(pos, end - pos);
				const std::string candidate = cur.empty() ? word : (cur + " " + word);
				if (cur.empty() || MeasureText(candidate, scale) <= max_width) {
					cur = candidate;
				} else {
					lines.push_back(cur);
					cur = word;
				}
				pos = end;
			}
			if (!cur.empty()) {
				lines.push_back(cur);
			}
		}
		if (nl >= text.size()) break;
		start = nl + 1;
	}
	return lines;
}

void XmbRenderer::OpenNoticeDialog(const std::string &title, const std::vector<std::string> &lines) {
	info_notice_title_ = title;
	info_notice_lines_ = lines;
	info_notice_active_ = true;
}

void XmbRenderer::UpdateThemeColor() {
	if (background_texture_ == 0) {
		glGenTextures(1, &background_texture_);
	}
	if (settings_.theme_style == 2 && settings_.color_index == 0 && settings_.bg_mode == 0) {
		wave_tint_ = {0.76f, 0.62f, 0.46f};
		return;
	}

	// Map official PSP-3000 console edition themes (3..11) to their real 13-27.bmp palette index
	static const int kEditionExtMap[9] = {
		15, // 3: Piano Black (Deep Obsidian Navy)
		21, // 4: Ceramic White (Pearl Silver)
		2,  // 5: Radiant Red (Carnival Red)
		12, // 6: Vibrant Blue (Carnival Blue)
		17, // 7: Spirited Green (Carnival Green)
		19, // 8: Bright Yellow (Carnival Yellow)
		0,  // 9: Blossom Pink
		14, // 10: Turquoise Green (LittleBigPlanet)
		7,  // 11: Lilac Purple (Hannah Montana)
	};

	const bool use_classic = (settings_.bg_mode == 1 || settings_.theme_style == 1);
	char rel_path[64] = {0};
	if (settings_.theme_style >= 3 && settings_.theme_style <= 11 && settings_.color_index == 0 && settings_.bg_mode == 0) {
		const int ext_idx = kEditionExtMap[settings_.theme_style - 3];
		std::snprintf(rel_path, sizeof(rel_path), "images/themes/ext_%02d.png", ext_idx);
	} else if (settings_.color_index >= 13 && settings_.color_index <= 34) {
		std::snprintf(rel_path, sizeof(rel_path), "images/themes/ext_%02d.png", settings_.color_index - 13);
	} else {
		const int month_index = EffectiveColorIndex();
		std::snprintf(rel_path, sizeof(rel_path), "images/themes/%s_%02d.png",
		              use_classic ? "classic" : "orig", month_index);
	}

	std::vector<uint8_t> png_file;
	std::vector<uint8_t> rgba;
	int w = 0;
	int h = 0;
	if (!root_.empty() && ReadFile(root_ + "/" + rel_path, &png_file) &&
	    DecodePng(png_file, &w, &h, &rgba) && w > 0 && h > 0) {
		const size_t mid = ((size_t)(h / 2) * (size_t)w + (size_t)(w / 2)) * 4;
		if (mid + 2 < rgba.size()) {
			wave_tint_ = {
				(float)rgba[mid + 0] / 255.0f,
				(float)rgba[mid + 1] / 255.0f,
				(float)rgba[mid + 2] / 255.0f,
			};
		}
		glBindTexture(GL_TEXTURE_2D, background_texture_);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		return;
	}

	// Fallback if extracted theme image is missing
	const std::array<float, 3> swatch = (settings_.theme_style == 3 && settings_.color_index == 0)
	                                        ? std::array<float, 3>{0.06f, 0.06f, 0.12f}
	                                        : (use_classic && settings_.color_index == 0
	                                               ? std::array<float, 3>{kClassicBlueTint[0], kClassicBlueTint[1], kClassicBlueTint[2]}
	                                               : ColorSwatchForChoice(settings_.color_index));
	wave_tint_ = swatch;
	constexpr size_t row_count = sizeof(kBackgroundRows) / sizeof(kBackgroundRows[0]);
	std::vector<uint8_t> fb_rgba(row_count * 4);
	for (size_t i = 0; i < row_count; ++i) {
		const float v = (float)kBackgroundRows[i][0] / 185.0f;
		const float haze = (i < 8) ? (8.0f - (float)i) * 0.010f : 0.0f;
		fb_rgba[i * 4 + 0] = (uint8_t)(std::clamp(swatch[0] * v + haze, 0.0f, 1.0f) * 255.0f + 0.5f);
		fb_rgba[i * 4 + 1] = (uint8_t)(std::clamp(swatch[1] * v + haze, 0.0f, 1.0f) * 255.0f + 0.5f);
		fb_rgba[i * 4 + 2] = (uint8_t)(std::clamp(swatch[2] * v + haze, 0.0f, 1.0f) * 255.0f + 0.5f);
		fb_rgba[i * 4 + 3] = 255;
	}
	glBindTexture(GL_TEXTURE_2D, background_texture_);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, (GLsizei)row_count, 0, GL_RGBA, GL_UNSIGNED_BYTE, fb_rgba.data());
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

void XmbRenderer::ShowToast(const std::string &message) {
	toast_message_ = message;
	toast_timer_ = 2.5f;
	show_toast_ = true;
}

void XmbRenderer::DrawToast() {
	if (toast_timer_ <= 0.0f || toast_message_.empty() || !font_loaded_) {
		return;
	}
	float alpha = std::min(1.0f, toast_timer_ * 2.0f);
	float scale = 0.72f;
	float text_width = MeasureText(toast_message_, scale);
	float pill_width = std::min(screen_width_ - 32.0f, text_width + 36.0f);
	float pill_x = (screen_width_ - pill_width) * 0.5f;
	float pill_y = 224.0f;
	float pill_h = 26.0f;

	const float bg_color[4] = {0.1f, 0.1f, 0.12f, 0.90f * alpha};
	DrawSolidQuad(pill_x, pill_y, pill_width, pill_h, bg_color);
	const float border_color[4] = {1.0f, 1.0f, 1.0f, 0.35f * alpha};
	DrawSolidQuad(pill_x, pill_y, pill_width, 1.0f, border_color);
	DrawSolidQuad(pill_x, pill_y + pill_h - 1.0f, pill_width, 1.0f, border_color);

	const float text_color[4] = {1.0f, 1.0f, 1.0f, alpha};
	DrawText(toast_message_, pill_x + 18.0f, pill_y + 18.0f, scale, text_color, true);
}

void XmbRenderer::DrawDialogFrame(float box_x, float box_y, float box_w, float box_h) {
	const float veil[4] = {0.0f, 0.0f, 0.0f, 0.74f};
	DrawSolidQuad(0.0f, 0.0f, screen_width_, kScreenHeight, veil);

	const float bg_box[4] = {0.10f, 0.10f, 0.12f, 0.96f};
	DrawSolidQuad(box_x, box_y, box_w, box_h, bg_box);

	const float border[4] = {1.0f, 1.0f, 1.0f, 0.35f};
	DrawSolidQuad(box_x, box_y, box_w, 1.5f, border);
	DrawSolidQuad(box_x, box_y + box_h - 1.5f, box_w, 1.5f, border);
	DrawSolidQuad(box_x, box_y, 1.5f, box_h, border);
	DrawSolidQuad(box_x + box_w - 1.5f, box_y, 1.5f, box_h, border);
}

void XmbRenderer::DrawDialogHints(float box_x, float box_y, float box_w, float box_h, bool show_enter) {
	if (!font_loaded_) return;
	const char *confirm_icon_key = settings_.swap_confirm ? "btn_circle" : "btn_cross";
	const char *back_icon_key = settings_.swap_confirm ? "btn_cross" : "btn_circle";
	auto confirm_icon = textures_.find(confirm_icon_key);
	auto back_icon = textures_.find(back_icon_key);

	const std::string enter_lbl = TextOr("xmb_hint_enter", "Enter");
	const std::string back_lbl = TextOr("xmb_hint_back", "Back");
	const float hint_scale = 0.54f;
	const float hint_color[4] = {1.0f, 1.0f, 1.0f, 0.72f};
	const float hint_y = box_y + box_h - 10.0f;

	float total_w = 12.0f + MeasureText(back_lbl, hint_scale);
	if (show_enter) {
		total_w += 12.0f + MeasureText(enter_lbl, hint_scale) + 16.0f;
	}
	float cur_x = box_x + box_w - total_w - 14.0f;
	if (show_enter) {
		if (confirm_icon != textures_.end()) {
			DrawTexture(confirm_icon->second, cur_x, hint_y - 4.0f, 1.0f, hint_color);
			cur_x += 12.0f;
		}
		cur_x += DrawText(enter_lbl, cur_x, hint_y, hint_scale, hint_color, true) + 16.0f;
	}
	if (back_icon != textures_.end()) {
		DrawTexture(back_icon->second, cur_x, hint_y - 4.0f, 1.0f, hint_color);
		cur_x += 12.0f;
	}
	DrawText(back_lbl, cur_x, hint_y, hint_scale, hint_color, true);
}

void XmbRenderer::DrawYesNoDialog(const std::string &line1, const std::string &line2, int selected) {
	float max_text_w = 0.0f;
	const float base_scale = line2.empty() ? 0.72f : 0.68f;
	if (font_loaded_) {
		max_text_w = MeasureText(line1, base_scale);
		if (!line2.empty()) {
			max_text_w = std::max(max_text_w, MeasureText(line2, base_scale));
		}
	}
	const float max_allowed_box_w = std::min(436.0f, screen_width_ - 24.0f);
	const float box_w = std::clamp(max_text_w + 44.0f, std::min(330.0f, max_allowed_box_w), max_allowed_box_w);
	const float box_x = (screen_width_ - box_w) * 0.5f;
	const float box_y = 66.0f;
	const float box_h = 138.0f;
	DrawDialogFrame(box_x, box_y, box_w, box_h);

	if (font_loaded_) {
		const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
		const float inner_max_w = box_w - 32.0f;
		auto draw_centered_fit = [&](const std::string &txt, float y, float pref_scale) {
			if (txt.empty()) return;
			float sc = pref_scale;
			float w = MeasureText(txt, sc);
			if (w > inner_max_w && w > 1.0f) {
				sc = std::max(0.46f, sc * (inner_max_w / w));
				w = MeasureText(txt, sc);
			}
			DrawText(txt, (screen_width_ - w) * 0.5f, y, sc, white, true);
		};
		if (line2.empty()) {
			draw_centered_fit(line1, box_y + 46.0f, 0.72f);
		} else {
			draw_centered_fit(line1, box_y + 36.0f, 0.68f);
			draw_centered_fit(line2, box_y + 56.0f, 0.68f);
		}

		const std::string yes_text = TextOr("msg_yes", "Yes");
		const std::string no_text = TextOr("msg_no", "No");
		const float btn_scale = 0.74f;
		const float yes_w = MeasureText(yes_text, btn_scale);
		const float no_w = MeasureText(no_text, btn_scale);

		const float cx = screen_width_ * 0.5f;
		const float yes_x = (cx - 75.0f) - yes_w * 0.5f;
		const float no_x = (cx + 75.0f) - no_w * 0.5f;
		const float btn_y = box_y + 94.0f;

		const float focus_color[4] = {1.0f, 1.0f, 1.0f, 0.28f};
		if (selected == 0) {
			DrawSolidQuad(yes_x - 14.0f, btn_y - 18.0f, yes_w + 28.0f, 26.0f, focus_color);
		} else {
			DrawSolidQuad(no_x - 14.0f, btn_y - 18.0f, no_w + 28.0f, 26.0f, focus_color);
		}

		DrawText(yes_text, yes_x, btn_y, btn_scale, white, true);
		DrawText(no_text, no_x, btn_y, btn_scale, white, true);
		DrawDialogHints(box_x, box_y, box_w, box_h, true);
	}
}

void XmbRenderer::SetExitDialog(bool active) {
	std::lock_guard<std::recursive_mutex> lock(mutex_);
	exit_dialog_active_ = active;
	if (active) {
		exit_dialog_selected_ = 1;
		PlaySound(0);
	}
}

void XmbRenderer::DrawExitDialog() {
	if (!exit_dialog_active_) {
		return;
	}
	DrawYesNoDialog(TextOr("msg_exit_psp", "Do you want to quit PSP mode?"), "", exit_dialog_selected_);
}

void XmbRenderer::DrawSavedDataPage() {
	const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
	auto arrow = textures_.find("arrow_l");
	if (arrow != textures_.end()) {
		const float shadow_color[4] = {0.0f, 0.0f, 0.0f, 0.48f};
		DrawTexture(arrow->second, 10.0f, 137.0f, 1.0f, shadow_color);
		DrawTexture(arrow->second, 9.0f, 136.0f, 1.0f, white);
	}
	auto cat_icon = textures_.find("item_savedata");

	if (saves_.empty()) {
		if (saves_loading_) {
			auto ld = textures_.find("item_loading");
			if (ld != textures_.end()) {
				const float pulse = 0.45f + 0.40f * std::sin(time_elapsed_ * 5.0f);
				const float lcol[4] = {1.0f, 1.0f, 1.0f, pulse};
				DrawTexture(ld->second, 92.0f, 136.0f, 0.85f, lcol);
			}
		} else if (font_loaded_) {
			const float empty_col[4] = {1.0f, 1.0f, 1.0f, 0.78f};
			DrawText(TextOr("msg_no_saved_data", "There is no saved data."), 174.0f, 145.0f, 0.82f, empty_col, true);
		}
		return;
	}

	save_page_index_ = std::max(0, std::min((int)saves_.size() - 1, save_page_index_));
	const auto &selected_save = saves_[save_page_index_];
	const float dim_white[4] = {1.0f, 1.0f, 1.0f, 0.78f};

	if (font_loaded_) {
		DrawText(selected_save.game_title, 174.0f, 70.0f, 0.80f, white, true);
		DrawText(selected_save.slot_name, 174.0f, 96.0f, 0.68f, dim_white, true);
		DrawText(selected_save.date_time, 174.0f, 118.0f, 0.62f, dim_white, true);
		DrawText(selected_save.size_str, 174.0f, 138.0f, 0.62f, dim_white, true);
	}

	if (!selected_save.preview_path.empty()) {
		if (const Texture *t = TextureForPath(selected_save.preview_path)) {
			if (t->width > 0 && t->height > 0) {
				const float box_cx = 320.0f;
				const float box_cy = 190.0f;
				const float thumb_w = 144.0f;
				const float thumb_h = 81.0f;
				const float border_col[4] = {1.0f, 1.0f, 1.0f, 0.35f};
				DrawSolidQuad(box_cx - thumb_w * 0.5f - 1.5f, box_cy - thumb_h * 0.5f - 1.5f,
				              thumb_w + 3.0f, thumb_h + 3.0f, border_col);
				const float scale = std::min(thumb_w / (float)t->width, thumb_h / (float)t->height);
				DrawTexture(*t, box_cx, box_cy, scale, white);
			}
		}
	}

	for (int i = 0; i < (int)saves_.size(); ++i) {
		const float offset = (float)i - save_scroll_;
		const float y = 136.0f + offset * 75.5f;
		if (y < -60.0f || y > 332.0f) {
			continue;
		}
		const float dist = std::abs(offset);
		if (dist < 0.6f) {
			const float t = 1.0f - dist;
			auto glow = textures_.find("focus_glow");
			if (glow != textures_.end()) {
				const float glow_color[4] = {1.0f, 1.0f, 1.0f, 0.70f * t};
				DrawTexture(glow->second, 92.0f, y, 0.88f, glow_color);
			}
			const Texture *icon = nullptr;
			if (!saves_[i].preview_path.empty()) {
				icon = TextureForPath(saves_[i].preview_path);
			}
			if (icon == nullptr && cat_icon != textures_.end()) {
				icon = &cat_icon->second;
			}
			if (icon && icon->width > 0 && icon->height > 0) {
				const float scale = std::min(144.0f / (float)icon->width, 80.0f / (float)icon->height);
				DrawTexture(*icon, 92.0f, y, scale, white);
			}
		} else {
			const float alpha = std::max(0.18f, 0.68f - (dist - 1.0f) * 0.22f);
			const float icon_color[4] = {1.0f, 1.0f, 1.0f, alpha};
			const Texture *icon = nullptr;
			if (!saves_[i].preview_path.empty()) {
				icon = TextureForPath(saves_[i].preview_path);
			}
			if (icon == nullptr && cat_icon != textures_.end()) {
				icon = &cat_icon->second;
			}
			if (icon && icon->width > 0 && icon->height > 0) {
				const float scale = std::min(81.0f / (float)icon->width, 45.0f / (float)icon->height);
				DrawTexture(*icon, 92.5f, y, scale, icon_color);
			}
		}
	}
}

void XmbRenderer::DrawEmptyMediaPage() {
	if (empty_media_kind_ == EmptyMediaKind::None) return;
	const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
	auto arrow = textures_.find("arrow_l");
	if (arrow != textures_.end()) {
		const float shadow_color[4] = {0.0f, 0.0f, 0.0f, 0.48f};
		DrawTexture(arrow->second, 11.0f, 137.0f, 1.0f, shadow_color);
		DrawTexture(arrow->second, 10.0f, 136.0f, 1.0f, white);
	}
	const char *cat_tex = "item_memstick";
	const char *msg_key = "msg_no_images";
	const char *msg_fb = "There are no images.";
	if (empty_media_kind_ == EmptyMediaKind::Photo) {
		cat_tex = "photo";
		msg_key = "xmb_no_images";
		msg_fb = "There are no images.";
	} else if (empty_media_kind_ == EmptyMediaKind::Music) {
		cat_tex = "music";
		msg_key = "xmb_no_tracks";
		msg_fb = "There are no tracks.";
	} else if (empty_media_kind_ == EmptyMediaKind::Video) {
		cat_tex = "video";
		msg_key = "xmb_no_videos";
		msg_fb = "There are no videos.";
	} else if (empty_media_kind_ == EmptyMediaKind::Comics) {
		cat_tex = "item_comics";
		msg_key = "xmb_no_content";
		msg_fb = "There is no content.";
	}
	auto cat_icon = textures_.find(cat_tex);
	if (cat_icon != textures_.end() && cat_icon->second.width > 0 && cat_icon->second.height > 0) {
		const float cat_color[4] = {1.0f, 1.0f, 1.0f, 0.48f};
		const float scale = std::min(1.0f, std::min(28.0f / (float)cat_icon->second.width, 28.0f / (float)cat_icon->second.height));
		DrawTexture(cat_icon->second, 38.0f, 136.0f, scale, cat_color);
	}
	if (font_loaded_) {
		const float empty_col[4] = {1.0f, 1.0f, 1.0f, 0.78f};
		DrawText(TextOr(msg_key, msg_fb), 174.0f, 145.0f, 0.82f, empty_col, true);
	}
}

int XmbRenderer::SettingsRowCount() const {
	switch (settings_kind_) {
	case SettingsKind::Theme: return 4;
	case SettingsKind::System: return 3;
	case SettingsKind::Sound: return 3;
	case SettingsKind::Video: return 2;
	case SettingsKind::Photo: return 2;
	case SettingsKind::DateTime: return 2;
	case SettingsKind::PowerSave: return 2;
	case SettingsKind::Security: return 1;
	case SettingsKind::Network: return 2;
	case SettingsKind::Rss: return 1;
	case SettingsKind::Display: return 2;
	case SettingsKind::Bluetooth: return 2;
	}
	return 1;
}

int XmbRenderer::SettingsPickerCount() const {
	switch (settings_kind_) {
	case SettingsKind::Theme:
		if (settings_sub_index_ == 0) return 12;
		if (settings_sub_index_ == 1) return 35;
		if (settings_sub_index_ == 2) return 3;
		return 4;
	case SettingsKind::System:
		return 2;
	case SettingsKind::Sound:
		return 2;
	case SettingsKind::Video:
		return settings_sub_index_ == 0 ? 3 : 2;
	case SettingsKind::Photo:
		return 3;
	case SettingsKind::DateTime:
		return settings_sub_index_ == 0 ? 3 : 2;
	case SettingsKind::PowerSave:
		return settings_sub_index_ == 0 ? 3 : 2;
	case SettingsKind::Security:
		return 10;
	case SettingsKind::Network:
		return settings_sub_index_ == 1 ? 4 : 0;
	case SettingsKind::Rss:
		return 2;
	case SettingsKind::Display:
		return settings_sub_index_ == 0 ? 3 : 2;
	case SettingsKind::Bluetooth:
		return 0;
	}
	return 2;
}

int XmbRenderer::SettingsCurrentValue() const {
	switch (settings_kind_) {
	case SettingsKind::Theme:
		if (settings_sub_index_ == 0) return settings_.theme_style;
		if (settings_sub_index_ == 1) return settings_.color_index;
		if (settings_sub_index_ == 2) return settings_.bg_mode;
		return settings_.wave_mode;
	case SettingsKind::System:
		if (settings_sub_index_ == 1) return settings_.swap_confirm ? 1 : 0;
		return 0;
	case SettingsKind::Sound:
		if (settings_sub_index_ == 0) return settings_.key_tone ? 0 : 1;
		if (settings_sub_index_ == 1) return settings_.bgm_enabled ? 0 : 1;
		return settings_.avls_enabled ? 0 : 1;
	case SettingsKind::Video:
		return settings_sub_index_ == 0 ? settings_.aspect_mode : (seq_play_enabled_ ? 0 : 1);
	case SettingsKind::Photo:
		return settings_sub_index_ == 0 ? settings_.slideshow_speed : settings_.slideshow_effect;
	case SettingsKind::DateTime:
		if (settings_sub_index_ == 0) return settings_.date_format;
		return settings_.time_24h ? 0 : 1;
	case SettingsKind::PowerSave:
		return settings_sub_index_ == 0 ? settings_.keep_screen_mode : (wlan_power_save_ ? 0 : 1);
	case SettingsKind::Security:
		return settings_.parental_level;
	case SettingsKind::Network:
		return settings_sub_index_ == 1 ? settings_.adhoc_channel : 0;
	case SettingsKind::Rss:
		return settings_.rss_auto_update ? 0 : 1;
	case SettingsKind::Display:
		if (settings_sub_index_ == 0) return settings_.aspect_mode;
		return settings_.touch_controls_enabled ? 0 : 1;
	case SettingsKind::Bluetooth:
		return bluetooth_enabled_ ? 0 : 1;
	}
	return 0;
}

void XmbRenderer::ApplySettingsPickerChoice(int v) {
	switch (settings_kind_) {
	case SettingsKind::Theme:
		if (settings_sub_index_ == 0) {
			settings_.theme_style = v;
			if (v >= 2) {
				settings_.color_index = 0;
				settings_.bg_mode = 0;
			}
		} else if (settings_sub_index_ == 1) settings_.color_index = v;
		else if (settings_sub_index_ == 2) settings_.bg_mode = v;
		else settings_.wave_mode = v;
		theme_dirty_ = true;
		break;
	case SettingsKind::System:
		if (settings_sub_index_ == 1) settings_.swap_confirm = (v == 1);
		break;
	case SettingsKind::Sound:
		if (settings_sub_index_ == 0) settings_.key_tone = (v == 0);
		else if (settings_sub_index_ == 1) settings_.bgm_enabled = (v == 0);
		else settings_.avls_enabled = (v == 0);
		break;
	case SettingsKind::Video:
		if (settings_sub_index_ == 0) settings_.aspect_mode = v;
		else seq_play_enabled_ = (v == 0);
		break;
	case SettingsKind::Photo:
		if (settings_sub_index_ == 0) settings_.slideshow_speed = v;
		else settings_.slideshow_effect = v;
		break;
	case SettingsKind::DateTime:
		if (settings_sub_index_ == 0) settings_.date_format = v;
		else settings_.time_24h = (v == 0);
		break;
	case SettingsKind::PowerSave:
		if (settings_sub_index_ == 0) settings_.keep_screen_mode = v;
		else wlan_power_save_ = (v == 0);
		break;
	case SettingsKind::Security:
		settings_.parental_level = v;
		break;
	case SettingsKind::Network:
		if (settings_sub_index_ == 1) settings_.adhoc_channel = v;
		break;
	case SettingsKind::Rss:
		settings_.rss_auto_update = (v == 0);
		break;
	case SettingsKind::Display:
		if (settings_sub_index_ == 0) settings_.aspect_mode = v;
		else settings_.touch_controls_enabled = (v == 0);
		break;
	case SettingsKind::Bluetooth:
		break;
	}
}

void XmbRenderer::DrawSettingsSubPage() {
	if (!settings_sub_active_) return;
	const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
	auto arrow = textures_.find("arrow_l");
	if (arrow != textures_.end()) {
		const float shadow_color[4] = {0.0f, 0.0f, 0.0f, 0.48f};
		DrawTexture(arrow->second, 11.0f, 137.0f, 1.0f, shadow_color);
		DrawTexture(arrow->second, 10.0f, 136.0f, 1.0f, white);
	}

	const char *cat_tex_name = "item_theme_settings";
	if (settings_kind_ == SettingsKind::Video) cat_tex_name = "item_video_settings";
	else if (settings_kind_ == SettingsKind::Photo) cat_tex_name = "item_photo_settings";
	else if (settings_kind_ == SettingsKind::System) cat_tex_name = "item_system_settings";
	else if (settings_kind_ == SettingsKind::DateTime) cat_tex_name = "item_date_settings";
	else if (settings_kind_ == SettingsKind::PowerSave) cat_tex_name = "item_power_settings";
	else if (settings_kind_ == SettingsKind::Sound) cat_tex_name = "item_sound_settings";
	else if (settings_kind_ == SettingsKind::Security) cat_tex_name = "item_security_settings";
	else if (settings_kind_ == SettingsKind::Network) cat_tex_name = "item_network_settings";
	else if (settings_kind_ == SettingsKind::Rss) cat_tex_name = "item_rss_settings";
	else if (settings_kind_ == SettingsKind::Display) cat_tex_name = "item_display_settings";
	else if (settings_kind_ == SettingsKind::Bluetooth) cat_tex_name = "item_bluetooth_settings";
	auto cat_tex = textures_.find(cat_tex_name);
	if (cat_tex != textures_.end() && cat_tex->second.width > 0 && cat_tex->second.height > 0) {
		const float cat_color[4] = {1.0f, 1.0f, 1.0f, 0.48f};
		const float scale = std::min(1.0f, std::min(28.0f / (float)cat_tex->second.width, 28.0f / (float)cat_tex->second.height));
		DrawTexture(cat_tex->second, 38.0f, 136.0f, scale, cat_color);
	}

	struct SettingRow {
		std::string label;
		std::string value;
	};
	std::vector<SettingRow> rows;
	const std::string opt_orig = TextOr("xmb_opt_original", "Original");
	const std::string opt_classic = TextOr("xmb_opt_classic", "Classic");
	const std::string opt_cookie = TextOr("xmb_theme_cookie", "Cookie");
	const std::string opt_piano = TextOr("xmb_theme_piano", "Piano Black");
	const std::string opt_on = TextOr("xmb_opt_on", "On");
	const std::string opt_off = TextOr("xmb_opt_off", "Off");
	const std::string opt_aspect_auto = TextOr("xmb_opt_aspect_auto", "Auto");
	const std::string opt_16_9 = TextOr("xmb_opt_16_9", "16:9");
	const std::string opt_4_3 = TextOr("xmb_opt_4_3", "4:3");
	auto aspect_label = [&](int mode) -> std::string {
		if (mode == 1) return opt_16_9;
		if (mode == 2) return opt_4_3;
		return opt_aspect_auto;
	};

	const std::vector<std::string> theme_names = {
		opt_orig,
		opt_classic,
		opt_cookie,
		opt_piano,
		TextOr("xmb_theme_ceramic", "Ceramic White"),
		TextOr("xmb_theme_radiant_red", "Radiant Red"),
		TextOr("xmb_theme_vibrant_blue", "Vibrant Blue"),
		TextOr("xmb_theme_spirited_green", "Spirited Green"),
		TextOr("xmb_theme_bright_yellow", "Bright Yellow"),
		TextOr("xmb_theme_blossom_pink", "Blossom Pink"),
		TextOr("xmb_theme_turquoise", "Turquoise"),
		TextOr("xmb_theme_lilac", "Lilac Purple"),
	};

	auto theme_swatch = [&](int t_idx) -> std::array<float, 3> {
		switch (t_idx) {
		case 0: return ColorSwatchForChoice(0);
		case 1: return {kClassicBlueTint[0], kClassicBlueTint[1], kClassicBlueTint[2]};
		case 2: return {0.78f, 0.56f, 0.34f};
		case 3: return {kExtColorSwatches[15][0], kExtColorSwatches[15][1], kExtColorSwatches[15][2]};
		case 4: return {kExtColorSwatches[21][0], kExtColorSwatches[21][1], kExtColorSwatches[21][2]};
		case 5: return {kExtColorSwatches[2][0], kExtColorSwatches[2][1], kExtColorSwatches[2][2]};
		case 6: return {kExtColorSwatches[12][0], kExtColorSwatches[12][1], kExtColorSwatches[12][2]};
		case 7: return {kExtColorSwatches[17][0], kExtColorSwatches[17][1], kExtColorSwatches[17][2]};
		case 8: return {kExtColorSwatches[19][0], kExtColorSwatches[19][1], kExtColorSwatches[19][2]};
		case 9: return {kExtColorSwatches[0][0], kExtColorSwatches[0][1], kExtColorSwatches[0][2]};
		case 10: return {kExtColorSwatches[14][0], kExtColorSwatches[14][1], kExtColorSwatches[14][2]};
		case 11: return {kExtColorSwatches[7][0], kExtColorSwatches[7][1], kExtColorSwatches[7][2]};
		default: return ColorSwatchForChoice(0);
		}
	};

	auto color_label = [&](int idx) -> std::string {
		if (idx == 0) return TextOr("xmb_opt_month_auto", "Month (Auto)");
		if (idx >= 1 && idx <= 12) return TextOr(kMonthKeys[idx - 1], kMonthFallbacks[idx - 1]);
		if (idx >= 13 && idx <= 34) return TextOr(kExtColorKeys[idx - 13], kExtColorFallbacks[idx - 13]);
		return TextOr("xmb_opt_month_auto", "Month (Auto)");
	};

	if (settings_kind_ == SettingsKind::Theme) {
		const int ts = std::clamp(settings_.theme_style, 0, (int)theme_names.size() - 1);
		const std::string &theme_val = theme_names[ts];
		std::string bg_val = settings_.bg_mode == 0 ? opt_orig : (settings_.bg_mode == 1 ? opt_classic : opt_off);
		std::string wave_val = opt_orig;
		if (settings_.wave_mode == 1) wave_val = TextOr("xmb_opt_slow", "Slow");
		else if (settings_.wave_mode == 2) wave_val = TextOr("xmb_opt_static", "Static");
		else if (settings_.wave_mode == 3) wave_val = opt_off;
		rows = {
			{ TextOr("xmb_setting_theme", "Theme"), theme_val },
			{ TextOr("xmb_setting_color", "Color"), color_label(settings_.color_index) },
			{ TextOr("xmb_setting_background", "Background"), bg_val },
			{ TextOr("xmb_setting_wave", "Wave Animation"), wave_val },
		};
	} else if (settings_kind_ == SettingsKind::System) {
		rows = {
			{ TextOr("xmb_setting_sysinfo", "System Information"), "" },
			{ TextOr("xmb_setting_confirm_btn", "Confirmation Button"),
			  settings_.swap_confirm ? TextOr("xmb_btn_circle_lbl", "O Button")
			                         : TextOr("xmb_btn_cross_lbl", "X Button") },
			{ TextOr("xmb_setting_restore", "Restore Default Settings"), "" },
		};
	} else if (settings_kind_ == SettingsKind::Sound) {
		rows = {
			{ TextOr("xmb_setting_keytone", "Key Tone"), settings_.key_tone ? opt_on : opt_off },
			{ TextOr("xmb_setting_bgm", "Game Preview Audio"), settings_.bgm_enabled ? opt_on : opt_off },
			{ TextOr("xmb_setting_avls", "AVLS"), settings_.avls_enabled ? opt_on : opt_off },
		};
	} else if (settings_kind_ == SettingsKind::Video) {
		rows = {
			{ TextOr("xmb_setting_aspect", "Aspect Ratio"), aspect_label(settings_.aspect_mode) },
			{ TextOr("xmb_setting_seq_play", "Sequential Playback"), seq_play_enabled_ ? opt_on : opt_off },
		};
	} else if (settings_kind_ == SettingsKind::Photo) {
		std::string spd = TextOr("xmb_opt_normal", "Normal");
		if (settings_.slideshow_speed == 1) spd = TextOr("xmb_opt_slow", "Slow");
		else if (settings_.slideshow_speed == 2) spd = TextOr("xmb_opt_fast", "Fast");
		std::string eff = TextOr("xmb_opt_slide", "Slide");
		if (settings_.slideshow_effect == 1) eff = TextOr("xmb_opt_fade", "Fade");
		else if (settings_.slideshow_effect == 2) eff = opt_off;
		rows = {
			{ TextOr("xmb_setting_ss_speed", "Slideshow Speed"), spd },
			{ TextOr("xmb_setting_ss_effect", "Slideshow Effect"), eff },
		};
	} else if (settings_kind_ == SettingsKind::DateTime) {
		const char *df = settings_.date_format == 1 ? "MM/DD/YYYY" : (settings_.date_format == 2 ? "YYYY/MM/DD" : "DD/MM/YYYY");
		rows = {
			{ TextOr("xmb_setting_date_fmt", "Date Format"), df },
			{ TextOr("xmb_setting_time_fmt", "Time Format"),
			  settings_.time_24h ? TextOr("xmb_opt_24h", "24-Hour Clock") : TextOr("xmb_opt_12h", "12-Hour Clock") },
		};
	} else if (settings_kind_ == SettingsKind::PowerSave) {
		std::string bl = opt_off;
		if (settings_.keep_screen_mode == 1) bl = TextOr("xmb_opt_2min", "2 Minutes");
		else if (settings_.keep_screen_mode == 2) bl = TextOr("xmb_opt_5min", "5 Minutes");
		rows = {
			{ TextOr("xmb_setting_backlight", "Backlight Auto-Off"), bl },
			{ TextOr("xmb_setting_wlan_ps", "WLAN Power Save"), wlan_power_save_ ? opt_on : opt_off },
		};
	} else if (settings_kind_ == SettingsKind::Security) {
		const std::string pval = settings_.parental_level == 0
		                             ? opt_off
		                             : (TextOr("xmb_level_prefix", "Level") + " " + std::to_string(settings_.parental_level));
		rows = {
			{ TextOr("xmb_setting_parental", "Parental Control"), pval },
		};
	} else if (settings_kind_ == SettingsKind::Network) {
		const std::string ch = settings_.adhoc_channel == 0
		                           ? TextOr("xmb_opt_auto", "Automatic")
		                           : (TextOr("xmb_ch_prefix", "Ch") + " " +
		                              (settings_.adhoc_channel == 1 ? "1" : (settings_.adhoc_channel == 2 ? "6" : "11")));
		rows = {
			{ TextOr("xmb_setting_net_status", "Internet Connection"),
			  network_connected_ ? TextOr("xmb_net_connected", "Connected")
			                     : TextOr("xmb_net_disconnected", "Not Connected") },
			{ TextOr("xmb_setting_adhoc_ch", "Ad Hoc Channel"), ch },
		};
	} else if (settings_kind_ == SettingsKind::Rss) {
		rows = {
			{ TextOr("xmb_setting_rss_auto", "Auto-Update"), settings_.rss_auto_update ? opt_on : opt_off },
		};
	} else if (settings_kind_ == SettingsKind::Display) {
		rows = {
			{ TextOr("xmb_setting_aspect", "Aspect Ratio"), aspect_label(settings_.aspect_mode) },
			{ TextOr("xmb_setting_touch_ctrl", "Touch Controls"), settings_.touch_controls_enabled ? opt_on : opt_off },
		};
	} else if (settings_kind_ == SettingsKind::Bluetooth) {
		const std::string bt_state = bluetooth_enabled_
		                                 ? (bluetooth_connected_ ? TextOr("xmb_bt_connected", "Connected") : opt_on)
		                                 : opt_off;
		rows = {
			{ "Bluetooth\u00ae", bt_state },
			{ TextOr("xmb_setting_bt_manage", "Manage Bluetooth\u00ae Devices"), "" },
		};
	}

	if (font_loaded_) {
		const float picker_x = screen_width_ - 138.0f;
		const float row_w = settings_picker_open_ ? std::max(140.0f, picker_x - 77.0f) : std::max(220.0f, screen_width_ - 112.0f);
		const float val_right = settings_picker_open_ ? (picker_x - 14.0f) : (screen_width_ - 52.0f);
		for (int i = 0; i < (int)rows.size(); ++i) {
			const float y = kSelectedItemY + ((float)i - settings_sub_scroll_) * 48.0f;
			if (y < 24.0f || y > kScreenHeight - 24.0f) continue;
			const float focus = 1.0f - std::min(1.0f, std::fabs((float)i - settings_sub_scroll_));
			const float alpha = settings_picker_open_ && i != settings_sub_index_ ? 0.35f : (0.55f + 0.45f * focus);
			const float col[4] = {1.0f, 1.0f, 1.0f, alpha};
			if (focus > 0.6f) {
				const float glow_bg[4] = {1.0f, 1.0f, 1.0f, 0.16f * focus};
				DrawSolidQuad(70.0f, y - 16.0f, row_w, 26.0f, glow_bg);
			}
			DrawText(rows[i].label, 78.0f, y + 3.0f, 0.74f, col, true);
			if (!rows[i].value.empty()) {
				const float val_col[4] = {1.0f, 1.0f, 1.0f, alpha * 0.85f};
				const float vw = MeasureText(rows[i].value, 0.64f);
				float val_x = val_right - vw;
				if (!settings_picker_open_) {
					const float label_end = 78.0f + MeasureText(rows[i].label, 0.74f) + 12.0f;
					val_x = std::max(val_x, label_end);
				}
				DrawText(rows[i].value, val_x, y + 3.0f, 0.64f, val_col, true);
			}
		}
	}

	// Right-side PSP value picker panel when open
	if (settings_picker_open_ && font_loaded_) {
		std::vector<std::string> choices;
		const bool is_theme_picker = (settings_kind_ == SettingsKind::Theme && settings_sub_index_ == 0);
		const bool is_color_picker = (settings_kind_ == SettingsKind::Theme && settings_sub_index_ == 1);
		if (settings_kind_ == SettingsKind::Theme) {
			if (settings_sub_index_ == 0) choices = theme_names;
			else if (settings_sub_index_ == 1) {
				for (int c = 0; c <= 34; ++c) choices.push_back(color_label(c));
			} else if (settings_sub_index_ == 2) choices = {opt_orig, opt_classic, opt_off};
			else choices = {opt_orig, TextOr("xmb_opt_slow", "Slow"), TextOr("xmb_opt_static", "Static"), opt_off};
		} else if (settings_kind_ == SettingsKind::System && settings_sub_index_ == 1) {
			choices = {TextOr("xmb_btn_cross_lbl", "X Button"), TextOr("xmb_btn_circle_lbl", "O Button")};
		} else if (settings_kind_ == SettingsKind::Sound || settings_kind_ == SettingsKind::Rss ||
		           (settings_kind_ == SettingsKind::Video && settings_sub_index_ == 1) ||
		           (settings_kind_ == SettingsKind::Display && settings_sub_index_ == 1) ||
		           (settings_kind_ == SettingsKind::PowerSave && settings_sub_index_ == 1)) {
			choices = {opt_on, opt_off};
		} else if (settings_kind_ == SettingsKind::Video || settings_kind_ == SettingsKind::Display) {
			choices = {opt_aspect_auto, opt_16_9, opt_4_3};
		} else if (settings_kind_ == SettingsKind::Photo) {
			if (settings_sub_index_ == 0) {
				choices = {TextOr("xmb_opt_normal", "Normal"), TextOr("xmb_opt_slow", "Slow"), TextOr("xmb_opt_fast", "Fast")};
			} else {
				choices = {TextOr("xmb_opt_slide", "Slide"), TextOr("xmb_opt_fade", "Fade"), opt_off};
			}
		} else if (settings_kind_ == SettingsKind::DateTime) {
			if (settings_sub_index_ == 0) choices = {"DD/MM/YYYY", "MM/DD/YYYY", "YYYY/MM/DD"};
			else choices = {TextOr("xmb_opt_24h", "24-Hour Clock"), TextOr("xmb_opt_12h", "12-Hour Clock")};
		} else if (settings_kind_ == SettingsKind::PowerSave) {
			choices = {opt_off, TextOr("xmb_opt_2min", "2 Minutes"), TextOr("xmb_opt_5min", "5 Minutes")};
		} else if (settings_kind_ == SettingsKind::Security) {
			choices.push_back(opt_off);
			for (int lvl = 1; lvl <= 9; ++lvl) {
				choices.push_back(TextOr("xmb_level_prefix", "Level") + " " + std::to_string(lvl));
			}
		} else if (settings_kind_ == SettingsKind::Network && settings_sub_index_ == 1) {
			const std::string chp = TextOr("xmb_ch_prefix", "Ch") + " ";
			choices = {TextOr("xmb_opt_auto", "Automatic"), chp + "1", chp + "6", chp + "11"};
		}

		if (!choices.empty()) {
			const float panel_w = 138.0f;
			const float panel_x = screen_width_ - panel_w;
			const float bg_color[4] = {0.08f, 0.08f, 0.10f, 0.94f};
			DrawSolidQuad(panel_x, 0.0f, panel_w, kScreenHeight, bg_color);
			const float border_color[4] = {1.0f, 1.0f, 1.0f, 0.35f};
			DrawSolidQuad(panel_x, 0.0f, 1.5f, kScreenHeight, border_color);

			for (int i = 0; i < (int)choices.size(); ++i) {
				const float y = kSelectedItemY + (float)(i - settings_picker_index_) * 26.0f;
				if (y < 24.0f || y > kScreenHeight - 20.0f) continue;
				const bool sel = (i == settings_picker_index_);
				if (sel) {
					const float focus_col[4] = {1.0f, 1.0f, 1.0f, 0.22f};
					DrawSolidQuad(panel_x + 1.5f, y - 16.0f, panel_w - 1.5f, 22.0f, focus_col);
				}
				float text_x = panel_x + 12.0f;
				float text_scale = 0.65f;
				if (is_color_picker || is_theme_picker) {
					const auto sw = is_theme_picker ? theme_swatch(i) : ColorSwatchForChoice(i);
					const float sw_border[4] = {1.0f, 1.0f, 1.0f, sel ? 0.95f : 0.55f};
					const float sw_fill[4] = {sw[0], sw[1], sw[2], 1.0f};
					DrawSolidQuad(panel_x + 8.0f, y - 10.0f, 14.0f, 10.0f, sw_border);
					DrawSolidQuad(panel_x + 9.0f, y - 9.0f, 12.0f, 8.0f, sw_fill);
					text_x = panel_x + 28.0f;
					text_scale = 0.58f;
				}
				const float tcol[4] = {1.0f, 1.0f, 1.0f, sel ? 1.0f : 0.62f};
				DrawText(choices[i], text_x, y, text_scale, tcol, true);
			}
		}
	}
}

void XmbRenderer::DrawSystemInfoDialog() {
	if (!system_info_dialog_) return;
	const float box_w = std::min(380.0f, screen_width_ - 28.0f);
	const float box_x = (screen_width_ - box_w) * 0.5f;
	const float box_y = 38.0f;
	const float box_h = 196.0f;
	DrawDialogFrame(box_x, box_y, box_w, box_h);

	if (font_loaded_) {
		const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
		const float label_col[4] = {1.0f, 1.0f, 1.0f, 0.68f};
		const float border[4] = {1.0f, 1.0f, 1.0f, 0.30f};
		DrawText(TextOr("xmb_setting_sysinfo", "System Information"), box_x + 18.0f, box_y + 26.0f, 0.78f, white, true);
		DrawSolidQuad(box_x + 16.0f, box_y + 34.0f, box_w - 32.0f, 1.0f, border);

		auto row = [&](float y, const std::string &k, const std::string &v) {
			DrawText(k, box_x + 20.0f, y, 0.64f, label_col, true);
			const float val_x = std::max(box_x + 158.0f, box_x + 28.0f + MeasureText(k, 0.64f));
			const float max_val_w = (box_x + box_w - 18.0f) - val_x;
			float val_scale = 0.64f;
			const float vw = MeasureText(v, val_scale);
			if (vw > max_val_w && vw > 1.0f && max_val_w > 20.0f) {
				val_scale = std::max(0.46f, val_scale * (max_val_w / vw));
			}
			DrawText(v, val_x, y, val_scale, white, true);
		};
		row(box_y + 62.0f, TextOr("xmb_sysinfo_fw", "System Software"), "7.1 EmuCore Edition");
		row(box_y + 86.0f, TextOr("xmb_sysinfo_model", "Model"), "PSP\u2122 4000");
		row(box_y + 110.0f, TextOr("xmb_sysinfo_name", "Nickname"), device_name_);
		row(box_y + 134.0f, TextOr("msgshare_info_space", "Free Space"), free_space_text_);

		DrawDialogHints(box_x, box_y, box_w, box_h, false);
	}
}

void XmbRenderer::DrawNoticeDialog() {
	if (!info_notice_active_) return;
	const float box_w = std::min(396.0f, screen_width_ - 28.0f);
	const float box_x = (screen_width_ - box_w) * 0.5f;
	const float box_y = 40.0f;
	const float box_h = 192.0f;
	DrawDialogFrame(box_x, box_y, box_w, box_h);

	if (font_loaded_) {
		const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
		const float body_col[4] = {1.0f, 1.0f, 1.0f, 0.88f};
		const float border[4] = {1.0f, 1.0f, 1.0f, 0.30f};
		DrawText(info_notice_title_, box_x + 18.0f, box_y + 26.0f, 0.76f, white, true);
		DrawSolidQuad(box_x + 16.0f, box_y + 34.0f, box_w - 32.0f, 1.0f, border);

		float y = box_y + 58.0f;
		const float max_y = box_y + box_h - 32.0f;
		for (const auto &raw_line : info_notice_lines_) {
			const auto wrapped = WrapText(raw_line, 0.62f, box_w - 36.0f);
			for (const auto &line : wrapped) {
				if (y > max_y) break;
				DrawText(line, box_x + 18.0f, y, 0.62f, body_col, true);
				y += 20.0f;
			}
			if (y > max_y) break;
		}
		DrawDialogHints(box_x, box_y, box_w, box_h, false);
	}
}

void XmbRenderer::DrawOptionsMenu() {
	if (!options_menu_active_) {
		return;
	}
	const float panel_w = 130.0f;
	const float panel_x = screen_width_ - panel_w;
	const float bg_color[4] = {0.08f, 0.08f, 0.10f, 0.94f};
	DrawSolidQuad(panel_x, 0.0f, panel_w, kScreenHeight, bg_color);

	const float border_color[4] = {1.0f, 1.0f, 1.0f, 0.35f};
	DrawSolidQuad(panel_x, 0.0f, 1.5f, kScreenHeight, border_color);

	if (font_loaded_) {
		const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
		static const char *kKeys[] = {"msgshare_opt_load", "msgshare_opt_del", "msgshare_opt_info"};
		static const char *kLabels[] = {"Start", "Delete", "Information"};
		const float first_y = 136.0f - 30.0f;
		for (int i = 0; i < 3; ++i) {
			const float item_y = first_y + i * 30.0f;
			const std::string label = TextOr(kKeys[i], kLabels[i]);
			if (options_selected_ == i) {
				const float focus_color[4] = {1.0f, 1.0f, 1.0f, 0.18f};
				DrawSolidQuad(panel_x + 1.5f, item_y - 17.0f, panel_w - 1.5f, 24.0f, focus_color);
				DrawText(label, panel_x + 16.0f, item_y, 0.70f, white, true);
			} else {
				const float unsel_color[4] = {1.0f, 1.0f, 1.0f, 0.60f};
				DrawText(label, panel_x + 16.0f, item_y, 0.70f, unsel_color, true);
			}
		}
	}
}

void XmbRenderer::DrawDeleteDialog() {
	if (!delete_dialog_active_) {
		return;
	}
	DrawYesNoDialog(
		TextOr("msg_delete_confirm_1", "The data will be deleted."),
		TextOr("msg_delete_confirm_2", "Are you sure you want to continue?"),
		delete_dialog_selected_);
}

void XmbRenderer::DrawInfoDialog() {
	if (!info_dialog_active_) {
		return;
	}
	const float box_w = std::min(388.0f, screen_width_ - 28.0f);
	const float box_x = (screen_width_ - box_w) * 0.5f;
	const float box_y = 26.0f;
	const float box_h = 220.0f;
	DrawDialogFrame(box_x, box_y, box_w, box_h);

	if (font_loaded_) {
		const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
		const float label_color[4] = {1.0f, 1.0f, 1.0f, 0.68f};
		const float border[4] = {1.0f, 1.0f, 1.0f, 0.30f};
		DrawText(TextOr("msgshare_opt_info", "Information"), box_x + 18.0f, box_y + 24.0f, 0.78f, white, true);
		DrawSolidQuad(box_x + 16.0f, box_y + 32.0f, box_w - 32.0f, 1.0f, border);

		auto draw_row = [&](float y, const std::string &lbl, const std::string &val) {
			const std::string display = val.empty() ? TextOr("msgshare_info_none", "-") : val;
			DrawText(lbl, box_x + 18.0f, y, 0.58f, label_color, true);
			const float val_x = std::max(box_x + 148.0f, box_x + 28.0f + MeasureText(lbl, 0.58f));
			const float max_val_w = (box_x + box_w - 18.0f) - val_x;
			float val_scale = 0.58f;
			const float vw = MeasureText(display, val_scale);
			if (vw > max_val_w && vw > 1.0f && max_val_w > 20.0f) {
				val_scale = std::max(0.44f, val_scale * (max_val_w / vw));
			}
			DrawText(display, val_x, y, val_scale, white, true);
		};

		if (saved_data_page_ && save_page_index_ >= 0 && save_page_index_ < (int)saves_.size()) {
			const auto &s = saves_[save_page_index_];
			draw_row(box_y + 58.0f, TextOr("msgshare_info_title", "Title"), s.game_title);
			draw_row(box_y + 82.0f, TextOr("xmb_saves_count_lbl", "Saved Data"), s.slot_name);
			draw_row(box_y + 106.0f, TextOr("msgshare_info_date", "Updated"), s.date_time);
			draw_row(box_y + 130.0f, TextOr("msgshare_info_size", "Size"), s.size_str);
		} else if (memory_stick_page_ && game_page_index_ >= 0 && game_page_index_ < (int)games_.size()) {
			const auto &g = games_[game_page_index_];
			draw_row(box_y + 54.0f, TextOr("msgshare_info_title", "Title"), g.title);
			draw_row(box_y + 75.0f, TextOr("msgshare_info_size", "Size"), g.size_str);
			draw_row(box_y + 96.0f, TextOr("xmb_info_version", "Version"), g.version);
			draw_row(box_y + 117.0f, TextOr("xmb_info_sysver", "Required Version"), g.system_version);
			draw_row(box_y + 138.0f, TextOr("xmb_info_discid", "Disc ID"), g.serial);
			draw_row(box_y + 159.0f, TextOr("xmb_info_region", "Region"), g.region);
			draw_row(box_y + 180.0f, TextOr("msgshare_info_date", "Updated"), g.date_str);
		}

		DrawDialogHints(box_x, box_y, box_w, box_h, false);
	}
}

void XmbRenderer::DrawTouchTutorialDialog() {
	if (!touch_tutorial_active_) {
		return;
	}
	const float box_w = std::min(416.0f, screen_width_ - 24.0f);
	const float box_x = (screen_width_ - box_w) * 0.5f;
	const float box_y = 26.0f;
	const float box_h = 220.0f;
	DrawDialogFrame(box_x, box_y, box_w, box_h);

	if (!font_loaded_) {
		return;
	}
	const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
	const float body_col[4] = {1.0f, 1.0f, 1.0f, 0.92f};
	const float sub_col[4] = {1.0f, 1.0f, 1.0f, 0.78f};
	const float border[4] = {1.0f, 1.0f, 1.0f, 0.30f};

	DrawText(TextOr("xmb_touch_guide_title", "Touch & Gesture Controls"), box_x + 18.0f, box_y + 24.0f, 0.76f, white, true);
	DrawSolidQuad(box_x + 16.0f, box_y + 32.0f, box_w - 32.0f, 1.0f, border);

	const char *confirm_key = settings_.swap_confirm ? "btn_circle" : "btn_cross";
	const char *back_key = settings_.swap_confirm ? "btn_cross" : "btn_circle";
	auto icon_confirm = textures_.find(confirm_key);
	auto icon_back = textures_.find(back_key);
	auto icon_tri = textures_.find("btn_triangle");
	auto arrow_l = textures_.find("arrow_l");
	auto arrow_r = textures_.find("arrow_r");

	auto draw_guide_row = [&](float y, const Texture *icon1, const Texture *icon2, const std::string &text) {
		float ix = box_x + 24.0f;
		if (icon1 != nullptr) {
			DrawTexture(*icon1, ix, y - 5.0f, 1.0f, white);
			ix += 14.0f;
		}
		if (icon2 != nullptr) {
			DrawTexture(*icon2, ix, y - 5.0f, 1.0f, white);
		}
		const float text_x = box_x + 48.0f;
		const float max_w = box_w - 64.0f;
		float scale = 0.60f;
		const float measured = MeasureText(text, scale);
		if (measured > max_w && measured > 1.0f) {
			scale = std::max(0.46f, scale * (max_w / measured));
		}
		DrawText(text, text_x, y, scale, body_col, true);
	};

	draw_guide_row(box_y + 62.0f,
	               arrow_l != textures_.end() ? &arrow_l->second : nullptr,
	               arrow_r != textures_.end() ? &arrow_r->second : nullptr,
	               TextOr("xmb_touch_guide_1", "Swipe Up / Down / Left / Right: Navigate XMB"));
	draw_guide_row(box_y + 94.0f,
	               icon_confirm != textures_.end() ? &icon_confirm->second : nullptr,
	               nullptr,
	               TextOr("xmb_touch_guide_2", "Tap: Confirm or start game"));
	draw_guide_row(box_y + 126.0f,
	               icon_tri != textures_.end() ? &icon_tri->second : nullptr,
	               nullptr,
	               TextOr("xmb_touch_guide_3", "Long Press (or \u25b3): Open Options menu"));
	draw_guide_row(box_y + 158.0f,
	               icon_back != textures_.end() ? &icon_back->second : nullptr,
	               nullptr,
	               TextOr("xmb_touch_guide_4", "2-Finger Tap (or \u25cb): Back or exit PSP mode"));

	DrawSolidQuad(box_x + 16.0f, box_y + box_h - 30.0f, box_w - 32.0f, 1.0f, border);
	const std::string skip_lbl = TextOr("xmb_touch_guide_skip", "Tap to Close");
	const float skip_scale = 0.58f;
	const float skip_w = MeasureText(skip_lbl, skip_scale);
	float skip_x = box_x + box_w - skip_w - 16.0f;
	if (icon_confirm != textures_.end()) {
		DrawTexture(icon_confirm->second, skip_x - 10.0f, box_y + box_h - 14.0f, 1.0f, sub_col);
	}
	DrawText(skip_lbl, skip_x, box_y + box_h - 10.0f, skip_scale, white, true);
}

void XmbRenderer::DrawTouchControlsBar() {
	if (!touch_mode_active_ || !settings_.touch_controls_enabled || !font_loaded_ || launching_ || opening_t_ < 0.72f) {
		return;
	}
	if (system_info_dialog_ || info_notice_active_ || info_dialog_active_ ||
	    exit_dialog_active_ || delete_dialog_active_ || restore_confirm_active_ ||
	    browser_confirm_active_ || touch_tutorial_active_) {
		return;
	}

	const char *confirm_key = settings_.swap_confirm ? "btn_circle" : "btn_cross";
	const char *back_key = settings_.swap_confirm ? "btn_cross" : "btn_circle";
	auto confirm_icon = textures_.find(confirm_key);
	auto back_icon = textures_.find(back_key);
	auto tri_icon = textures_.find("btn_triangle");

	const std::string enter_lbl = TextOr("xmb_hint_enter", "Enter");
	const std::string opt_lbl = TextOr("xmb_hint_options", "Options");
	const std::string back_lbl = TextOr("xmb_hint_back", "Back");
	const bool show_options = (memory_stick_page_ && !games_.empty()) || (saved_data_page_ && !saves_.empty());

	const float scale = 0.54f;
	const float col[4] = {1.0f, 1.0f, 1.0f, 0.78f};
	const float hint_y = kScreenHeight - 10.0f;

	float cur_x = screen_width_ - 14.0f;
	// 1. Back (Circle) on far right
	const float back_w = MeasureText(back_lbl, scale);
	cur_x -= back_w;
	DrawText(back_lbl, cur_x, hint_y, scale, col, true);
	if (back_icon != textures_.end()) {
		cur_x -= 10.0f;
		DrawTexture(back_icon->second, cur_x, hint_y - 4.0f, 1.0f, col);
	}
	cur_x -= 16.0f;

	// 2. Options (Triangle) when inside Games or Saved Data
	if (show_options) {
		const float opt_w = MeasureText(opt_lbl, scale);
		cur_x -= opt_w;
		DrawText(opt_lbl, cur_x, hint_y, scale, col, true);
		if (tri_icon != textures_.end()) {
			cur_x -= 10.0f;
			DrawTexture(tri_icon->second, cur_x, hint_y - 4.0f, 1.0f, col);
		}
		cur_x -= 16.0f;
	}

	// 3. Enter (Cross)
	const float enter_w = MeasureText(enter_lbl, scale);
	cur_x -= enter_w;
	DrawText(enter_lbl, cur_x, hint_y, scale, col, true);
	if (confirm_icon != textures_.end()) {
		cur_x -= 10.0f;
		DrawTexture(confirm_icon->second, cur_x, hint_y - 4.0f, 1.0f, col);
	}
}

void XmbRenderer::DrawGameIcon(const GameInfo &game, float center_x, float center_y,
                               float width, float height, const float color[4]) {
	const Texture *tex = nullptr;
	if (!game.icon_path.empty()) {
		tex = TextureForPath(game.icon_path);
	}
	if (tex == nullptr) {
		auto default_icon = textures_.find("item_game");
		if (default_icon != textures_.end()) {
			tex = &default_icon->second;
		}
	}
	if (tex != nullptr && tex->width > 0 && tex->height > 0) {
		const float scale = std::min(width / (float)tex->width, height / (float)tex->height);
		DrawTexture(*tex, center_x, center_y, scale, color);
	}
}

void XmbRenderer::KeyPressed(Key key) {
	bool do_exit = false;
	int do_game_focused = -999;
	int do_save_selected = -1;
	int do_delete_save = -1;
	int do_delete_game = -1;
	bool do_settings_changed = false;
	std::string do_open_browser;
	XmbSettings new_settings;

	{
		std::lock_guard<std::recursive_mutex> lock(mutex_);

		if (launching_) {
			return;
		}
		if (opening_t_ < 0.72f) {
			opening_t_ = 0.72f;
		}

		// 0. Touch & Gesture Controls tutorial dialog (any button skips/closes)
		if (touch_tutorial_active_) {
			PlaySound(1);
			touch_tutorial_active_ = false;
			goto dispatch_callbacks;
		}

		// 1. System Info dialog
		if (system_info_dialog_) {
			if (IsBackKey(key) || IsConfirmKey(key)) {
				PlaySound(2);
				system_info_dialog_ = false;
			}
			goto dispatch_callbacks;
		}

		// 1b. Notice dialog (System Update, USB, Firmware notices, Information Board, etc.)
		if (info_notice_active_) {
			if (IsBackKey(key) || IsConfirmKey(key)) {
				PlaySound(2);
				info_notice_active_ = false;
			}
			goto dispatch_callbacks;
		}

		// 1c. Restore Default Settings confirmation
		if (restore_confirm_active_) {
			switch (key) {
			case Key::Left:
				if (restore_confirm_selected_ != 0) {
					restore_confirm_selected_ = 0;
					PlaySound(0);
				}
				break;
			case Key::Right:
				if (restore_confirm_selected_ != 1) {
					restore_confirm_selected_ = 1;
					PlaySound(0);
				}
				break;
			case Key::Confirm:
				if (restore_confirm_selected_ == 0) {
					PlaySound(1);
					restore_confirm_active_ = false;
					settings_ = XmbSettings{};
					seq_play_enabled_ = true;
					wlan_power_save_ = true;
					theme_dirty_ = true;
					new_settings = settings_;
					do_settings_changed = true;
				} else {
					PlaySound(2);
					restore_confirm_active_ = false;
				}
				break;
			case Key::Back:
				PlaySound(2);
				restore_confirm_active_ = false;
				break;
			default:
				break;
			}
			goto dispatch_callbacks;
		}

		// 1d. Browser launch confirmation
		if (browser_confirm_active_) {
			switch (key) {
			case Key::Left:
				if (browser_confirm_selected_ != 0) {
					browser_confirm_selected_ = 0;
					PlaySound(0);
				}
				break;
			case Key::Right:
				if (browser_confirm_selected_ != 1) {
					browser_confirm_selected_ = 1;
					PlaySound(0);
				}
				break;
			case Key::Confirm:
				if (browser_confirm_selected_ == 0) {
					PlaySound(1);
					browser_confirm_active_ = false;
					do_open_browser = pending_browser_url_;
				} else {
					PlaySound(2);
					browser_confirm_active_ = false;
				}
				break;
			case Key::Back:
				PlaySound(2);
				browser_confirm_active_ = false;
				break;
			default:
				break;
			}
			goto dispatch_callbacks;
		}

		// 2. Delete confirmation dialog
		if (delete_dialog_active_) {
			switch (key) {
			case Key::Left:
				if (delete_dialog_selected_ != 0) {
					delete_dialog_selected_ = 0;
					PlaySound(0);
				}
				break;
			case Key::Right:
				if (delete_dialog_selected_ != 1) {
					delete_dialog_selected_ = 1;
					PlaySound(0);
				}
				break;
			case Key::Confirm:
				if (delete_dialog_selected_ == 0) {
					PlaySound(1);
					delete_dialog_active_ = false;
					options_menu_active_ = false;
					if (saved_data_page_ && save_page_index_ >= 0 && save_page_index_ < (int)saves_.size()) {
						do_delete_save = save_page_index_;
					} else if (memory_stick_page_ && game_page_index_ >= 0 && game_page_index_ < (int)games_.size()) {
						do_delete_game = game_page_index_;
					}
				} else {
					PlaySound(2);
					delete_dialog_active_ = false;
				}
				break;
			case Key::Back:
				PlaySound(2);
				delete_dialog_active_ = false;
				break;
			default:
				break;
			}
			goto dispatch_callbacks;
		}

		// 3. Information dialog
		if (info_dialog_active_) {
			if (IsBackKey(key) || IsConfirmKey(key)) {
				PlaySound(2);
				info_dialog_active_ = false;
			}
			goto dispatch_callbacks;
		}

		// 4. Options menu
		if (options_menu_active_) {
			switch (key) {
			case Key::Up:
				if (options_selected_ > 0) {
					options_selected_ -= 1;
					PlaySound(0);
				}
				break;
			case Key::Down:
				if (options_selected_ < 2) {
					options_selected_ += 1;
					PlaySound(0);
				}
				break;
			case Key::Options:
			case Key::Back:
				options_menu_active_ = false;
				PlaySound(2);
				break;
			case Key::Confirm:
				if (options_selected_ == 0) {
					options_menu_active_ = false;
					if (memory_stick_page_ && game_page_index_ >= 0 && game_page_index_ < (int)games_.size()) {
						PlaySound(8);
						launching_ = true;
						launch_dispatched_ = false;
						launch_t_ = 0.0f;
						launching_save_index_ = -1;
					} else if (saved_data_page_ && save_page_index_ >= 0 && save_page_index_ < (int)saves_.size()) {
						PlaySound(8);
						launching_ = true;
						launch_dispatched_ = false;
						launch_t_ = 0.0f;
						launching_save_index_ = save_page_index_;
					}
				} else if (options_selected_ == 1) {
					options_menu_active_ = false;
					delete_dialog_active_ = true;
					delete_dialog_selected_ = 1;
					PlaySound(0);
				} else if (options_selected_ == 2) {
					options_menu_active_ = false;
					info_dialog_active_ = true;
					PlaySound(1);
				}
				break;
			default:
				break;
			}
			goto dispatch_callbacks;
		}

		// 5. Exit PSP dialog
		if (exit_dialog_active_) {
			switch (key) {
			case Key::Left:
				if (exit_dialog_selected_ != 0) {
					exit_dialog_selected_ = 0;
					PlaySound(0);
				}
				break;
			case Key::Right:
				if (exit_dialog_selected_ != 1) {
					exit_dialog_selected_ = 1;
					PlaySound(0);
				}
				break;
			case Key::Confirm:
				if (exit_dialog_selected_ == 0) {
					PlaySound(1);
					exit_dialog_active_ = false;
					do_exit = true;
				} else {
					PlaySound(2);
					exit_dialog_active_ = false;
				}
				break;
			case Key::Back:
				PlaySound(2);
				exit_dialog_active_ = false;
				break;
			default:
				break;
			}
			goto dispatch_callbacks;
		}

		// 6. Settings sub-menu
		if (settings_sub_active_) {
			const int row_count = SettingsRowCount();
			if (settings_picker_open_) {
				const int max_c = SettingsPickerCount();
				switch (key) {
				case Key::Up:
					if (settings_picker_index_ > 0) {
						settings_picker_index_ -= 1;
						PlaySound(0);
					}
					break;
				case Key::Down:
					if (settings_picker_index_ + 1 < max_c) {
						settings_picker_index_ += 1;
						PlaySound(0);
					}
					break;
				case Key::Confirm:
					ApplySettingsPickerChoice(settings_picker_index_);
					settings_picker_open_ = false;
					new_settings = settings_;
					do_settings_changed = true;
					PlaySound(1);
					break;
				case Key::Back:
				case Key::Left:
					settings_picker_open_ = false;
					PlaySound(2);
					break;
				default:
					break;
				}
			} else {
				switch (key) {
				case Key::Up:
					if (settings_sub_index_ > 0) {
						settings_sub_index_ -= 1;
						PlaySound(0);
					}
					break;
				case Key::Down:
					if (settings_sub_index_ + 1 < row_count) {
						settings_sub_index_ += 1;
						PlaySound(0);
					}
					break;
				case Key::Confirm:
				case Key::Right:
					if (settings_kind_ == SettingsKind::System && settings_sub_index_ == 0) {
						system_info_dialog_ = true;
						PlaySound(1);
					} else if (settings_kind_ == SettingsKind::System && settings_sub_index_ == 2) {
						restore_confirm_active_ = true;
						restore_confirm_selected_ = 1;
						PlaySound(0);
					} else if (settings_kind_ == SettingsKind::Bluetooth) {
						OpenNoticeDialog(
							TextOr("xmb_setting_bt_devices", "Bluetooth\u00ae Device Settings"),
							{
								bluetooth_enabled_
									? TextOr("xmb_notice_bt_on", "Bluetooth\u00ae is enabled. Paired controllers and audio devices are active.")
									: TextOr("xmb_notice_bt_off", "Bluetooth\u00ae is turned off on this device."),
							});
						PlaySound(1);
					} else if (SettingsPickerCount() > 0) {
						settings_picker_open_ = true;
						settings_picker_index_ = SettingsCurrentValue();
						PlaySound(1);
					} else {
						PlaySound(1);
					}
					break;
				case Key::Back:
				case Key::Left:
					settings_sub_active_ = false;
					PlaySound(2);
					break;
				default:
					break;
				}
			}
			goto dispatch_callbacks;
		}

		// 7. Empty Media sub-page
		if (empty_media_kind_ != EmptyMediaKind::None) {
			if (IsBackKey(key) || key == Key::Left) {
				empty_media_kind_ = EmptyMediaKind::None;
				PlaySound(2);
			}
			goto dispatch_callbacks;
		}

		// 8. Triangle (Options) toggle
		if (key == Key::Options) {
			if ((memory_stick_page_ && !games_.empty()) || (saved_data_page_ && !saves_.empty())) {
				options_menu_active_ = !options_menu_active_;
				options_selected_ = 0;
				PlaySound(options_menu_active_ ? 4 : 2);
			}
			goto dispatch_callbacks;
		}

		// 9. Memory stick page (games list)
		if (memory_stick_page_) {
			switch (key) {
			case Key::Up:
				if (game_page_index_ > 0) {
					game_page_index_ -= 1;
					PlaySound(0);
					do_game_focused = game_page_index_;
				}
				break;
			case Key::Down:
				if (game_page_index_ + 1 < (int)games_.size()) {
					game_page_index_ += 1;
					PlaySound(0);
					do_game_focused = game_page_index_;
				}
				break;
			case Key::Confirm:
				if (game_page_index_ >= 0 && game_page_index_ < (int)games_.size()) {
					PlaySound(8);
					launching_ = true;
					launch_dispatched_ = false;
					launch_t_ = 0.0f;
					launching_save_index_ = -1;
				}
				break;
			case Key::Back:
			case Key::Left:
				memory_stick_page_ = false;
				PlaySound(2);
				do_game_focused = -1;
				break;
			default:
				break;
			}
			goto dispatch_callbacks;
		}

		// 10. Saved data page (quick saves list)
		if (saved_data_page_) {
			switch (key) {
			case Key::Up:
				if (save_page_index_ > 0) {
					save_page_index_ -= 1;
					PlaySound(0);
				}
				break;
			case Key::Down:
				if (save_page_index_ + 1 < (int)saves_.size()) {
					save_page_index_ += 1;
					PlaySound(0);
				}
				break;
			case Key::Confirm:
				if (save_page_index_ >= 0 && save_page_index_ < (int)saves_.size()) {
					options_menu_active_ = true;
					options_selected_ = 0;
					PlaySound(4);
				}
				break;
			case Key::Back:
			case Key::Left:
				saved_data_page_ = false;
				PlaySound(2);
				break;
			default:
				break;
			}
			goto dispatch_callbacks;
		}

		// 11. Main XMB menu
		switch (key) {
		case Key::Up:
			if (item_ > 0) {
				item_from_ = item_;
				item_ -= 1;
				item_t_ = 0.0f;
				PlaySound(0);
			}
			break;
		case Key::Down:
			if (item_ + 1 < (int)items_.size()) {
				item_from_ = item_;
				item_ += 1;
				item_t_ = 0.0f;
				PlaySound(0);
			}
			break;
		case Key::Left:
			if (category_ > 0) {
				category_selected_[category_] = item_;
				previous_items_ = items_;
				previous_item_ = item_;
				items_fade_t_ = 0.0f;
				category_from_ = category_;
				category_ -= 1;
				category_t_ = 0.0f;
				items_ = categories_[category_];
				item_ = category_selected_[category_];
				item_from_ = item_;
				item_t_ = 1.0f;
				PlaySound(3);
			}
			break;
		case Key::Right:
			if (category_ < 7) {
				category_selected_[category_] = item_;
				previous_items_ = items_;
				previous_item_ = item_;
				items_fade_t_ = 0.0f;
				category_from_ = category_;
				category_ += 1;
				category_t_ = 0.0f;
				items_ = categories_[category_];
				item_ = category_selected_[category_];
				item_from_ = item_;
				item_t_ = 1.0f;
				PlaySound(3);
			}
			break;
		case Key::Confirm:
			if (item_ >= 0 && item_ < (int)items_.size()) {
				const std::string &tex = items_[item_].texture_name;
				const std::string &title = items_[item_].title;
				if (category_ == 0) {
					if (tex == "item_update") {
						OpenNoticeDialog(title, {
							TextOr("xmb_notice_update_1", "The system software of your PSP\u2122 4000 system is version 7.1 EmuCore Edition"),
							TextOr("xmb_notice_update_2", "There is no need to update."),
						});
						PlaySound(1);
						break;
					}
					if (tex == "item_usb") {
						OpenNoticeDialog(title, {
							TextOr("xmb_usb_status", "Memory Stick\u2122 USB Mode"),
							TextOr("xmb_games_count_lbl", "Installed Games") + ": " + std::to_string(games_.size()),
							TextOr("msgshare_info_space", "Free Space") + ": " + free_space_text_,
							TextOr("xmb_sysinfo_name", "Nickname") + ": " + device_name_,
						});
						PlaySound(1);
						break;
					}
					static const struct {
						const char *tex_name;
						SettingsKind kind;
					} kSettingsMap[] = {
						{"item_video_settings", SettingsKind::Video},
						{"item_photo_settings", SettingsKind::Photo},
						{"item_system_settings", SettingsKind::System},
						{"item_theme_settings", SettingsKind::Theme},
						{"item_date_settings", SettingsKind::DateTime},
						{"item_power_settings", SettingsKind::PowerSave},
						{"item_sound_settings", SettingsKind::Sound},
						{"item_security_settings", SettingsKind::Security},
						{"item_network_settings", SettingsKind::Network},
						{"item_rss_settings", SettingsKind::Rss},
						{"item_display_settings", SettingsKind::Display},
						{"item_bluetooth_settings", SettingsKind::Bluetooth},
					};
					bool opened_sub = false;
					for (const auto &entry : kSettingsMap) {
						if (tex == entry.tex_name) {
							settings_sub_active_ = true;
							settings_kind_ = entry.kind;
							settings_sub_index_ = 0;
							settings_sub_scroll_ = 0.0f;
							settings_picker_open_ = false;
							PlaySound(1);
							opened_sub = true;
							break;
						}
					}
					if (opened_sub) break;
				} else if (category_ == 1) {
					if (tex == "item_comics") {
						empty_media_kind_ = EmptyMediaKind::Comics;
						PlaySound(1);
						break;
					}
					if (tex == "item_1seg") {
						OpenNoticeDialog(title, {TextOr("xmb_notice_1seg", "1Seg tuner is not connected.")});
						PlaySound(1);
						break;
					}
				} else if (category_ == 2) {
					if (tex == "item_camera") {
						OpenNoticeDialog(title, {TextOr("xmb_notice_camera", "Connect a camera compatible with the PSP\u2122 system.")});
						PlaySound(1);
						break;
					}
					if (tex == "item_memstick") {
						empty_media_kind_ = EmptyMediaKind::Photo;
						PlaySound(1);
						break;
					}
				} else if (category_ == 3) {
					if (tex == "item_sensme" || tex == "item_memstick") {
						empty_media_kind_ = EmptyMediaKind::Music;
						PlaySound(1);
						break;
					}
				} else if (category_ == 4) {
					if (tex == "item_memstick") {
						empty_media_kind_ = EmptyMediaKind::Video;
						PlaySound(1);
						break;
					}
				} else if (category_ == 5) {
					if (tex == "item_game_sharing") {
						OpenNoticeDialog(title, {
							TextOr("xmb_notice_gamesharing_1", "Searching for a nearby PSP\u2122 system..."),
							TextOr("xmb_notice_gamesharing_2", "No Game Sharing sessions found."),
						});
						PlaySound(1);
						break;
					}
					if (tex == "item_savedata") {
						saved_data_page_ = true;
						save_page_index_ = 0;
						save_scroll_ = 0.0f;
						PlaySound(1);
						break;
					}
					if (tex == "item_memstick") {
						memory_stick_page_ = true;
						game_page_index_ = 0;
						game_scroll_ = 0.0f;
						PlaySound(1);
						if (!games_.empty()) {
							do_game_focused = game_page_index_;
						}
						break;
					}
				} else if (category_ == 6) {
					if (tex == "item_manuals") {
						touch_tutorial_active_ = true;
						PlaySound(1);
						break;
					}
					if (tex == "item_locationfree") {
						OpenNoticeDialog(title, {TextOr("xmb_notice_locationfree", "No LocationFree\u2122 Base Station is registered.")});
						PlaySound(1);
						break;
					}
					if (tex == "item_remoteplay") {
						OpenNoticeDialog(title, {TextOr("xmb_notice_remoteplay", "Register a PS3\u2122 system to start Remote Play.")});
						PlaySound(1);
						break;
					}
					if (tex == "item_rss") {
						OpenNoticeDialog(title, {TextOr("xmb_notice_rss", "No RSS channels have been added.")});
						PlaySound(1);
						break;
					}
					if (tex == "item_internet_browser") {
						browser_confirm_active_ = true;
						browser_confirm_selected_ = 0;
						pending_browser_url_ = "https://www.playstation.com";
						PlaySound(1);
						break;
					}
					if (tex == "item_internet_search") {
						browser_confirm_active_ = true;
						browser_confirm_selected_ = 0;
						pending_browser_url_ = "https://www.google.com";
						PlaySound(1);
						break;
					}
					if (tex == "item_internet_radio") {
						OpenNoticeDialog(title, {TextOr("xmb_notice_radio", "No Internet Radio player stations saved.")});
						PlaySound(1);
						break;
					}
					if (tex == "item_skype") {
						OpenNoticeDialog(title, {TextOr("xmb_notice_skype", "Skype\u2122 service for PSP\u2122 has been discontinued.")});
						PlaySound(1);
						break;
					}
				} else if (category_ == 7) {
					if (tex == "item_psn_signup") {
						OpenNoticeDialog(title, {TextOr("xmb_notice_psn", "PlayStation\u00aeNetwork is unavailable in offline emulator mode.")});
						PlaySound(1);
						break;
					}
					if (tex == "item_psn_store") {
						OpenNoticeDialog(title, {TextOr("xmb_notice_ps_store", "PlayStation\u00aeStore cannot be accessed in offline emulator mode.")});
						PlaySound(1);
						break;
					}
					if (tex == "item_info_board") {
						OpenNoticeDialog(title, {
							"PSP\u2122 4000  |  7.1 EmuCore Edition",
							TextOr("xmb_games_count_lbl", "Installed Games") + ": " + std::to_string(games_.size()) +
							    "   " + TextOr("xmb_saves_count_lbl", "Saved Data") + ": " + std::to_string(saves_.size()),
							TextOr("msgshare_info_space", "Free Space") + ": " + free_space_text_,
							TextOr("xmb_setting_net_status", "Internet Connection") + ": " +
							    (network_connected_ ? TextOr("xmb_net_connected", "Connected")
							                        : TextOr("xmb_net_disconnected", "Not Connected")),
						});
						PlaySound(1);
						break;
					}
				}
			}
			PlaySound(5);
			break;
		case Key::Back:
			exit_dialog_active_ = true;
			exit_dialog_selected_ = 1;
			PlaySound(0);
			break;
		default:
			break;
		}
	}

dispatch_callbacks:
	if (do_exit) {
		ExitRequested();
	}
	if (do_game_focused != -999) {
		GameFocused(do_game_focused);
	}
	if (do_save_selected >= 0) {
		SaveSelected(do_save_selected);
	}
	if (do_delete_save >= 0) {
		DeleteSaveRequested(do_delete_save);
	}
	if (do_delete_game >= 0) {
		DeleteGameRequested(do_delete_game);
	}
	if (do_settings_changed) {
		SettingsChanged(new_settings);
	}
	if (!do_open_browser.empty()) {
		OpenBrowserRequested(do_open_browser);
	}
}

void XmbRenderer::DrawCategoryRow() {
	float intro_alpha = 1.0f;
	float intro_slide_x = 0.0f;
	float intro_scale = 1.0f;
	if (opening_t_ < 1.0f) {
		const float u = std::clamp((opening_t_ - 0.38f) / 0.62f, 0.0f, 1.0f);
		const float ease = 1.0f - (1.0f - u) * (1.0f - u) * (1.0f - u);
		intro_alpha = ease;
		intro_slide_x = (1.0f - ease) * 44.0f;
		intro_scale = 1.0f + (1.0f - ease) * 0.16f;
	} else if (resume_t_ < 1.0f) {
		const float u = std::clamp(resume_t_ / 0.85f, 0.0f, 1.0f);
		const float ease = 1.0f - (1.0f - u) * (1.0f - u);
		intro_alpha = ease;
		intro_slide_x = (1.0f - ease) * 20.0f;
	}
	if (intro_alpha <= 0.005f) {
		return;
	}

	const char *order[] = {"settings", "extras", "photo", "music", "video", "game", "network", "psn"};
	const char *cookie_order[] = {
		"cookies_cat_0", "cookies_cat_1", "cookies_cat_2", "cookies_cat_3",
		"cookies_cat_4", "cookies_cat_5", "cookies_cat_6", "cookies_cat_7",
	};
	const bool same = category_ == category_from_ || category_t_ >= 1.0f;
	const float shift_from = kCategoryX[category_from_] - kCategoryAnchorX;
	const float shift_to = kCategoryX[category_] - kCategoryAnchorX;
	const float shift = Lerp(shift_from, shift_to, category_t_);
	for (int index = 0; index < 8; ++index) {
		auto texture = textures_.end();
		if (settings_.theme_style == 2) {
			texture = textures_.find(cookie_order[index]);
		}
		if (texture == textures_.end()) {
			texture = textures_.find(order[index]);
		}
		if (texture == textures_.end()) {
			continue;
		}
		const float x = kCategoryAnchorX + (kCategoryX[index] - kCategoryAnchorX) - shift + intro_slide_x;
		float icon_scale = 0.8f;
		float icon_alpha = 0.55f;
		if (same) {
			if (index == category_) {
				icon_scale = 1.0f;
				icon_alpha = 1.0f;
			}
		} else if (index == category_from_) {
			icon_scale = Lerp(1.0f, 0.8f, category_t_);
			icon_alpha = Lerp(1.0f, 0.55f, category_t_);
		} else if (index == category_) {
			icon_scale = Lerp(0.8f, 1.0f, category_t_);
			icon_alpha = Lerp(0.55f, 1.0f, category_t_);
		}
		icon_scale *= intro_scale;
		icon_alpha *= intro_alpha;
		const float icon_color[4] = {1.0f, 1.0f, 1.0f, icon_alpha};
		auto glow = textures_.find("focus_glow");
		if (glow != textures_.end() && index == category_ && icon_scale >= 0.99f) {
			const float glow_color[4] = {1.0f, 1.0f, 1.0f, 0.62f * intro_alpha};
			DrawTexture(glow->second, x, kCategoryY, 1.0f, glow_color);
		}
		const float fit = (texture->second.height > 44) ? (34.0f / (float)texture->second.height) : 1.0f;
		DrawTexture(texture->second, x, kCategoryY, icon_scale * fit, icon_color);
		if (!font_loaded_) {
			continue;
		}
		const float label_scale = 0.63f;
		const std::string label = TextOr(kCategoryKeys[index], kCategoryFallbacks[index]);
		if (same && index == category_) {
			const float width = MeasureText(label, label_scale);
			const float label_color[4] = {1.0f, 1.0f, 1.0f, intro_alpha};
			DrawText(label, x - width * 0.5f, 103.0f, label_scale, label_color, true);
		} else if (!same && index == category_from_) {
			const float width = MeasureText(label, label_scale);
			const float label_color[4] = {1.0f, 1.0f, 1.0f, (1.0f - category_t_) * intro_alpha};
			DrawText(label, x - width * 0.5f, 103.0f, label_scale, label_color, true);
		} else if (!same && index == category_) {
			const float width = MeasureText(label, label_scale);
			const float label_color[4] = {1.0f, 1.0f, 1.0f, category_t_ * intro_alpha};
			DrawText(label, x - width * 0.5f, 103.0f, label_scale, label_color, true);
		}
	}
}

void XmbRenderer::SetGames(const std::vector<GameInfo> &games) {
	std::lock_guard<std::recursive_mutex> lock(mutex_);
	games_ = games;
	if (!games_.empty()) {
		games_loading_ = false;
	}
	if (game_page_index_ >= (int)games_.size()) {
		game_page_index_ = std::max(0, (int)games_.size() - 1);
	}
	game_scroll_ = (float)game_page_index_;
	pic1_loaded_path_.clear();
	pic0_loaded_path_.clear();
	pic1_texture_ = 0;
	pic0_texture_ = 0;
	if (memory_stick_page_ && !games_.empty()) {
		pending_focus_notify_ = true;
	}
}

void XmbRenderer::SetSaveStates(const std::vector<SaveStateInfo> &saves) {
	std::lock_guard<std::recursive_mutex> lock(mutex_);
	saves_ = saves;
	if (!saves_.empty()) {
		saves_loading_ = false;
	}
	if (save_page_index_ >= (int)saves_.size()) {
		save_page_index_ = std::max(0, (int)saves_.size() - 1);
	}
	save_scroll_ = (float)save_page_index_;
}

void XmbRenderer::SetBattery(int level, bool charging) {
	std::lock_guard<std::recursive_mutex> lock(mutex_);
	battery_level_ = std::max(0, std::min(100, level));
	battery_charging_ = charging;
}

std::pair<int, int> XmbRenderer::GetState() {
	std::lock_guard<std::recursive_mutex> lock(mutex_);
	if (saved_data_page_) {
		return {5, 2000 + save_page_index_};
	}
	if (memory_stick_page_) {
		return {5, 1000 + game_page_index_};
	}
	return {category_, item_};
}

void XmbRenderer::SetState(int category, int item) {
	std::lock_guard<std::recursive_mutex> lock(mutex_);
	launching_ = false;
	launch_dispatched_ = false;
	launch_t_ = 0.0f;
	settings_sub_active_ = false;
	settings_picker_open_ = false;
	empty_media_kind_ = EmptyMediaKind::None;
	if (category == 5 && item >= 2000) {
		category_ = 5;
		item_ = 1;
		category_selected_[5] = 1;
		items_ = categories_[5];
		saved_data_page_ = true;
		memory_stick_page_ = false;
		save_page_index_ = item - 2000;
		if (!saves_.empty()) {
			save_page_index_ = std::max(0, std::min((int)saves_.size() - 1, save_page_index_));
		}
		save_scroll_ = (float)save_page_index_;
		return;
	}
	if (category == 5 && item >= 1000) {
		category_ = 5;
		item_ = 2;
		category_selected_[5] = 2;
		items_ = categories_[5];
		memory_stick_page_ = true;
		saved_data_page_ = false;
		game_page_index_ = item - 1000;
		if (!games_.empty()) {
			game_page_index_ = std::max(0, std::min((int)games_.size() - 1, game_page_index_));
			pending_focus_notify_ = true;
		}
		game_scroll_ = (float)game_page_index_;
		return;
	}
	memory_stick_page_ = false;
	saved_data_page_ = false;
	category = std::max(0, std::min(7, category));
	if (category != category_) {
		category_selected_[category_] = item_;
		category_from_ = category_;
		category_ = category;
		category_t_ = 1.0f;
		items_ = categories_[category_];
	}
	const int count = (int)items_.size();
	item_ = count > 0 ? std::max(0, std::min(count - 1, item)) : 0;
	item_from_ = item_;
	item_t_ = 1.0f;
	category_selected_[category_] = item_;
	previous_items_ = items_;
	previous_item_ = item_;
	items_fade_t_ = 1.0f;
}

void XmbRenderer::DrawMemoryStickPage() {
	const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
	auto arrow = textures_.find("arrow_l");
	if (arrow != textures_.end()) {
		const float shadow_color[4] = {0.0f, 0.0f, 0.0f, 0.48f};
		DrawTexture(arrow->second, 10.0f, 137.0f, 1.0f, shadow_color);
		DrawTexture(arrow->second, 9.0f, 136.0f, 1.0f, white);
	}

	if (games_.empty()) {
		if (games_loading_) {
			auto ld = textures_.find("item_loading");
			if (ld != textures_.end()) {
				const float pulse = 0.45f + 0.40f * std::sin(time_elapsed_ * 5.0f);
				const float lcol[4] = {1.0f, 1.0f, 1.0f, pulse};
				DrawTexture(ld->second, 92.0f, 136.0f, 0.85f, lcol);
			}
		} else if (font_loaded_) {
			const float empty_col[4] = {1.0f, 1.0f, 1.0f, 0.78f};
			DrawText(TextOr("msgshare_error_no_game", "There are no games."), 174.0f, 145.0f, 0.82f, empty_col, true);
		}
		return;
	}

	game_page_index_ = std::max(0, std::min((int)games_.size() - 1, game_page_index_));
	const auto &selected_game = games_[game_page_index_];

	// 1. Fullscreen PIC1.PNG background with crossfade
	if (selected_game.pic1_path != pic1_loaded_path_) {
		pic1_loaded_path_ = selected_game.pic1_path;
		pic1_texture_ = 0;
		if (!selected_game.pic1_path.empty()) {
			if (const Texture *t = TextureForPath(selected_game.pic1_path)) {
				pic1_texture_ = t->id;
			}
		}
		pic1_alpha_ = 0.0f;
	}
	if (pic1_texture_ != 0) {
		pic1_alpha_ = std::min(1.0f, pic1_alpha_ + 0.05f);
		glBindTexture(GL_TEXTURE_2D, pic1_texture_);
		const float pic1_color[4] = {1.0f, 1.0f, 1.0f, pic1_alpha_};
		DrawQuad(0.0f, 0.0f, screen_width_, kScreenHeight, pic1_color);
	}

	// 2. PIC0.PNG logo banner (and title when no PIC0 banner)
	if (selected_game.pic0_path != pic0_loaded_path_) {
		pic0_loaded_path_ = selected_game.pic0_path;
		pic0_texture_ = 0;
		if (!selected_game.pic0_path.empty()) {
			if (const Texture *t = TextureForPath(selected_game.pic0_path)) {
				pic0_texture_ = t->id;
			}
		}
	}
	if (pic0_texture_ != 0) {
		glBindTexture(GL_TEXTURE_2D, pic0_texture_);
		const float pic0_w = std::min(305.0f, screen_width_ - 172.0f);
		const float pic0_h = pic0_w * (172.0f / 305.0f);
		DrawQuad(165.0f, 134.0f - pic0_h * 0.5f, pic0_w, pic0_h, white);
	} else if (font_loaded_) {
		DrawText(selected_game.title, 174.0f, 145.0f, 0.82f, white, true);
	}

	// Re-draw left arrow over PIC1 so it stays legible
	if (arrow != textures_.end()) {
		const float shadow_color[4] = {0.0f, 0.0f, 0.0f, 0.48f};
		DrawTexture(arrow->second, 10.0f, 137.0f, 1.0f, shadow_color);
		DrawTexture(arrow->second, 9.0f, 136.0f, 1.0f, white);
	}

	// 3. Vertical list of games
	for (int i = 0; i < (int)games_.size(); ++i) {
		const float offset = (float)i - game_scroll_;
		const float y = 136.0f + offset * 75.5f;
		if (y < -60.0f || y > 332.0f) {
			continue;
		}
		const float dist = std::abs(offset);
		if (dist < 0.6f) {
			const float t = 1.0f - dist;
			auto glow = textures_.find("focus_glow");
			if (glow != textures_.end()) {
				const float glow_color[4] = {1.0f, 1.0f, 1.0f, 0.70f * t};
				DrawTexture(glow->second, 92.0f, y, 0.88f, glow_color);
			}
			DrawGameIcon(games_[i], 92.0f, y, 144.0f, 80.0f, white);
		} else {
			const float alpha = std::max(0.18f, 0.68f - (dist - 1.0f) * 0.22f);
			const float icon_color[4] = {1.0f, 1.0f, 1.0f, alpha};
			DrawGameIcon(games_[i], 92.5f, y, 81.0f, 45.0f, icon_color);
		}
	}
}

void XmbRenderer::DrawItems() {
	const float fade = items_fade_t_;
	const float eased = 1.0f - (1.0f - fade) * (1.0f - fade);
	if (fade < 1.0f) {
		DrawItemColumn(previous_items_, previous_item_, (float)previous_item_, 1.0f - eased);
	}
	const float scroll = Lerp((float)item_from_, (float)item_, item_t_);
	DrawItemColumn(items_, item_, scroll, eased);
}

void XmbRenderer::DrawItemColumn(const std::vector<Item> &list, int selected, float scroll,
                                 float global_alpha) {
	float intro_slide_y = 0.0f;
	if (opening_t_ < 1.0f) {
		const float u = std::clamp((opening_t_ - 0.46f) / 0.54f, 0.0f, 1.0f);
		const float ease = 1.0f - (1.0f - u) * (1.0f - u) * (1.0f - u);
		global_alpha *= ease;
		intro_slide_y = (1.0f - ease) * 24.0f;
	} else if (resume_t_ < 1.0f) {
		const float u = std::clamp(resume_t_ / 0.85f, 0.0f, 1.0f);
		const float ease = 1.0f - (1.0f - u) * (1.0f - u);
		global_alpha *= ease;
		intro_slide_y = (1.0f - ease) * 14.0f;
	}
	if (global_alpha <= 0.001f || list.empty()) {
		return;
	}
	for (int index = 0; index < (int)list.size(); ++index) {
		if (index < selected) {
			continue;
		}
		const float y = kSelectedItemY + ((float)index - scroll) * kItemSpacing + intro_slide_y;
		if (y < -48.0f || y > kScreenHeight + 48.0f) {
			continue;
		}
		float focus = 1.0f - std::min(1.0f, std::fabs((float)index - scroll));
		float alpha = (0.55f + 0.45f * focus) * global_alpha;
		if (list[index].disabled) {
			alpha *= 0.45f;
		}
		const float color[4] = {1.0f, 1.0f, 1.0f, alpha};
		auto texture = textures_.find(list[index].texture_name);
		const Texture *icon_texture = texture != textures_.end() ? &texture->second : nullptr;
		if (list[index].is_game && !list[index].icon_path.empty()) {
			if (const Texture *embedded = TextureForPath(list[index].icon_path)) {
				icon_texture = embedded;
			}
		}
		if (icon_texture != nullptr) {
			float icon_scale = 1.0f;
			if (list[index].is_game) {
				const float max_dimension = std::max(1, std::max(icon_texture->width, icon_texture->height));
				icon_scale = 48.0f / max_dimension;
			} else if (icon_texture->width > 0 && icon_texture->height > 0) {
				icon_scale = std::min(1.0f, std::min(38.0f / (float)icon_texture->width,
				                                     30.0f / (float)icon_texture->height));
			}
			auto glow = textures_.find("focus_glow");
			if (glow != textures_.end() && focus > 0.6f) {
				const float glow_color[4] = {1.0f, 1.0f, 1.0f, 0.65f * focus * global_alpha};
				DrawTexture(glow->second, kItemIconX, y, list[index].is_game ? 0.8f : 0.65f, glow_color);
			}
			DrawTexture(*icon_texture, kItemIconX, y, icon_scale, color);
		}
		if (!font_loaded_) {
			continue;
		}
		const bool has_subtitle = !list[index].subtitle.empty();
		const float title_baseline = has_subtitle ? y - 5.0f : y + 5.0f;
		DrawText(list[index].title, 141.0f, title_baseline, 0.81f, color, true);
		if (has_subtitle) {
			const float subtitle_color[4] = {1.0f, 1.0f, 1.0f, alpha * 0.85f};
			DrawText(list[index].subtitle, 141.0f, y + 14.0f, 0.62f, subtitle_color, true);
			const float rule_color[4] = {1.0f, 1.0f, 1.0f, alpha * 0.5f};
			DrawSolidQuad(141.0f, y + 1.0f, std::max(180.0f, screen_width_ - 145.0f), 1.0f, rule_color);
		}
	}
}

void XmbRenderer::DrawOpeningTransition() {
	if (opening_t_ >= 1.0f) {
		return;
	}
	// 1. Smooth black fade-in at the very start of XMB entry (0.00 -> 0.32)
	if (opening_t_ < 0.32f) {
		const float fade = 1.0f - (opening_t_ / 0.32f);
		const float black[4] = {0.0f, 0.0f, 0.0f, fade * fade};
		DrawSolidQuad(0.0f, 0.0f, screen_width_, kScreenHeight, black);
	}
	// 2. Authentic opening_plugin.rco light-streak glow across the wave (0.05 -> 0.72)
	if (opening_t_ < 0.74f) {
		const float u = std::clamp((opening_t_ - 0.04f) / 0.68f, 0.0f, 1.0f);
		const float bell = std::sin(u * 3.14159265f);
		const float cx = screen_width_ * 0.5f;
		glBlendFunc(GL_SRC_ALPHA, GL_ONE);

		auto glow_mid = textures_.find("open_glow_b_mid");
		auto glow_top = textures_.find("open_glow_b_top");
		auto glow_bot = textures_.find("open_glow_b_bot");
		auto glow_a = textures_.find("open_glow_a");
		if (glow_mid != textures_.end()) {
			const float gx = (cx - 140.0f) + u * 280.0f;
			const float gc[4] = {1.0f, 1.0f, 1.0f, 0.68f * bell};
			DrawTexture(glow_mid->second, gx, 136.0f, 1.35f, gc);
		}
		if (glow_top != textures_.end()) {
			const float gc[4] = {1.0f, 1.0f, 1.0f, 0.52f * bell};
			DrawTexture(glow_top->second, cx, 122.0f, 1.0f, gc);
		}
		if (glow_bot != textures_.end()) {
			const float gc[4] = {1.0f, 1.0f, 1.0f, 0.52f * bell};
			DrawTexture(glow_bot->second, cx, 150.0f, 1.0f, gc);
		}
		if (glow_a != textures_.end()) {
			const float gx = (cx - 180.0f) + u * 360.0f;
			const float gc[4] = {1.0f, 1.0f, 1.0f, 0.75f * bell};
			DrawTexture(glow_a->second, gx, 136.0f, 1.6f, gc);
		}
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

		// Subtle Sony Computer Entertainment emblem glimmer before XMB icons take focus
		if (opening_t_ < 0.56f) {
			const float lu = std::clamp((opening_t_ - 0.06f) / 0.48f, 0.0f, 1.0f);
			const float l_alpha = std::sin(lu * 3.14159265f) * 0.88f;
			auto sce = textures_.find("open_sce_logo");
			if (sce != textures_.end() && l_alpha > 0.02f) {
				const float sc[4] = {1.0f, 1.0f, 1.0f, l_alpha};
				DrawTexture(sce->second, cx + 96.0f, 148.0f, 0.90f, sc);
			}
		}
	}
}

void XmbRenderer::DrawResumeTransition() {
	if (resume_t_ >= 1.0f) {
		return;
	}
	// Smooth fade-in from black on returning from a game
	if (resume_t_ < 0.48f) {
		const float fade = 1.0f - (resume_t_ / 0.48f);
		const float black[4] = {0.0f, 0.0f, 0.0f, fade * fade};
		DrawSolidQuad(0.0f, 0.0f, screen_width_, kScreenHeight, black);
	}
	// Authentic 15-frame system_plugin_fg/tex_busy.png spinner in bottom-right corner
	const float busy_alpha = 1.0f - std::clamp((resume_t_ - 0.55f) / 0.45f, 0.0f, 1.0f);
	if (busy_alpha > 0.02f) {
		const int frame = static_cast<int>(time_elapsed_ * 30.0f) % 15;
		const float bcol[4] = {1.0f, 1.0f, 1.0f, busy_alpha * 0.92f};
		auto busy_sh = textures_.find("status_busy_shadow");
		auto busy = textures_.find("status_busy");
		if (busy_sh != textures_.end()) {
			DrawTextureCrop(busy_sh->second, screen_width_ - 27.0f, 249.0f, 0.72f, bcol, 0, frame * 34, 34, 34);
		}
		if (busy != textures_.end()) {
			DrawTextureCrop(busy->second, screen_width_ - 28.0f, 248.0f, 0.72f, bcol, 0, frame * 34, 34, 34);
		}
	}
}

void XmbRenderer::DrawGameBootAnimation() {
	if (!launching_) {
		return;
	}
	const float t = std::clamp(launch_t_, 0.0f, 1.0f);
	const float cx = screen_width_ * 0.5f;

	// 1. Luminous pearl-white & sky-cyan PSP Gameboot backdrop using real Sony 13-27.bmp gradient (ext_10.png)
	const float bg_in = std::clamp(t / 0.18f, 0.0f, 1.0f);
	const float bg_ease = bg_in * bg_in * (3.0f - 2.0f * bg_in);
	const float base_col[4] = {0.90f, 0.96f, 1.0f, bg_ease};
	DrawSolidQuad(0.0f, 0.0f, screen_width_, kScreenHeight, base_col);

	auto gb_bg = textures_.find("gameboot_bg");
	if (gb_bg != textures_.end() && gb_bg->second.id != 0) {
		glBindTexture(GL_TEXTURE_2D, gb_bg->second.id);
		const float grad_col[4] = {1.0f, 1.0f, 1.0f, bg_ease * 0.34f};
		DrawQuad(0.0f, 0.0f, screen_width_, kScreenHeight, grad_col);
	}

	// Render the live 3D PSP wave mesh over the luminous gameboot backdrop
	DrawWave(0.0f);
	glUseProgram(program_);
	glUniform2f(u_screen_, screen_width_, kScreenHeight);
	glUniform1i(u_texture_, 0);
	glActiveTexture(GL_TEXTURE0);
	glBindVertexArray(vertex_array_);

	// 2. Authentic opening_plugin.rco light beams & glow ribbons
	glBlendFunc(GL_SRC_ALPHA, GL_ONE);
	auto glow_mid = textures_.find("open_glow_b_mid");
	auto glow_top = textures_.find("open_glow_b_top");
	auto glow_bot = textures_.find("open_glow_b_bot");
	auto glow_a = textures_.find("open_glow_a");
	auto glow_v = textures_.find("open_glow");

	const float beam_bell = std::sin(std::clamp(t / 0.88f, 0.0f, 1.0f) * 3.14159265f);
	if (glow_v != textures_.end()) {
		const float vx1 = (cx - 130.0f) + t * 260.0f;
		const float vx2 = (cx + 130.0f) - t * 240.0f;
		const float vc[4] = {0.75f, 0.92f, 1.0f, 0.55f * beam_bell};
		DrawTexture(glow_v->second, vx1, 136.0f, 1.8f, vc);
		DrawTexture(glow_v->second, vx2, 136.0f, 1.4f, vc);
	}
	if (glow_mid != textures_.end()) {
		const float gx = (cx - 160.0f) + t * 320.0f;
		const float gc[4] = {1.0f, 1.0f, 1.0f, 0.85f * beam_bell};
		DrawTexture(glow_mid->second, gx, 136.0f, 1.8f, gc);
	}
	if (glow_top != textures_.end()) {
		const float gc[4] = {1.0f, 1.0f, 1.0f, 0.72f * beam_bell};
		DrawTexture(glow_top->second, cx, 114.0f, 1.0f, gc);
	}
	if (glow_bot != textures_.end()) {
		const float gc[4] = {1.0f, 1.0f, 1.0f, 0.72f * beam_bell};
		DrawTexture(glow_bot->second, cx, 158.0f, 1.0f, gc);
	}
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	// 3. Authentic opening_plugin/tex_psp_logo.png ("PSP PlayStation Portable", 260x80)
	auto psp_logo = textures_.find("open_psp_logo");
	if (psp_logo != textures_.end() && t >= 0.10f) {
		const float lin = std::clamp((t - 0.10f) / 0.22f, 0.0f, 1.0f);
		const float lout = 1.0f - std::clamp((t - 0.76f) / 0.16f, 0.0f, 1.0f);
		const float lalpha = lin * lout;
		const float lscale = 0.88f + 0.13f * (1.0f - (1.0f - std::clamp((t - 0.10f) / 0.72f, 0.0f, 1.0f)) *
		                                             (1.0f - std::clamp((t - 0.10f) / 0.72f, 0.0f, 1.0f)));
		// Crisp dark-navy drop shadow + dark-charcoal/metallic PSP logo over luminous pearl wave
		const float shadow_c[4] = {0.06f, 0.14f, 0.26f, lalpha * 0.42f};
		const float logo_c[4] = {0.10f, 0.13f, 0.20f, lalpha * 0.96f};
		DrawTexture(psp_logo->second, cx + 1.5f, 137.5f, lscale, shadow_c);
		DrawTexture(psp_logo->second, cx, 136.0f, lscale, logo_c);

		// Specular highlight sweep across the PSP logo
		if (glow_a != textures_.end()) {
			glBlendFunc(GL_SRC_ALPHA, GL_ONE);
			const float sx = (cx - 130.0f) + std::clamp((t - 0.15f) / 0.60f, 0.0f, 1.0f) * 260.0f;
			const float sc[4] = {1.0f, 1.0f, 1.0f, 0.75f * lalpha};
			DrawTexture(glow_a->second, sx, 130.0f, 1.45f, sc);
			glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		}
	}

	// 4. Final fade to pure black before handing over to EmulationScreen (0.78 -> 1.00)
	if (t > 0.78f) {
		const float u = std::clamp((t - 0.78f) / 0.20f, 0.0f, 1.0f);
		const float black[4] = {0.0f, 0.0f, 0.0f, u * u * (3.0f - 2.0f * u)};
		DrawSolidQuad(0.0f, 0.0f, screen_width_, kScreenHeight, black);
	}
}

void XmbRenderer::Draw() {
	if (surface_width_ <= 0 || surface_height_ <= 0 || program_ == 0) {
		return;
	}
	int selected_game_to_launch = -1;
	int selected_save_to_launch = -1;
	int deferred_focus_game = -999;
	{
		std::lock_guard<std::recursive_mutex> lock(mutex_);
		const auto now = std::chrono::steady_clock::now();
		float dt = std::chrono::duration<float>(now - last_draw_).count();
		last_draw_ = now;
		dt = std::min(dt, 0.1f);
		time_elapsed_ += dt;

		if (pending_focus_notify_) {
			pending_focus_notify_ = false;
			if (memory_stick_page_ && game_page_index_ >= 0 && game_page_index_ < (int)games_.size()) {
				deferred_focus_game = game_page_index_;
			}
		}

		if (boot_t_ < 1.0f) {
			boot_t_ = std::min(1.0f, boot_t_ + dt / 0.50f);
		}
		if (opening_t_ < 1.0f) {
			opening_t_ = std::min(1.0f, opening_t_ + dt / 1.40f);
		}
		if (resume_t_ < 1.0f) {
			resume_t_ = std::min(1.0f, resume_t_ + dt / 0.65f);
		}
		if (toast_timer_ > 0.0f) {
			toast_timer_ -= dt;
		}
		if (memory_stick_page_) {
			game_scroll_ += ((float)game_page_index_ - game_scroll_) * (1.0f - std::exp(-dt / 0.055f));
		}
		if (saved_data_page_) {
			save_scroll_ += ((float)save_page_index_ - save_scroll_) * (1.0f - std::exp(-dt / 0.055f));
		}
		if (settings_sub_active_) {
			settings_sub_scroll_ += ((float)settings_sub_index_ - settings_sub_scroll_) * (1.0f - std::exp(-dt / 0.055f));
		}

		category_t_ = AdvanceTowardsOne(category_t_, dt, kCategoryTau);
		item_t_ = AdvanceTowardsOne(item_t_, dt, kItemTau);
		if (items_fade_t_ < 1.0f) {
			items_fade_t_ = std::min(1.0f, items_fade_t_ + dt / 0.18f);
		}

		if (theme_dirty_) {
			theme_dirty_ = false;
			UpdateThemeColor();
		}

		if (settings_.aspect_mode == 1) {
			screen_width_ = kScreenWidth; // 16:9 Original (480x272)
		} else if (settings_.aspect_mode == 2) {
			screen_width_ = kScreenHeight * (4.0f / 3.0f); // 4:3 Original (362.67x272)
		} else {
			// Auto (Full Screen): adapt virtual width to device aspect ratio (20:9, 16:9, 16:10, 10:7, 4:3)
			// keeping 1:1 square virtual pixels so icons and text never stretch or distort.
			const float surf_aspect = (float)surface_width_ / (float)std::max(1, surface_height_);
			screen_width_ = std::clamp(kScreenHeight * surf_aspect, 360.0f, 680.0f);
		}

		const float target_w = screen_width_;
		const float scale = std::min(surface_width_ / target_w, surface_height_ / kScreenHeight);
		const int view_width = (int)std::lround(target_w * scale);
		const int view_height = (int)std::lround(kScreenHeight * scale);
		const int view_x = (surface_width_ - view_width) / 2;
		const int view_y = (surface_height_ - view_height) / 2;

		glViewport(0, 0, surface_width_, surface_height_);
		glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);

		glViewport(view_x, view_y, view_width, view_height);
		glEnable(GL_SCISSOR_TEST);
		glScissor(view_x, view_y, view_width, view_height);

		if (settings_.bg_mode == 2) {
			glClearColor(0.08f, 0.08f, 0.10f, 1.0f);
		} else {
			glClearColor(wave_tint_[0] * 0.22f, wave_tint_[1] * 0.22f, wave_tint_[2] * 0.22f, 1.0f);
		}
		glClear(GL_COLOR_BUFFER_BIT);
		glDisable(GL_SCISSOR_TEST);

		glUseProgram(program_);
		glUniform2f(u_screen_, screen_width_, kScreenHeight);
		glUniform1i(u_texture_, 0);
		glActiveTexture(GL_TEXTURE0);
		glBindVertexArray(vertex_array_);
		DrawBackground();

		DrawWave(dt);

		glUseProgram(program_);
		glUniform2f(u_screen_, screen_width_, kScreenHeight);
		glUniform1i(u_texture_, 0);
		glActiveTexture(GL_TEXTURE0);
		glBindVertexArray(vertex_array_);

		if (memory_stick_page_) {
			DrawMemoryStickPage();
			DrawStatusBar();
		} else if (saved_data_page_) {
			DrawSavedDataPage();
			DrawStatusBar();
		} else if (settings_sub_active_) {
			DrawSettingsSubPage();
			DrawStatusBar();
		} else if (empty_media_kind_ != EmptyMediaKind::None) {
			DrawEmptyMediaPage();
			DrawStatusBar();
		} else {
			DrawItems();
			DrawCategoryRow();
			DrawStatusBar();
		}
		DrawTouchControlsBar();

		DrawOpeningTransition();
		DrawResumeTransition();

		DrawToast();
		DrawExitDialog();
		DrawOptionsMenu();
		DrawDeleteDialog();
		DrawInfoDialog();
		DrawSystemInfoDialog();
		DrawNoticeDialog();
		if (restore_confirm_active_) {
			DrawYesNoDialog(
				TextOr("xmb_restore_confirm_1", "Restore default settings?"),
				TextOr("xmb_restore_confirm_2", "All settings will return to defaults."),
				restore_confirm_selected_);
		}
		if (browser_confirm_active_) {
			DrawYesNoDialog(
				TextOr("xmb_browser_confirm_1", "Do you want to start the Internet browser?"),
				TextOr("xmb_browser_confirm_2", "Opens outside EmuCoreA in your device browser."),
				browser_confirm_selected_);
		}
		DrawTouchTutorialDialog();

		if (launching_) {
			launch_t_ = std::min(1.0f, launch_t_ + dt / 2.10f);
			DrawGameBootAnimation();
			if (launch_t_ >= 1.0f && !launch_dispatched_) {
				launch_dispatched_ = true;
				if (launching_save_index_ >= 0) {
					selected_save_to_launch = launching_save_index_;
					launching_save_index_ = -1;
				} else {
					selected_game_to_launch = game_page_index_;
				}
			}
		}

		glBindVertexArray(0);
		if (!logged_gl_error_) {
			const GLenum error = glGetError();
			if (error != GL_NO_ERROR) {
				__android_log_print(ANDROID_LOG_ERROR, kTag, "GL error %04x", error);
				logged_gl_error_ = true;
			}
		}
	}
	if (deferred_focus_game != -999) {
		GameFocused(deferred_focus_game);
	}
	if (selected_save_to_launch >= 0) {
		SaveSelected(selected_save_to_launch);
	}
	if (selected_game_to_launch >= 0) {
		GameSelected(selected_game_to_launch);
	}
}

void XmbRenderer::Shutdown() {
	pic1_texture_ = 0;
	pic0_texture_ = 0;
	pic1_loaded_path_.clear();
	pic0_loaded_path_.clear();
	if (white_texture_) {
		glDeleteTextures(1, &white_texture_);
		white_texture_ = 0;
	}
	if (background_texture_) {
		glDeleteTextures(1, &background_texture_);
		background_texture_ = 0;
	}
	if (wave_texture_) {
		glDeleteTextures(1, &wave_texture_);
		wave_texture_ = 0;
	}
	for (auto &entry : image_textures_) {
		if (entry.second.id) {
			glDeleteTextures(1, &entry.second.id);
		}
	}
	image_textures_.clear();
	for (auto &entry : shadow_glyph_textures_) {
		if (entry.second.id) {
			glDeleteTextures(1, &entry.second.id);
		}
	}
	shadow_glyph_textures_.clear();
	for (auto &entry : glyph_textures_) {
		if (entry.second.id) {
			glDeleteTextures(1, &entry.second.id);
		}
	}
	glyph_textures_.clear();
	fallback_glyphs_.clear();
	for (auto &entry : textures_) {
		if (entry.second.id) {
			glDeleteTextures(1, &entry.second.id);
		}
	}
	textures_.clear();
	if (vertex_buffer_) {
		glDeleteBuffers(1, &vertex_buffer_);
		vertex_buffer_ = 0;
	}
	if (vertex_array_) {
		glDeleteVertexArrays(1, &vertex_array_);
		vertex_array_ = 0;
	}
	if (program_) {
		glDeleteProgram(program_);
		program_ = 0;
	}
	if (wave_ebo_) {
		glDeleteBuffers(1, &wave_ebo_);
		wave_ebo_ = 0;
	}
	if (wave_vbo_) {
		glDeleteBuffers(1, &wave_vbo_);
		wave_vbo_ = 0;
	}
	if (wave_vao_) {
		glDeleteVertexArrays(1, &wave_vao_);
		wave_vao_ = 0;
	}
	if (wave_program_) {
		glDeleteProgram(wave_program_);
		wave_program_ = 0;
	}
	root_.clear();
}

}  // namespace xmb

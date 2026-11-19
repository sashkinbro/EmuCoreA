#include "xmb_renderer.h"

#include <android/log.h>

#include <cmath>
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
constexpr float kWaveTint[3] = {1.0f, 0.55f, 0.20f};
constexpr float kWaveNdcScaleY = 0.68f;
constexpr float kWaveNdcOffsetY = -0.40f;

// Row medians measured from the 6.60 default theme. The wave adds moving
// highlights on top rather than carrying the whole lower-half colour.
const uint8_t kBackgroundRows[17][3] = {
	{107, 46, 27}, {99, 39, 23}, {96, 37, 22}, {90, 33, 20}, {86, 30, 18},
	{81, 27, 16}, {77, 24, 14}, {74, 21, 12}, {96, 29, 12}, {144, 49, 12},
	{153, 52, 12}, {169, 58, 12}, {182, 62, 12}, {185, 63, 12}, {183, 62, 11},
	{182, 62, 10}, {182, 62, 10},
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

void BuildWaveMvp(float out[16]) {
	constexpr float kFovY = 30.0f;
	constexpr float kNearFactor = 0.1f;
	constexpr float kFarFactor = 4.0f;
	const float aspect = kScreenWidth / kScreenHeight;
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
	// Place the sheet in the lower half of the screen; the raw model is centred.
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

bool XmbRenderer::Initialize(const std::string &resource_root) {
	root_ = resource_root;
	LoadTextCatalog();
	textures_.clear();
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
		{"item_savedata", "images/topmenu_icon/EJ.png"},
		{"item_memstick", "images/topmenu_icon/BI.png"},
		{"item_update", "images/topmenu_icon/EH.png"},
		{"battery", "images/system_plugin_fg/tex_battery.png"},
		{"focus_glow", "images/system_plugin/tex_3x3_focus.png"},
		{"item_usb", "images/topmenu_icon/CA.png"},
		{"item_video_settings", "images/topmenu_icon/CX.png"},
		{"item_photo_settings", "images/topmenu_icon/CY.png"},
		{"item_system_settings", "images/topmenu_icon/CZ.png"},
		{"item_camera", "images/topmenu_icon/DL.png"},
		{"item_comics", "images/topmenu_icon/DW.png"},
		{"item_sensme", "images/topmenu_icon/DM.png"},
		{"item_internet_search", "images/topmenu_icon/FB.png"},
		{"item_internet_browser", "images/topmenu_icon/EU.png"},
		{"item_psn_signup", "images/topmenu_icon/FU.png"},
		{"item_psn_store", "images/topmenu_icon/FV.png"},
		{"item_game", "images/system_plugin/tex_default_game_icon.png"},
		{"frame_subs", "images/game_plugin/tex_subs_frame.png"},
		{"arrow_l", "images/game_plugin/tex_arrow_l.png"},
		{"arrow_l_shadow", "images/game_plugin/tex_arrow_l_shadow.png"},
		{"arrow_r", "images/game_plugin/tex_arrow_r.png"},
		{"arrow_r_shadow", "images/game_plugin/tex_arrow_r_shadow.png"},
	};
	for (const auto &asset : assets) {
		LoadTexture(asset.name, asset.path);
	}
	categories_[0] = {
		{ TextOr("msgtop_sysconf_usb", "USB Connection"), "", "item_usb" },
		{ TextOr("msgtop_sysconf_video", "Video Settings"), "", "item_video_settings" },
		{ TextOr("msgtop_sysconf_photo", "Photo Settings"), "", "item_photo_settings" },
		{ TextOr("msgtop_sysconf_console", "System Settings"), "", "item_system_settings", true },
	};
	categories_[1] = {{ TextOr("msg_digitalcomics", "Digital Comics"), "", "item_comics" }};
	categories_[2] = {{ TextOr("msgtop_camera", "Camera"), "", "item_camera" }};
	categories_[3] = {{ TextOr("msg_sensme_channels", "SensMe\u2122 channels"), "", "item_sensme" }};
	categories_[4] = {{ TextOr("msgshare_ms", "Memory Stick\u2122"), TextOr("msgshare_info_space", "Free Space") + " 1459 MB", "item_memstick" }};
	categories_[5] = {
		{ TextOr("msgtop_game_savedata", "Saved Data Utility"), "", "item_savedata" },
		{ TextOr("msgshare_ms", "Memory Stick\u2122"), TextOr("msgshare_info_space", "Free Space") + " 1459 MB", "item_memstick" },
	};
	categories_[6] = {
		{ TextOr("msgtop_network_browser", "Internet Browser"), "", "item_internet_browser" },
		{ TextOr("msg_internet_search", "Internet Search"), "", "item_internet_search" },
	};
	categories_[7] = {
		{ TextOr("msg_signup", "Sign Up for PlayStation\u00aeNetwork"), "", "item_psn_signup" },
		{ TextOr("msg_ps_store", "PlayStation\u00aeStore"), "", "item_psn_store" },
	};
	category_selected_ = {1, 0, 0, 0, 0, 0, 0, 0};
	items_ = categories_[5];
	item_ = 0;
	item_from_ = 0;
	item_t_ = 1.0f;
	category_ = 5;
	category_from_ = 5;
	category_t_ = 1.0f;
	font_loaded_ = font_.Load(root_ + "/fonts/ltn0.pgf");
	if (font_loaded_) {
		__android_log_print(ANDROID_LOG_INFO, kTag, "font loaded: %s", font_.name().c_str());
	} else {
		__android_log_print(ANDROID_LOG_WARN, kTag, "fonts/ltn0.pgf could not be loaded");
	}
	wave_loaded_ = LoadWave();
	if (background_texture_ == 0) {
		std::vector<uint8_t> rgba(sizeof(kBackgroundRows) / sizeof(kBackgroundRows[0]) * 4);
		for (size_t i = 0; i < sizeof(kBackgroundRows) / sizeof(kBackgroundRows[0]); ++i) {
			rgba[i * 4 + 0] = kBackgroundRows[i][0];
			rgba[i * 4 + 1] = kBackgroundRows[i][1];
			rgba[i * 4 + 2] = kBackgroundRows[i][2];
			rgba[i * 4 + 3] = 255;
		}
		glGenTextures(1, &background_texture_);
		glBindTexture(GL_TEXTURE_2D, background_texture_);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, (GLsizei)(sizeof(kBackgroundRows) / sizeof(kBackgroundRows[0])),
		             0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	}
	__android_log_print(ANDROID_LOG_INFO, kTag, "scene initialized with %zu textures", textures_.size());
	return true;
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
		return false;
	}
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
	__android_log_print(ANDROID_LOG_INFO, kTag, "firmware text catalog: %zu strings", strings_.size());
	return !strings_.empty();
}

std::string XmbRenderer::TextOr(const std::string &key, const std::string &fallback) const {
	auto found = strings_.find(key);
	return found != strings_.end() ? found->second : fallback;
}

bool XmbRenderer::LoadWave() {
	PspWaveLoadResult result;
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
	__android_log_print(ANDROID_LOG_INFO, kTag,
	                    "wave %dx%d morph=%d tex=%dx%d loop=%.1f @%.1f alpha=%.2f pos=%.2f,%.2f,%.2f scale=%.2f",
	                    wave_.uCount, wave_.vCount, wave_.morphCount, wave_.texW, wave_.texH,
	                    wave_.loopFrames(), wave_.frameRate, wave_.alpha,
	                    wave_.modelPos[0], wave_.modelPos[1], wave_.modelPos[2], wave_.modelScale[0]);
	if (!wave_.rgba.empty()) {
		unsigned long long sum[3] = {0, 0, 0};
		uint8_t high[3] = {0, 0, 0};
		for (size_t i = 0; i + 3 < wave_.rgba.size(); i += 4) {
			for (int c = 0; c < 3; ++c) {
				sum[c] += wave_.rgba[i + c];
				high[c] = std::max(high[c], wave_.rgba[i + c]);
			}
		}
		const size_t pixels = wave_.rgba.size() / 4;
		__android_log_print(ANDROID_LOG_INFO, kTag, "wave texture avg %.0f,%.0f,%.0f max %d,%d,%d",
		                    (double)sum[0] / (double)pixels, (double)sum[1] / (double)pixels,
		                    (double)sum[2] / (double)pixels, high[0], high[1], high[2]);
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
	if (background_texture_ == 0) {
		return;
	}
	const float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
	glBindTexture(GL_TEXTURE_2D, background_texture_);
	DrawQuad(0.0f, 0.0f, kScreenWidth, kScreenHeight, color);
}

void XmbRenderer::DrawStatusBar() {
	if (!font_loaded_) {
		return;
	}
	char clock[32] = {0};
	const std::time_t now = std::time(nullptr);
	std::tm local{};
	localtime_r(&now, &local);
	std::strftime(clock, sizeof(clock), "%d/%m %H:%M", &local);
	const float scale = 0.755f;
	const float width = font_.TextWidth(clock, scale);
	const float color[4] = {1.0f, 1.0f, 1.0f, 0.95f};
	DrawText(clock, 444.0f - width, 18.0f, scale, color, true);
	auto battery = textures_.find("battery");
	if (battery != textures_.end()) {
		DrawTextureCrop(battery->second, 464.0f, 12.0f, 1.0f, color, 0, 0, 44, 16);
	}
}

void XmbRenderer::DrawWave(float dt) {
	if (!wave_loaded_ || wave_program_ == 0) {
		return;
	}
	const float loop = wave_.loopFrames();
	if (loop > 0.0f) {
		wave_frame_ = std::fmod(wave_frame_ + dt * wave_.frameRate, loop);
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
	BuildWaveMvp(mvp);
	glUniformMatrix4fv(wave_mvp_, 1, GL_FALSE, mvp);
	const float alpha = wave_.alpha * wave_.tint[3] * kWaveBrightness;
	glUniform4f(wave_color_, wave_.diffuse[0] * wave_.tint[0] * alpha * kWaveTint[0],
	            wave_.diffuse[1] * wave_.tint[1] * alpha * kWaveTint[1],
	            wave_.diffuse[2] * wave_.tint[2] * alpha * kWaveTint[2], 1.0f);
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
		__android_log_print(ANDROID_LOG_WARN, kTag, "cannot load game icon %s", path.c_str());
	}
	const auto inserted = image_textures_.emplace(path, texture);
	return inserted.first->second.id != 0 ? &inserted.first->second : nullptr;
}

const XmbRenderer::Texture *XmbRenderer::GlyphTexture(uint32_t codepoint) {
	auto found = glyph_textures_.find(codepoint);
	if (found != glyph_textures_.end()) {
		return &found->second;
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
		const FontGlyph *glyph = font_.Glyph(codepoint);
		if (!glyph) {
			continue;
		}
		const Texture *texture = GlyphTexture(codepoint);
		if (texture) {
			const float glyph_x = x + glyph->left * scale;
			const float glyph_y = baseline - glyph->top * scale;
			const float width = glyph->width * scale;
			const float height = glyph->height * scale;
			if (shadow) {
				glBindTexture(GL_TEXTURE_2D, texture->id);
				DrawQuad(glyph_x + 1.0f, glyph_y + 1.0f, width, height, shadow_color);
			}
			DrawQuad(glyph_x, glyph_y, width, height, color);
		}
		x += glyph->advance_h / 64.0f * scale;
	}
	return x - start;
}

void XmbRenderer::KeyPressed(Key key) {
	std::lock_guard<std::mutex> lock(mutex_);
	if (memory_stick_page_) {
		switch (key) {
		case Key::Left:
			if (game_page_index_ > 0) {
				game_page_index_ -= 1;
				PlaySound(0);
			}
			break;
		case Key::Right:
			if (game_page_index_ + 1 < (int)game_titles_.size()) {
				game_page_index_ += 1;
				PlaySound(0);
			}
			break;
		case Key::Confirm:
			if (game_page_index_ >= 0 && game_page_index_ < (int)game_titles_.size()) {
				GameSelected(game_page_index_);
			}
			PlaySound(1);
			break;
		case Key::Back:
			memory_stick_page_ = false;
			PlaySound(2);
			break;
		case Key::Up:
		case Key::Down:
			break;
		}
		return;
	}
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
		if (category_ == 5 && item_ >= 0 && item_ < (int)items_.size() &&
		    items_[item_].texture_name == "item_memstick" && !game_titles_.empty()) {
			memory_stick_page_ = true;
			game_page_index_ = 0;
			PlaySound(1);
			break;
		}
		if (category_ == 5 && item_ >= 0 && item_ < (int)items_.size() && items_[item_].is_game) {
			GameSelected(items_[item_].game_index);
		}
		PlaySound(1);
		break;
	case Key::Back:
		PlaySound(2);
		break;
	}
}

void XmbRenderer::DrawCategoryRow() {
	const char *order[] = {"settings", "extras", "photo", "music", "video", "game", "network", "psn"};
	const bool same = category_ == category_from_ || category_t_ >= 1.0f;
	const float shift_from = kCategoryX[category_from_] - kCategoryAnchorX;
	const float shift_to = kCategoryX[category_] - kCategoryAnchorX;
	const float shift = Lerp(shift_from, shift_to, category_t_);
	for (int index = 0; index < 8; ++index) {
		auto texture = textures_.find(order[index]);
		if (texture == textures_.end()) {
			continue;
		}
		const float x = kCategoryAnchorX + (kCategoryX[index] - kCategoryAnchorX) - shift;
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
		const float icon_color[4] = {1.0f, 1.0f, 1.0f, icon_alpha};
		auto glow = textures_.find("focus_glow");
		if (glow != textures_.end() && index == category_ && icon_scale >= 0.99f) {
			const float glow_color[4] = {1.0f, 1.0f, 1.0f, 0.62f};
			DrawTexture(glow->second, x, kCategoryY, 1.0f, glow_color);
		}
		DrawTexture(texture->second, x, kCategoryY, icon_scale, icon_color);
		if (!font_loaded_) {
			continue;
		}
		const float label_scale = 0.63f;
		const std::string label = TextOr(kCategoryKeys[index], kCategoryFallbacks[index]);
		if (same && index == category_) {
			const float width = font_.TextWidth(label, label_scale);
			const float label_color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
			DrawText(label, x - width * 0.5f, 103.0f, label_scale, label_color, true);
		} else if (!same && index == category_from_) {
			const float width = font_.TextWidth(label, label_scale);
			const float label_color[4] = {1.0f, 1.0f, 1.0f, 1.0f - category_t_};
			DrawText(label, x - width * 0.5f, 103.0f, label_scale, label_color, true);
		} else if (!same && index == category_) {
			const float width = font_.TextWidth(label, label_scale);
			const float label_color[4] = {1.0f, 1.0f, 1.0f, category_t_};
			DrawText(label, x - width * 0.5f, 103.0f, label_scale, label_color, true);
		}
	}
}

void XmbRenderer::SetGames(const std::vector<std::string> &titles,
                           const std::vector<std::string> &icon_paths) {
	std::lock_guard<std::mutex> lock(mutex_);
	game_titles_ = titles;
	game_icons_ = icon_paths;
	if (game_page_index_ >= (int)game_titles_.size()) {
		game_page_index_ = std::max(0, (int)game_titles_.size() - 1);
	}
	if (game_titles_.empty()) {
		memory_stick_page_ = false;
	}
}

std::pair<int, int> XmbRenderer::GetState() {
	std::lock_guard<std::mutex> lock(mutex_);
	return {category_, item_};
}

void XmbRenderer::SetState(int category, int item) {
	std::lock_guard<std::mutex> lock(mutex_);
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
	if (game_titles_.empty()) {
		return;
	}
	game_page_index_ = std::max(0, std::min((int)game_titles_.size() - 1, game_page_index_));
	const float frame_x = 92.0f;
	const float frame_y = 136.0f;
	const float white[4] = {1.0f, 1.0f, 1.0f, 0.92f};
	auto frame = textures_.find("frame_subs");
	if (frame != textures_.end()) {
		DrawTexture(frame->second, frame_x, frame_y, 1.0f, white);
	}
	const std::string &icon_path = game_page_index_ < (int)game_icons_.size()
		? game_icons_[game_page_index_] : std::string();
	if (const Texture *icon = icon_path.empty() ? nullptr : TextureForPath(icon_path)) {
		const float scale = std::min(144.0f / std::max(1, icon->width),
		                             80.0f / std::max(1, icon->height));
		DrawTexture(*icon, frame_x, frame_y, scale, white);
	}
	if (font_loaded_) {
		DrawText(game_titles_[game_page_index_], 174.0f, 145.0f, 0.82f, white, true);
	}
	auto draw_arrow = [&](const char *name, float x, float alpha) {
		auto shadow = textures_.find("arrow_l_shadow");
		if (shadow != textures_.end()) {
			const float shadow_color[4] = {0.0f, 0.0f, 0.0f, 0.55f * alpha};
			DrawTexture(shadow->second, x + 1.0f, frame_y + 1.0f, 1.0f, shadow_color);
		}
		auto arrow = textures_.find(name);
		if (arrow != textures_.end()) {
			const float color[4] = {1.0f, 1.0f, 1.0f, alpha};
			DrawTexture(arrow->second, x, frame_y, 1.0f, color);
		}
	};
	draw_arrow("arrow_l", 6.0f, 1.0f);
	if (game_page_index_ + 1 < (int)game_titles_.size()) {
		draw_arrow("arrow_r", 474.0f, 1.0f);
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
	if (global_alpha <= 0.001f || list.empty()) {
		return;
	}
	for (int index = 0; index < (int)list.size(); ++index) {
		if (index < selected) {
			continue;
		}
		const float y = kSelectedItemY + ((float)index - scroll) * kItemSpacing;
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
			DrawQuad(141.0f, y + 1.0f, 335.0f, 1.0f, rule_color);
		}
	}
}

void XmbRenderer::Draw() {
	if (surface_width_ <= 0 || surface_height_ <= 0 || program_ == 0) {
		return;
	}
	std::lock_guard<std::mutex> lock(mutex_);
	const auto now = std::chrono::steady_clock::now();
	float dt = std::chrono::duration<float>(now - last_draw_).count();
	last_draw_ = now;
	dt = std::min(dt, 0.1f);
	category_t_ = AdvanceTowardsOne(category_t_, dt, kCategoryTau);
	item_t_ = AdvanceTowardsOne(item_t_, dt, kItemTau);
	if (items_fade_t_ < 1.0f) {
		items_fade_t_ = std::min(1.0f, items_fade_t_ + dt / 0.18f);
	}

	const float scale = std::min(surface_width_ / kScreenWidth, surface_height_ / kScreenHeight);
	const int view_width = (int)std::lround(kScreenWidth * scale);
	const int view_height = (int)std::lround(kScreenHeight * scale);
	const int view_x = (surface_width_ - view_width) / 2;
	const int view_y = (surface_height_ - view_height) / 2;

	glViewport(0, 0, surface_width_, surface_height_);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	glViewport(view_x, view_y, view_width, view_height);
	glEnable(GL_SCISSOR_TEST);
	glScissor(view_x, view_y, view_width, view_height);
	// Provisional theme colour; the wave is a separate, unimplemented layer.
	glClearColor(62.0f / 255.0f, 13.0f / 255.0f, 7.0f / 255.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glDisable(GL_SCISSOR_TEST);

	glUseProgram(program_);
	glUniform2f(u_screen_, kScreenWidth, kScreenHeight);
	glUniform1i(u_texture_, 0);
	glActiveTexture(GL_TEXTURE0);
	glBindVertexArray(vertex_array_);
	DrawBackground();

	DrawWave(dt);

	glUseProgram(program_);
	glUniform2f(u_screen_, kScreenWidth, kScreenHeight);
	glUniform1i(u_texture_, 0);
	glActiveTexture(GL_TEXTURE0);
	glBindVertexArray(vertex_array_);

	if (memory_stick_page_) {
		DrawMemoryStickPage();
		DrawStatusBar();
	} else {
		DrawItems();
		DrawCategoryRow();
		DrawStatusBar();
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

void XmbRenderer::Shutdown() {
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
	root_.clear();
}

}  // namespace xmb

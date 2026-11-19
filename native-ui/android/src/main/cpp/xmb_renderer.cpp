#include "xmb_renderer.h"

#include <android/log.h>

#include <cmath>
#include <cstring>
#include <vector>

#include "png_image.h"

namespace xmb {
namespace {

constexpr const char *kTag = "EmuCoreXmb";
constexpr float kScreenWidth = 480.0f;
constexpr float kScreenHeight = 272.0f;

// Measured from the 6.60 shell; provisional while the animations are ported.
constexpr float kCategoryY = 71.0f;
// Anchors with Game selected. Video/Game/Network/PSN were measured directly;
// the four slots to the left are extrapolated by the Video-Game spacing and
// only matter once the row actually moves.
constexpr float kCategoryX[8] = {-318.0f, -232.5f, -147.0f, -61.5f, 24.0f, 109.5f, 195.5f, 274.0f};
constexpr float kItemIconX = 110.0f;
constexpr float kSelectedItemY = 136.0f;
constexpr float kItemSpacing = 66.0f;

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
	};
	for (const auto &asset : assets) {
		LoadTexture(asset.name, asset.path);
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

void XmbRenderer::DrawQuad(float x, float y, float width, float height, float alpha) {
	const float x1 = x + width;
	const float y1 = y + height;
	const float vertices[24] = {
		x, y, 0.0f, 0.0f,
		x1, y, 1.0f, 0.0f,
		x, y1, 0.0f, 1.0f,
		x1, y, 1.0f, 0.0f,
		x1, y1, 1.0f, 1.0f,
		x, y1, 0.0f, 1.0f,
	};
	static float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
	color[3] = alpha;
	glUniform4fv(u_color_, 1, color);
	glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer_);
	glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(vertices), vertices);
	glDrawArrays(GL_TRIANGLES, 0, 6);
}

void XmbRenderer::DrawTexture(const Texture &texture, float center_x, float center_y, float scale, float alpha) {
	if (texture.id == 0) {
		return;
	}
	const float width = texture.width * scale;
	const float height = texture.height * scale;
	glBindTexture(GL_TEXTURE_2D, texture.id);
	DrawQuad(center_x - width * 0.5f, center_y - height * 0.5f, width, height, alpha);
}

void XmbRenderer::Draw() {
	if (surface_width_ <= 0 || surface_height_ <= 0 || program_ == 0) {
		return;
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

	const char *order[] = {"settings", "extras", "photo", "music", "video", "game", "network", "psn"};
	constexpr int selected = 5;
	for (int index = 0; index < 8; ++index) {
		auto texture = textures_.find(order[index]);
		if (texture == textures_.end()) {
			continue;
		}
		const float x = kCategoryX[index];
		const bool is_selected = index == selected;
		DrawTexture(texture->second, x, kCategoryY, is_selected ? 1.0f : 0.8f,
		            is_selected ? 1.0f : 0.55f);
	}

	if (auto saved = textures_.find("item_savedata"); saved != textures_.end()) {
		DrawTexture(saved->second, kItemIconX, kSelectedItemY, 1.0f, 1.0f);
	}
	if (auto stick = textures_.find("item_memstick"); stick != textures_.end()) {
		DrawTexture(stick->second, kItemIconX, kSelectedItemY + kItemSpacing, 1.0f, 0.65f);
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

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

// Measured from the 6.60 shell. The category anchors are absolute x positions
// with Game selected; the row moves by the difference between anchors.
constexpr float kCategoryY = 71.0f;
constexpr float kCategoryAnchorX = 109.5f;
constexpr float kCategoryX[8] = {-318.0f, -232.5f, -147.0f, -61.5f, 24.0f, 109.5f, 195.5f, 274.0f};
const char *kCategoryNames[8] = {
	"Settings", "Extras", "Photo", "Music", "Video", "Game", "Network", "PlayStation Network",
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
		{"item_update", "images/topmenu_icon/EH.png"},
	};
	for (const auto &asset : assets) {
		LoadTexture(asset.name, asset.path);
	}
	items_ = {
		{ "Saved Data Utility", "", "item_savedata" },
		{ "Memory Stick", "Free Space 1459 MB", "item_memstick" },
		{ "PSP Update ver 6.10", "", "item_update" },
	};
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

void XmbRenderer::DrawQuad(float x, float y, float width, float height, const float color[4]) {
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
	glUniform4fv(u_color_, 1, color);
	glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer_);
	glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(vertices), vertices);
	glDrawArrays(GL_TRIANGLES, 0, 6);
}

void XmbRenderer::DrawTexture(const Texture &texture, float center_x, float center_y, float scale,
                              const float color[4]) {
	if (texture.id == 0) {
		return;
	}
	const float width = texture.width * scale;
	const float height = texture.height * scale;
	glBindTexture(GL_TEXTURE_2D, texture.id);
	DrawQuad(center_x - width * 0.5f, center_y - height * 0.5f, width, height, color);
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
			glBindTexture(GL_TEXTURE_2D, texture->id);
			const float glyph_x = x + glyph->left * scale;
			const float glyph_y = baseline - glyph->top * scale;
			const float width = glyph->width * scale;
			const float height = glyph->height * scale;
			if (shadow) {
				DrawQuad(glyph_x + 1.0f, glyph_y + 1.0f, width, height, shadow_color);
			}
			DrawQuad(glyph_x, glyph_y, width, height, color);
		}
		x += glyph->advance_h / 64.0f * scale;
	}
	return x - start;
}

void XmbRenderer::KeyPressed(Key key) {
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
			category_from_ = category_;
			category_ -= 1;
			category_t_ = 0.0f;
			PlaySound(3);
		}
		break;
	case Key::Right:
		if (category_ < 7) {
			category_from_ = category_;
			category_ += 1;
			category_t_ = 0.0f;
			PlaySound(3);
		}
		break;
	case Key::Confirm:
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
		DrawTexture(texture->second, x, kCategoryY, icon_scale, icon_color);
		if (!font_loaded_) {
			continue;
		}
		const float label_scale = 0.66f;
		if (same && index == category_) {
			const float width = font_.TextWidth(kCategoryNames[index], label_scale);
			const float label_color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
			DrawText(kCategoryNames[index], x - width * 0.5f, 105.0f, label_scale, label_color, true);
		} else if (!same && index == category_from_) {
			const float width = font_.TextWidth(kCategoryNames[index], label_scale);
			const float label_color[4] = {1.0f, 1.0f, 1.0f, 1.0f - category_t_};
			DrawText(kCategoryNames[index], x - width * 0.5f, 105.0f, label_scale, label_color, true);
		} else if (!same && index == category_) {
			const float width = font_.TextWidth(kCategoryNames[index], label_scale);
			const float label_color[4] = {1.0f, 1.0f, 1.0f, category_t_};
			DrawText(kCategoryNames[index], x - width * 0.5f, 105.0f, label_scale, label_color, true);
		}
	}
}

void XmbRenderer::DrawItems() {
	const float scroll = Lerp((float)item_from_, (float)item_, item_t_);
	for (int index = 0; index < (int)items_.size(); ++index) {
		if (index < item_ && item_t_ >= 0.99f) {
			continue;  // once the scroll settles, rows above the selection are not drawn
		}
		const float y = kSelectedItemY + ((float)index - scroll) * kItemSpacing;
		if (y < -48.0f || y > kScreenHeight + 48.0f) {
			continue;
		}
		const float focus = 1.0f - std::min(1.0f, std::fabs((float)index - scroll));
		const float alpha = 0.55f + 0.45f * focus;
		const float color[4] = {1.0f, 1.0f, 1.0f, alpha};
		auto texture = textures_.find(items_[index].texture_name);
		if (texture != textures_.end()) {
			DrawTexture(texture->second, kItemIconX, y, 1.0f, color);
		}
		if (!font_loaded_) {
			continue;
		}
		const bool has_subtitle = !items_[index].subtitle.empty();
		const float title_baseline = has_subtitle ? y - 5.0f : y + 4.0f;
		DrawText(items_[index].title, 140.0f, title_baseline, 0.82f, color, true);
		if (has_subtitle) {
			const float subtitle_color[4] = {1.0f, 1.0f, 1.0f, alpha * 0.85f};
			DrawText(items_[index].subtitle, 140.0f, y + 14.0f, 0.62f, subtitle_color, true);
			const float rule_color[4] = {1.0f, 1.0f, 1.0f, alpha * 0.5f};
			DrawQuad(140.0f, y + 1.0f, 336.0f, 1.0f, rule_color);
		}
	}
}

void XmbRenderer::Draw() {
	if (surface_width_ <= 0 || surface_height_ <= 0 || program_ == 0) {
		return;
	}
	const auto now = std::chrono::steady_clock::now();
	float dt = std::chrono::duration<float>(now - last_draw_).count();
	last_draw_ = now;
	dt = std::min(dt, 0.1f);
	category_t_ = AdvanceTowardsOne(category_t_, dt, kCategoryTau);
	item_t_ = AdvanceTowardsOne(item_t_, dt, kItemTau);

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

	DrawItems();
	DrawCategoryRow();

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

#pragma once

#include <GLES3/gl3.h>

#include <chrono>
#include <string>
#include <unordered_map>
#include <vector>

#include "pgf_font.h"

namespace xmb {

// Implemented in xmb_jni.cpp; forwards a sound event to the Kotlin audio host.
void PlaySound(int sound);

// The first native scene. It draws the measured Game-column anchors with the
// original topmenu_icon textures and PGF text. Input drives a local selection
// state with the reference's measured scroll/slide timing; the wave and the
// remaining shell chrome are still separate work.
class XmbRenderer {
public:
	enum class Key {
		Up,
		Down,
		Left,
		Right,
		Confirm,
		Back,
	};

	bool Initialize(const std::string &resource_root);
	void Resize(int width, int height);
	void Draw();
	void KeyPressed(Key key);
	void Shutdown();

private:
	struct Texture {
		GLuint id = 0;
		int width = 0;
		int height = 0;
	};

	struct Item {
		std::string title;
		std::string subtitle;
		std::string texture_name;
	};

	bool BuildProgram();
	bool LoadTexture(const std::string &name, const std::string &relative_path);
	const Texture *GlyphTexture(uint32_t codepoint);
	void DrawTexture(const Texture &texture, float center_x, float center_y, float scale, const float color[4]);
	void DrawQuad(float x, float y, float width, float height, const float color[4]);
	float DrawText(const std::string &text, float x, float baseline, float scale,
	               const float color[4], bool shadow);
	void DrawCategoryRow();
	void DrawItems();

	GLuint program_ = 0;
	GLuint vertex_array_ = 0;
	GLuint vertex_buffer_ = 0;
	GLint u_screen_ = -1;
	GLint u_color_ = -1;
	GLint u_texture_ = -1;
	std::unordered_map<std::string, Texture> textures_;
	std::unordered_map<uint32_t, Texture> glyph_textures_;
	PgfFont font_;
	bool font_loaded_ = false;
	std::string root_;
	int surface_width_ = 0;
	int surface_height_ = 0;
	bool logged_gl_error_ = false;

	std::vector<Item> items_;
	int category_ = 5;
	int category_from_ = 5;
	float category_t_ = 1.0f;
	int item_ = 0;
	int item_from_ = 0;
	float item_t_ = 1.0f;
	std::chrono::steady_clock::time_point last_draw_ = std::chrono::steady_clock::now();
};

}  // namespace xmb

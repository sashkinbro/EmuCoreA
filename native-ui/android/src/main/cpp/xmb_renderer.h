#pragma once

#include <GLES3/gl3.h>

#include <string>
#include <unordered_map>

#include "pgf_font.h"

namespace xmb {

// The first native scene. It draws the measured Game-column anchors with the
// original topmenu_icon textures; text, the wave and the animations are not
// implemented yet and must not be mistaken for parity.
class XmbRenderer {
public:
	bool Initialize(const std::string &resource_root);
	void Resize(int width, int height);
	void Draw();
	void Shutdown();

private:
	struct Texture {
		GLuint id = 0;
		int width = 0;
		int height = 0;
	};

	bool BuildProgram();
	bool LoadTexture(const std::string &name, const std::string &relative_path);
	const Texture *GlyphTexture(uint32_t codepoint);
	void DrawTexture(const Texture &texture, float center_x, float center_y, float scale, const float color[4]);
	void DrawQuad(float x, float y, float width, float height, const float color[4]);
	float DrawText(const std::string &text, float x, float baseline, float scale,
	               const float color[4], bool shadow);

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
};

}  // namespace xmb

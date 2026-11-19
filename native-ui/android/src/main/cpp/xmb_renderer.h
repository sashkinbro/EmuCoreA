#pragma once

#include <GLES3/gl3.h>

#include <string>
#include <unordered_map>

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
	void DrawTexture(const Texture &texture, float center_x, float center_y, float scale, float alpha);
	void DrawQuad(float x, float y, float width, float height, float alpha);

	GLuint program_ = 0;
	GLuint vertex_array_ = 0;
	GLuint vertex_buffer_ = 0;
	GLint u_screen_ = -1;
	GLint u_color_ = -1;
	GLint u_texture_ = -1;
	std::unordered_map<std::string, Texture> textures_;
	std::string root_;
	int surface_width_ = 0;
	int surface_height_ = 0;
	bool logged_gl_error_ = false;
};

}  // namespace xmb

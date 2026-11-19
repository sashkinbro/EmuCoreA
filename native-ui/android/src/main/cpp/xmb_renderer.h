#pragma once

#include <GLES3/gl3.h>

#include <chrono>
#include <array>
#include <mutex>
#include <string>
#include <utility>
#include <unordered_map>
#include <vector>

#include "pgf_font.h"
#include "psp_wave.hpp"

namespace xmb {

// Implemented in xmb_jni.cpp; forwards a sound event to the Kotlin audio host.
void PlaySound(int sound);

// Implemented in xmb_jni.cpp; forwards the selected library game to the host.
void GameSelected(int index);

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
	void SetGames(const std::vector<std::string> &titles, const std::vector<std::string> &icon_paths);
	std::pair<int, int> GetState();
	void SetState(int category, int item);
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
		std::string icon_path;
		bool disabled = false;
		bool is_game = false;
		int game_index = -1;

		Item() = default;
		Item(std::string title, std::string subtitle, std::string texture, bool disabled_item = false)
			: title(std::move(title)), subtitle(std::move(subtitle)),
			  texture_name(std::move(texture)), disabled(disabled_item) {}
	};

	bool BuildProgram();
	bool BuildWaveProgram();
	bool LoadWave();
	void DrawBackground();
	void DrawWave(float dt);
	void DrawStatusBar();
	bool LoadTexture(const std::string &name, const std::string &relative_path);
	const Texture *GlyphTexture(uint32_t codepoint);
	const Texture *ShadowGlyphTexture(uint32_t codepoint);
	const Texture *TextureForPath(const std::string &path);
	void DrawTexture(const Texture &texture, float center_x, float center_y, float scale, const float color[4]);
	void DrawTextureUv(const Texture &texture, float center_x, float center_y, float scale,
	                   const float color[4], float u0, float v0, float u1, float v1);
	void DrawTextureCrop(const Texture &texture, float center_x, float center_y, float scale,
	                     const float color[4], int x, int y, int width, int height);
	void DrawQuad(float x, float y, float width, float height, const float color[4]);
	void DrawQuadUv(float x, float y, float width, float height, const float color[4],
	                float u0, float v0, float u1, float v1);
	float DrawText(const std::string &text, float x, float baseline, float scale,
	               const float color[4], bool shadow);
	void DrawCategoryRow();
	void DrawItems();
	void DrawMemoryStickPage();
	void DrawItemColumn(const std::vector<Item> &list, int selected, float scroll, float global_alpha);

	GLuint program_ = 0;
	GLuint vertex_array_ = 0;
	GLuint vertex_buffer_ = 0;
	GLint u_screen_ = -1;
	GLint u_color_ = -1;
	GLint u_texture_ = -1;
	std::unordered_map<std::string, Texture> textures_;
	std::unordered_map<std::string, Texture> image_textures_;
	std::unordered_map<uint32_t, Texture> glyph_textures_;
	std::unordered_map<uint32_t, Texture> shadow_glyph_textures_;
	PgfFont font_;
	bool font_loaded_ = false;
	PspWave wave_;
	PspWaveTessellator wave_tessellator_;
	bool wave_loaded_ = false;
	GLuint wave_program_ = 0;
	GLuint wave_vao_ = 0;
	GLuint wave_vbo_ = 0;
	GLuint wave_ebo_ = 0;
	GLuint wave_texture_ = 0;
	GLuint background_texture_ = 0;
	GLint wave_mvp_ = -1;
	GLint wave_color_ = -1;
	GLint wave_sampler_ = -1;
	std::vector<float> wave_vertices_;
	float wave_frame_ = 0.0f;
	std::string root_;
	int surface_width_ = 0;
	int surface_height_ = 0;
	bool logged_gl_error_ = false;

	std::vector<Item> items_;
	std::vector<Item> previous_items_;
	std::vector<std::string> game_titles_;
	std::vector<std::string> game_icons_;
	bool memory_stick_page_ = false;
	int game_page_index_ = 0;
	std::array<std::vector<Item>, 8> categories_;
	std::array<int, 8> category_selected_{};
	int previous_item_ = 0;
	float items_fade_t_ = 1.0f;
	int category_ = 5;
	int category_from_ = 5;
	float category_t_ = 1.0f;
	int item_ = 0;
	int item_from_ = 0;
	float item_t_ = 1.0f;
	std::chrono::steady_clock::time_point last_draw_ = std::chrono::steady_clock::now();
	std::mutex mutex_;
};

}  // namespace xmb

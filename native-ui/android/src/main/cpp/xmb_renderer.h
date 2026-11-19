#pragma once

#include <GLES3/gl3.h>

#include <array>
#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "pgf_font.h"
#include "psp_wave.hpp"

namespace xmb {

struct XmbSettings {
	int theme_style = 0;      // 0 = Original, 1 = Classic, 2 = Cookie, 3 = Piano Black
	int color_index = 0;      // 0 = Month (Auto), 1..12 = Jan..Dec, 13..34 = Extended 13-27.bmp Colors
	int bg_mode = 0;          // 0 = Original, 1 = Classic, 2 = Off
	int wave_mode = 0;        // 0 = Original, 1 = Slow, 2 = Static, 3 = Off
	bool key_tone = true;
	bool bgm_enabled = true;
	int date_format = 0;      // 0 = DD/MM/YYYY, 1 = MM/DD/YYYY, 2 = YYYY/MM/DD
	bool time_24h = true;
	bool swap_confirm = false;// false = X Enter / O Back, true = O Enter / X Back
	bool avls_enabled = false;
	int keep_screen_mode = 0; // 0 = Off (Always On), 1 = 2 Min, 2 = 5 Min
	int aspect_mode = 0;      // 0 = 16:9 Full, 1 = 4:3 Original
	int slideshow_speed = 0;  // 0 = Normal, 1 = Slow, 2 = Fast
	int slideshow_effect = 0; // 0 = Slide, 1 = Fade, 2 = Off
	int parental_level = 0;   // 0 = Off, 1..9 = Level 1..9
	int adhoc_channel = 0;    // 0 = Automatic, 1 = Ch 1, 2 = Ch 6, 3 = Ch 11
	bool rss_auto_update = false;
};

struct FallbackGlyph {
	int width = 0;
	int height = 0;
	int left = 0;
	int top = 0;
	int advance_h = 0;
};

// Implemented in xmb_jni.cpp; forwards events and font requests to the Android host.
void PlaySound(int sound);
void GameSelected(int index);
void GameFocused(int index);
void ExitRequested();
void SaveSelected(int index);
void DeleteSaveRequested(int index);
void DeleteGameRequested(int index);
void SettingsChanged(const XmbSettings &settings);
void OpenBrowserRequested(const std::string &url);
bool RasterizeFallbackGlyph(uint32_t codepoint, FallbackGlyph *out_metrics, std::vector<uint8_t> *out_alpha);

struct GameInfo {
	std::string title;
	std::string icon_path;
	std::string pic1_path;
	std::string pic0_path;
	std::string wav_path;
	std::string serial;
	std::string version;
	std::string system_version;
	std::string region;
	std::string size_str;
	std::string date_str;
};

struct SaveStateInfo {
	std::string game_title;
	std::string slot_name;
	std::string date_time;
	std::string size_str;
	std::string preview_path;
	std::string save_path;
	std::string game_path;
};

// The native PSP XMB scene renderer.
class XmbRenderer {
public:
	enum class Key {
		Up,
		Down,
		Left,
		Right,
		Confirm,
		Back,
		Options,
	};

	bool Initialize(const std::string &resource_root);
	void Resize(int width, int height);
	void Draw();
	void KeyPressed(Key key);
	void SetGames(const std::vector<GameInfo> &games);
	void SetSaveStates(const std::vector<SaveStateInfo> &saves);
	void SetGamesLoading(bool loading);
	void SetSavesLoading(bool loading);
	void SetNetworkStatus(bool connected);
	void SetBattery(int level, bool charging);
	void SetStrings(const std::unordered_map<std::string, std::string> &strings);
	void SetStorageInfo(const std::string &free_space_text, const std::string &device_name);
	void ApplySettings(const XmbSettings &settings);
	std::pair<int, int> GetState();
	void SetState(int category, int item);
	void SetExitDialog(bool active);
	bool IsExitDialogActive() const { return exit_dialog_active_; }
	void Shutdown();

private:
	enum class SettingsKind {
		Video = 0,
		Photo = 1,
		System = 2,
		Theme = 3,
		DateTime = 4,
		PowerSave = 5,
		Sound = 6,
		Security = 7,
		Network = 8,
		Rss = 9,
		Display = 10,
	};

	enum class EmptyMediaKind {
		None = 0,
		Photo = 1,
		Music = 2,
		Video = 3,
		Comics = 4,
	};

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
	bool LoadTextCatalog();
	void RebuildCategories();
	int EffectiveColorIndex() const;
	std::string TextOr(const std::string &key, const std::string &fallback) const;
	std::vector<std::string> WrapText(const std::string &text, float scale, float max_width);
	std::array<float, 3> ColorSwatchForChoice(int choice_index) const;
	int SettingsRowCount() const;
	int SettingsPickerCount() const;
	int SettingsCurrentValue() const;
	void ApplySettingsPickerChoice(int value_index);
	void OpenNoticeDialog(const std::string &title, const std::vector<std::string> &lines);
	void DrawBackground();
	void DrawWave(float dt);
	void DrawStatusBar();
	bool LoadTexture(const std::string &name, const std::string &relative_path);
	const Texture *GlyphTexture(uint32_t codepoint);
	const Texture *ShadowGlyphTexture(uint32_t codepoint);
	bool EnsureFallbackGlyph(uint32_t codepoint, FallbackGlyph *out_metrics, const Texture **out_tex);
	const Texture *TextureForPath(const std::string &path);
	void DrawTexture(const Texture &texture, float center_x, float center_y, float scale, const float color[4]);
	void DrawTextureUv(const Texture &texture, float center_x, float center_y, float scale,
	                   const float color[4], float u0, float v0, float u1, float v1);
	void DrawTextureCrop(const Texture &texture, float center_x, float center_y, float scale,
	                     const float color[4], int x, int y, int width, int height);
	void DrawQuad(float x, float y, float width, float height, const float color[4]);
	void DrawSolidQuad(float x, float y, float width, float height, const float color[4]);
	void DrawQuadUv(float x, float y, float width, float height, const float color[4],
	                float u0, float v0, float u1, float v1);
	float MeasureText(const std::string &text, float scale);
	float DrawText(const std::string &text, float x, float baseline, float scale,
	               const float color[4], bool shadow);
	void DrawCategoryRow();
	void DrawItems();
	void DrawMemoryStickPage();
	void DrawSavedDataPage();
	void DrawEmptyMediaPage();
	void DrawSettingsSubPage();
	void DrawSystemInfoDialog();
	void DrawNoticeDialog();
	void DrawItemColumn(const std::vector<Item> &list, int selected, float scroll, float global_alpha);
	void DrawGameIcon(const GameInfo &game, float center_x, float center_y, float width, float height, const float color[4]);
	void DrawDialogFrame(float box_x, float box_y, float box_w, float box_h);
	void DrawDialogHints(float box_x, float box_y, float box_w, float box_h, bool show_enter);
	void DrawYesNoDialog(const std::string &line1, const std::string &line2, int selected);
	void DrawExitDialog();
	void DrawOptionsMenu();
	void DrawDeleteDialog();
	void DrawInfoDialog();
	void DrawToast();
	void ShowToast(const std::string &message);
	void UpdateThemeColor();
	bool IsConfirmKey(Key key) const;
	bool IsBackKey(Key key) const;

	GLuint program_ = 0;
	GLuint vertex_array_ = 0;
	GLuint vertex_buffer_ = 0;
	GLuint white_texture_ = 0;
	GLint u_screen_ = -1;
	GLint u_color_ = -1;
	GLint u_texture_ = -1;
	std::unordered_map<std::string, Texture> textures_;
	std::unordered_map<std::string, Texture> image_textures_;
	std::unordered_map<uint32_t, Texture> glyph_textures_;
	std::unordered_map<uint32_t, Texture> shadow_glyph_textures_;
	std::unordered_map<uint32_t, FallbackGlyph> fallback_glyphs_;
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
	std::array<float, 3> wave_tint_{0.725f, 0.247f, 0.047f};
	std::string root_;
	std::unordered_map<std::string, std::string> strings_;
	std::unordered_map<std::string, std::string> string_overrides_;
	std::string free_space_text_ = "1459 MB";
	std::string device_name_ = "PSP™";
	int surface_width_ = 0;
	int surface_height_ = 0;
	bool logged_gl_error_ = false;
	bool initialized_once_ = false;
	bool games_loading_ = true;
	bool saves_loading_ = true;
	bool network_connected_ = false;

	std::vector<Item> items_;
	std::vector<Item> previous_items_;
	std::vector<GameInfo> games_;
	std::vector<SaveStateInfo> saves_;
	bool memory_stick_page_ = false;
	bool saved_data_page_ = false;
	EmptyMediaKind empty_media_kind_ = EmptyMediaKind::None;
	bool settings_sub_active_ = false;
	SettingsKind settings_kind_ = SettingsKind::Theme;
	int settings_sub_index_ = 0;
	float settings_sub_scroll_ = 0.0f;
	bool settings_picker_open_ = false;
	int settings_picker_index_ = 0;
	bool system_info_dialog_ = false;
	bool info_notice_active_ = false;
	std::string info_notice_title_;
	std::vector<std::string> info_notice_lines_;
	bool restore_confirm_active_ = false;
	int restore_confirm_selected_ = 1;
	bool browser_confirm_active_ = false;
	int browser_confirm_selected_ = 0;
	std::string pending_browser_url_;
	bool pending_focus_notify_ = false;
	bool theme_dirty_ = true;

	int game_page_index_ = 0;
	int save_page_index_ = 0;
	float game_scroll_ = 0.0f;
	float save_scroll_ = 0.0f;
	GLuint pic1_texture_ = 0;
	std::string pic1_loaded_path_;
	GLuint pic0_texture_ = 0;
	std::string pic0_loaded_path_;
	float pic1_alpha_ = 0.0f;
	bool exit_dialog_active_ = false;
	int exit_dialog_selected_ = 1;
	bool options_menu_active_ = false;
	int options_selected_ = 0;
	bool delete_dialog_active_ = false;
	int delete_dialog_selected_ = 1;
	bool info_dialog_active_ = false;
	float boot_t_ = 0.0f;
	bool launching_ = false;
	float launch_t_ = 0.0f;
	XmbSettings settings_;
	int battery_level_ = 100;
	bool battery_charging_ = false;
	float time_elapsed_ = 0.0f;
	bool show_toast_ = false;
	std::string toast_message_;
	float toast_timer_ = 0.0f;
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
	std::recursive_mutex mutex_;
};

}  // namespace xmb

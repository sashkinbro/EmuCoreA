#include <jni.h>

#include <android/log.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "at3_decoder.h"
#include "xmb_renderer.h"

namespace {

constexpr const char *kLogTag = "EmuCoreXmb";
std::string g_resource_root;
xmb::XmbRenderer g_renderer;
JavaVM *g_vm = nullptr;

jclass g_audio_class = nullptr;
jmethodID g_audio_play = nullptr;

jclass g_host_class = nullptr;
jmethodID g_host_game_selected = nullptr;
jmethodID g_host_game_focused = nullptr;
jmethodID g_host_exit_requested = nullptr;
jmethodID g_host_save_selected = nullptr;
jmethodID g_host_delete_save = nullptr;
jmethodID g_host_delete_game = nullptr;
jmethodID g_host_settings_changed = nullptr;
jmethodID g_host_open_browser = nullptr;

jclass g_font_fallback_class = nullptr;
jmethodID g_font_rasterize_glyph = nullptr;

void EnsureHostClass(JNIEnv *env) {
	if (g_host_class == nullptr) {
		jclass local = env->FindClass("com/sbro/emucorea/xmb/XmbHost");
		if (local != nullptr) {
			g_host_class = reinterpret_cast<jclass>(env->NewGlobalRef(local));
			env->DeleteLocalRef(local);
			g_host_game_selected = env->GetStaticMethodID(g_host_class, "onGameSelected", "(I)V");
			g_host_game_focused = env->GetStaticMethodID(g_host_class, "onGameFocused", "(I)V");
			g_host_exit_requested = env->GetStaticMethodID(g_host_class, "onExitRequested", "()V");
			g_host_save_selected = env->GetStaticMethodID(g_host_class, "onSaveSelected", "(I)V");
			g_host_delete_save = env->GetStaticMethodID(g_host_class, "onDeleteSave", "(I)V");
			g_host_delete_game = env->GetStaticMethodID(g_host_class, "onDeleteGame", "(I)V");
			g_host_settings_changed = env->GetStaticMethodID(g_host_class, "onSettingsChanged", "([I)V");
			g_host_open_browser = env->GetStaticMethodID(g_host_class, "onOpenBrowser", "(Ljava/lang/String;)V");
		} else {
			env->ExceptionClear();
		}
	}
}

void CallHostInt(jmethodID method, int index) {
	if (g_vm == nullptr) return;
	JNIEnv *env = nullptr;
	if (g_vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK || env == nullptr) return;
	EnsureHostClass(env);
	if (g_host_class != nullptr && method != nullptr) {
		env->CallStaticVoidMethod(g_host_class, method, index);
		if (env->ExceptionCheck()) env->ExceptionClear();
	}
}

std::vector<std::string> CollectStringArray(JNIEnv *env, jobjectArray array) {
	std::vector<std::string> values;
	if (array == nullptr) return values;
	const jsize count = env->GetArrayLength(array);
	values.reserve((size_t)count);
	for (jsize i = 0; i < count; ++i) {
		auto value = reinterpret_cast<jstring>(env->GetObjectArrayElement(array, i));
		if (value == nullptr) {
			values.emplace_back();
			continue;
		}
		const char *chars = env->GetStringUTFChars(value, nullptr);
		if (chars != nullptr) {
			values.emplace_back(chars);
			env->ReleaseStringUTFChars(value, chars);
		} else {
			values.emplace_back();
		}
		env->DeleteLocalRef(value);
	}
	return values;
}

std::string CollectString(JNIEnv *env, jstring value) {
	if (value == nullptr) return {};
	const char *chars = env->GetStringUTFChars(value, nullptr);
	if (chars == nullptr) return {};
	std::string result(chars);
	env->ReleaseStringUTFChars(value, chars);
	return result;
}

}  // namespace

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *) {
	g_vm = vm;
	return JNI_VERSION_1_6;
}

namespace xmb {

void PlaySound(int sound) {
	if (g_vm == nullptr) return;
	JNIEnv *env = nullptr;
	if (g_vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK || env == nullptr) return;
	if (g_audio_class == nullptr) {
		jclass local = env->FindClass("com/sbro/emucorea/xmb/XmbAudio");
		if (local == nullptr) {
			env->ExceptionClear();
			return;
		}
		g_audio_class = reinterpret_cast<jclass>(env->NewGlobalRef(local));
		env->DeleteLocalRef(local);
		if (g_audio_class != nullptr) {
			g_audio_play = env->GetStaticMethodID(g_audio_class, "play", "(I)V");
		}
	}
	if (g_audio_class != nullptr && g_audio_play != nullptr) {
		env->CallStaticVoidMethod(g_audio_class, g_audio_play, sound);
		if (env->ExceptionCheck()) env->ExceptionClear();
	}
}

void GameSelected(int index) {
	if (g_vm == nullptr) return;
	JNIEnv *env = nullptr;
	if (g_vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK || env == nullptr) return;
	EnsureHostClass(env);
	CallHostInt(g_host_game_selected, index);
}

void GameFocused(int index) {
	if (g_vm == nullptr) return;
	JNIEnv *env = nullptr;
	if (g_vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK || env == nullptr) return;
	EnsureHostClass(env);
	CallHostInt(g_host_game_focused, index);
}

void ExitRequested() {
	if (g_vm == nullptr) return;
	JNIEnv *env = nullptr;
	if (g_vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK || env == nullptr) return;
	EnsureHostClass(env);
	if (g_host_class != nullptr && g_host_exit_requested != nullptr) {
		env->CallStaticVoidMethod(g_host_class, g_host_exit_requested);
		if (env->ExceptionCheck()) env->ExceptionClear();
	}
}

void SaveSelected(int index) {
	if (g_vm == nullptr) return;
	JNIEnv *env = nullptr;
	if (g_vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK || env == nullptr) return;
	EnsureHostClass(env);
	CallHostInt(g_host_save_selected, index);
}

void DeleteSaveRequested(int index) {
	if (g_vm == nullptr) return;
	JNIEnv *env = nullptr;
	if (g_vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK || env == nullptr) return;
	EnsureHostClass(env);
	CallHostInt(g_host_delete_save, index);
}

void DeleteGameRequested(int index) {
	if (g_vm == nullptr) return;
	JNIEnv *env = nullptr;
	if (g_vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK || env == nullptr) return;
	EnsureHostClass(env);
	CallHostInt(g_host_delete_game, index);
}

void SettingsChanged(const XmbSettings &s) {
	if (g_vm == nullptr) return;
	JNIEnv *env = nullptr;
	if (g_vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK || env == nullptr) return;
	EnsureHostClass(env);
	if (g_host_class == nullptr || g_host_settings_changed == nullptr) return;
	jintArray arr = env->NewIntArray(17);
	if (arr == nullptr) return;
	jint values[17] = {
		s.theme_style,
		s.color_index,
		s.bg_mode,
		s.wave_mode,
		s.key_tone ? 1 : 0,
		s.bgm_enabled ? 1 : 0,
		s.date_format,
		s.time_24h ? 1 : 0,
		s.swap_confirm ? 1 : 0,
		s.avls_enabled ? 1 : 0,
		s.keep_screen_mode,
		s.aspect_mode,
		s.slideshow_speed,
		s.slideshow_effect,
		s.parental_level,
		s.adhoc_channel,
		s.rss_auto_update ? 1 : 0,
	};
	env->SetIntArrayRegion(arr, 0, 17, values);
	env->CallStaticVoidMethod(g_host_class, g_host_settings_changed, arr);
	if (env->ExceptionCheck()) env->ExceptionClear();
	env->DeleteLocalRef(arr);
}

void OpenBrowserRequested(const std::string &url) {
	if (g_vm == nullptr) return;
	JNIEnv *env = nullptr;
	if (g_vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK || env == nullptr) return;
	EnsureHostClass(env);
	if (g_host_class == nullptr || g_host_open_browser == nullptr) return;
	jstring jurl = env->NewStringUTF(url.c_str());
	if (jurl == nullptr) return;
	env->CallStaticVoidMethod(g_host_class, g_host_open_browser, jurl);
	if (env->ExceptionCheck()) env->ExceptionClear();
	env->DeleteLocalRef(jurl);
}

bool RasterizeFallbackGlyph(uint32_t codepoint, FallbackGlyph *out_metrics, std::vector<uint8_t> *out_alpha) {
	if (g_vm == nullptr || out_metrics == nullptr || out_alpha == nullptr) return false;
	JNIEnv *env = nullptr;
	if (g_vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK || env == nullptr) return false;
	if (g_font_fallback_class == nullptr) {
		jclass local = env->FindClass("com/sbro/emucorea/xmb/XmbFontFallback");
		if (local == nullptr) {
			env->ExceptionClear();
			return false;
		}
		g_font_fallback_class = reinterpret_cast<jclass>(env->NewGlobalRef(local));
		env->DeleteLocalRef(local);
		g_font_rasterize_glyph = env->GetStaticMethodID(g_font_fallback_class, "rasterizeGlyph", "(I)[B");
	}
	if (g_font_fallback_class == nullptr || g_font_rasterize_glyph == nullptr) return false;

	auto arr = reinterpret_cast<jbyteArray>(
		env->CallStaticObjectMethod(g_font_fallback_class, g_font_rasterize_glyph, (jint)codepoint));
	if (env->ExceptionCheck()) {
		env->ExceptionClear();
		if (arr != nullptr) env->DeleteLocalRef(arr);
		return false;
	}
	if (arr == nullptr) return false;

	const jsize len = env->GetArrayLength(arr);
	if (len < 20) {
		env->DeleteLocalRef(arr);
		return false;
	}
	std::vector<uint8_t> buf((size_t)len);
	env->GetByteArrayRegion(arr, 0, len, reinterpret_cast<jbyte *>(buf.data()));
	env->DeleteLocalRef(arr);

	auto read_i32 = [&](size_t off) -> int32_t {
		uint32_t v = (uint32_t)buf[off] | ((uint32_t)buf[off + 1] << 8) |
		             ((uint32_t)buf[off + 2] << 16) | ((uint32_t)buf[off + 3] << 24);
		return (int32_t)v;
	};
	out_metrics->width = read_i32(0);
	out_metrics->height = read_i32(4);
	out_metrics->left = read_i32(8);
	out_metrics->top = read_i32(12);
	out_metrics->advance_h = read_i32(16);
	const size_t expected = 20u + (size_t)std::max(0, out_metrics->width) * (size_t)std::max(0, out_metrics->height);
	if (buf.size() < expected) return false;
	out_alpha->assign(buf.begin() + 20, buf.begin() + expected);
	return true;
}

}  // namespace xmb

extern "C" JNIEXPORT void JNICALL
Java_com_sbro_emucorea_xmb_XmbNative_nativeSetGames(JNIEnv *env, jclass,
                                                    jobjectArray titles,
                                                    jobjectArray icons,
                                                    jobjectArray pic1s,
                                                    jobjectArray pic0s,
                                                    jobjectArray wavs,
                                                    jobjectArray serials,
                                                    jobjectArray versions,
                                                    jobjectArray sys_versions,
                                                    jobjectArray regions,
                                                    jobjectArray sizes,
                                                    jobjectArray dates) {
	std::vector<std::string> t = CollectStringArray(env, titles);
	std::vector<std::string> ic = CollectStringArray(env, icons);
	std::vector<std::string> p1 = CollectStringArray(env, pic1s);
	std::vector<std::string> p0 = CollectStringArray(env, pic0s);
	std::vector<std::string> wv = CollectStringArray(env, wavs);
	std::vector<std::string> sr = CollectStringArray(env, serials);
	std::vector<std::string> vr = CollectStringArray(env, versions);
	std::vector<std::string> sv = CollectStringArray(env, sys_versions);
	std::vector<std::string> rg = CollectStringArray(env, regions);
	std::vector<std::string> sz = CollectStringArray(env, sizes);
	std::vector<std::string> dt = CollectStringArray(env, dates);

	const size_t count = t.size();
	std::vector<xmb::GameInfo> list;
	list.reserve(count);
	for (size_t i = 0; i < count; ++i) {
		xmb::GameInfo info;
		info.title = t[i];
		info.icon_path = i < ic.size() ? ic[i] : "";
		info.pic1_path = i < p1.size() ? p1[i] : "";
		info.pic0_path = i < p0.size() ? p0[i] : "";
		info.wav_path = i < wv.size() ? wv[i] : "";
		info.serial = i < sr.size() ? sr[i] : "";
		info.version = i < vr.size() ? vr[i] : "";
		info.system_version = i < sv.size() ? sv[i] : "";
		info.region = i < rg.size() ? rg[i] : "";
		info.size_str = i < sz.size() ? sz[i] : "";
		info.date_str = i < dt.size() ? dt[i] : "";
		list.push_back(std::move(info));
	}
	g_renderer.SetGames(list);
}

extern "C" JNIEXPORT void JNICALL
Java_com_sbro_emucorea_xmb_XmbNative_nativeSetSaves(JNIEnv *env, jclass,
                                                    jobjectArray titles,
                                                    jobjectArray slots,
                                                    jobjectArray dates,
                                                    jobjectArray sizes,
                                                    jobjectArray previews,
                                                    jobjectArray saves,
                                                    jobjectArray games) {
	std::vector<std::string> t = CollectStringArray(env, titles);
	std::vector<std::string> sl = CollectStringArray(env, slots);
	std::vector<std::string> dt = CollectStringArray(env, dates);
	std::vector<std::string> sz = CollectStringArray(env, sizes);
	std::vector<std::string> pv = CollectStringArray(env, previews);
	std::vector<std::string> sv = CollectStringArray(env, saves);
	std::vector<std::string> gm = CollectStringArray(env, games);

	const size_t count = t.size();
	std::vector<xmb::SaveStateInfo> list;
	list.reserve(count);
	for (size_t i = 0; i < count; ++i) {
		xmb::SaveStateInfo info;
		info.game_title = t[i];
		info.slot_name = i < sl.size() ? sl[i] : "";
		info.date_time = i < dt.size() ? dt[i] : "";
		info.size_str = i < sz.size() ? sz[i] : "";
		info.preview_path = i < pv.size() ? pv[i] : "";
		info.save_path = i < sv.size() ? sv[i] : "";
		info.game_path = i < gm.size() ? gm[i] : "";
		list.push_back(std::move(info));
	}
	g_renderer.SetSaveStates(list);
}

extern "C" JNIEXPORT void JNICALL
Java_com_sbro_emucorea_xmb_XmbNative_nativeSetBattery(JNIEnv *, jclass, jint level, jboolean charging) {
	g_renderer.SetBattery((int)level, charging == JNI_TRUE);
}

extern "C" JNIEXPORT void JNICALL
Java_com_sbro_emucorea_xmb_XmbNative_nativeSetStrings(JNIEnv *env, jclass,
                                                      jobjectArray keys,
                                                      jobjectArray values) {
	std::vector<std::string> k = CollectStringArray(env, keys);
	std::vector<std::string> v = CollectStringArray(env, values);
	std::unordered_map<std::string, std::string> map;
	const size_t count = std::min(k.size(), v.size());
	map.reserve(count);
	for (size_t i = 0; i < count; ++i) {
		if (!k[i].empty() && !v[i].empty()) {
			map[k[i]] = v[i];
		}
	}
	g_renderer.SetStrings(map);
}

extern "C" JNIEXPORT void JNICALL
Java_com_sbro_emucorea_xmb_XmbNative_nativeSetStorageInfo(JNIEnv *env, jclass,
                                                          jstring free_space_text,
                                                          jstring device_name) {
	g_renderer.SetStorageInfo(CollectString(env, free_space_text), CollectString(env, device_name));
}

extern "C" JNIEXPORT void JNICALL
Java_com_sbro_emucorea_xmb_XmbNative_nativeSetGamesLoading(JNIEnv *, jclass, jboolean loading) {
	g_renderer.SetGamesLoading(loading == JNI_TRUE);
}

extern "C" JNIEXPORT void JNICALL
Java_com_sbro_emucorea_xmb_XmbNative_nativeSetSavesLoading(JNIEnv *, jclass, jboolean loading) {
	g_renderer.SetSavesLoading(loading == JNI_TRUE);
}

extern "C" JNIEXPORT void JNICALL
Java_com_sbro_emucorea_xmb_XmbNative_nativeSetNetworkStatus(JNIEnv *, jclass, jboolean connected) {
	g_renderer.SetNetworkStatus(connected == JNI_TRUE);
}

extern "C" JNIEXPORT void JNICALL
Java_com_sbro_emucorea_xmb_XmbNative_nativeSetSettings(JNIEnv *, jclass,
                                                       jint theme_style,
                                                       jint color_index,
                                                       jint bg_mode,
                                                       jint wave_mode,
                                                       jboolean key_tone,
                                                       jboolean bgm_enabled,
                                                       jint date_format,
                                                       jboolean time_24h,
                                                       jboolean swap_confirm,
                                                       jboolean avls_enabled,
                                                       jint keep_screen_mode,
                                                       jint aspect_mode,
                                                       jint slideshow_speed,
                                                       jint slideshow_effect,
                                                       jint parental_level,
                                                       jint adhoc_channel,
                                                       jboolean rss_auto_update) {
	xmb::XmbSettings s;
	s.theme_style = (int)theme_style;
	s.color_index = (int)color_index;
	s.bg_mode = (int)bg_mode;
	s.wave_mode = (int)wave_mode;
	s.key_tone = (key_tone == JNI_TRUE);
	s.bgm_enabled = (bgm_enabled == JNI_TRUE);
	s.date_format = (int)date_format;
	s.time_24h = (time_24h == JNI_TRUE);
	s.swap_confirm = (swap_confirm == JNI_TRUE);
	s.avls_enabled = (avls_enabled == JNI_TRUE);
	s.keep_screen_mode = (int)keep_screen_mode;
	s.aspect_mode = (int)aspect_mode;
	s.slideshow_speed = (int)slideshow_speed;
	s.slideshow_effect = (int)slideshow_effect;
	s.parental_level = (int)parental_level;
	s.adhoc_channel = (int)adhoc_channel;
	s.rss_auto_update = (rss_auto_update == JNI_TRUE);
	g_renderer.ApplySettings(s);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_sbro_emucorea_xmb_XmbNative_nativeDecodeAt3(JNIEnv *env, jclass, jstring at3_path, jstring wav_path) {
	if (at3_path == nullptr || wav_path == nullptr) return JNI_FALSE;
	const char *at3_chars = env->GetStringUTFChars(at3_path, nullptr);
	const char *wav_chars = env->GetStringUTFChars(wav_path, nullptr);
	bool ok = false;
	if (at3_chars != nullptr && wav_chars != nullptr) {
		ok = xmb::DecodeAt3FileToWavFile(at3_chars, wav_chars);
	}
	if (at3_chars != nullptr) env->ReleaseStringUTFChars(at3_path, at3_chars);
	if (wav_chars != nullptr) env->ReleaseStringUTFChars(wav_path, wav_chars);
	return ok ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_sbro_emucorea_xmb_XmbNative_nativeDecodeAt3Bytes(JNIEnv *env, jclass, jbyteArray at3_bytes, jstring wav_path) {
	if (at3_bytes == nullptr || wav_path == nullptr) return JNI_FALSE;
	const jsize len = env->GetArrayLength(at3_bytes);
	if (len <= 0) return JNI_FALSE;
	jbyte *bytes = env->GetByteArrayElements(at3_bytes, nullptr);
	const char *wav_chars = env->GetStringUTFChars(wav_path, nullptr);
	bool ok = false;
	if (bytes != nullptr && wav_chars != nullptr) {
		std::vector<uint8_t> wav_data;
		if (xmb::DecodeAt3ToWav(reinterpret_cast<const uint8_t *>(bytes), (size_t)len, wav_data)) {
			FILE *f = std::fopen(wav_chars, "wb");
			if (f != nullptr) {
				std::fwrite(wav_data.data(), 1, wav_data.size(), f);
				std::fclose(f);
				ok = true;
			}
		}
	}
	if (bytes != nullptr) env->ReleaseByteArrayElements(at3_bytes, bytes, JNI_ABORT);
	if (wav_chars != nullptr) env->ReleaseStringUTFChars(wav_path, wav_chars);
	return ok ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_sbro_emucorea_xmb_XmbNative_nativeVersion(JNIEnv *env, jclass) {
	return env->NewStringUTF("0.2");
}

extern "C" JNIEXPORT void JNICALL
Java_com_sbro_emucorea_xmb_XmbNative_nativeSetResourceRoot(JNIEnv *env, jclass, jstring path) {
	if (path == nullptr) {
		g_resource_root.clear();
		return;
	}
	g_resource_root = CollectString(env, path);
	__android_log_print(ANDROID_LOG_INFO, kLogTag, "resource root: %s", g_resource_root.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_sbro_emucorea_xmb_XmbNative_nativeResourceRoot(JNIEnv *env, jclass) {
	return env->NewStringUTF(g_resource_root.c_str());
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_sbro_emucorea_xmb_XmbNative_nativeInit(JNIEnv *, jclass) {
	if (g_resource_root.empty()) {
		__android_log_print(ANDROID_LOG_ERROR, kLogTag, "nativeInit without a resource root");
		return JNI_FALSE;
	}
	const bool ok = g_renderer.Initialize(g_resource_root);
	__android_log_print(ANDROID_LOG_INFO, kLogTag, "nativeInit %s", ok ? "ok" : "failed");
	return ok ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_sbro_emucorea_xmb_XmbNative_nativeResize(JNIEnv *, jclass, jint width, jint height) {
	g_renderer.Resize((int)width, (int)height);
}

extern "C" JNIEXPORT void JNICALL
Java_com_sbro_emucorea_xmb_XmbNative_nativeDraw(JNIEnv *, jclass) {
	g_renderer.Draw();
}

extern "C" JNIEXPORT void JNICALL
Java_com_sbro_emucorea_xmb_XmbNative_nativeKey(JNIEnv *, jclass, jint key) {
	switch (key) {
	case 0: g_renderer.KeyPressed(xmb::XmbRenderer::Key::Up); break;
	case 1: g_renderer.KeyPressed(xmb::XmbRenderer::Key::Down); break;
	case 2: g_renderer.KeyPressed(xmb::XmbRenderer::Key::Left); break;
	case 3: g_renderer.KeyPressed(xmb::XmbRenderer::Key::Right); break;
	case 4: g_renderer.KeyPressed(xmb::XmbRenderer::Key::Confirm); break;
	case 5: g_renderer.KeyPressed(xmb::XmbRenderer::Key::Back); break;
	case 6: g_renderer.KeyPressed(xmb::XmbRenderer::Key::Options); break;
	default: break;
	}
}

extern "C" JNIEXPORT jintArray JNICALL
Java_com_sbro_emucorea_xmb_XmbNative_nativeGetState(JNIEnv *env, jclass) {
	const auto state = g_renderer.GetState();
	jint values[2] = {state.first, state.second};
	jintArray result = env->NewIntArray(2);
	if (result != nullptr) {
		env->SetIntArrayRegion(result, 0, 2, values);
	}
	return result;
}

extern "C" JNIEXPORT void JNICALL
Java_com_sbro_emucorea_xmb_XmbNative_nativeSetState(JNIEnv *, jclass, jint category, jint item) {
	g_renderer.SetState((int)category, (int)item);
}

extern "C" JNIEXPORT void JNICALL
Java_com_sbro_emucorea_xmb_XmbNative_nativeSetWirelessStatus(
	JNIEnv *, jclass,
	jboolean wifi_connected,
	jboolean bluetooth_enabled,
	jboolean bluetooth_connected,
	jboolean audio_muted
) {
	g_renderer.SetWirelessStatus(
		wifi_connected == JNI_TRUE,
		bluetooth_enabled == JNI_TRUE,
		bluetooth_connected == JNI_TRUE,
		audio_muted == JNI_TRUE);
}

extern "C" JNIEXPORT void JNICALL
Java_com_sbro_emucorea_xmb_XmbNative_nativeTriggerTransition(JNIEnv *, jclass, jint kind) {
	g_renderer.TriggerTransition((int)kind);
}

extern "C" JNIEXPORT void JNICALL
Java_com_sbro_emucorea_xmb_XmbNative_nativeShutdown(JNIEnv *, jclass) {
	g_renderer.Shutdown();
}

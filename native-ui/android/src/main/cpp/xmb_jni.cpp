#include <jni.h>

#include <android/log.h>

#include <string>

#include "xmb_renderer.h"

namespace {

constexpr const char *kLogTag = "EmuCoreXmb";
std::string g_resource_root;
xmb::XmbRenderer g_renderer;
JavaVM *g_vm = nullptr;
jclass g_audio_class = nullptr;
jmethodID g_audio_play = nullptr;

}  // namespace

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *) {
	g_vm = vm;
	return JNI_VERSION_1_6;
}

namespace xmb {

void PlaySound(int sound) {
	if (g_vm == nullptr) {
		return;
	}
	JNIEnv *env = nullptr;
	if (g_vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK || env == nullptr) {
		return;
	}
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
		if (env->ExceptionCheck()) {
			env->ExceptionClear();
		}
	}
}

}  // namespace xmb

extern "C" JNIEXPORT jstring JNICALL
Java_com_sbro_emucorea_xmb_XmbNative_nativeVersion(JNIEnv *env, jclass) {
	return env->NewStringUTF("0.1");
}

extern "C" JNIEXPORT void JNICALL
Java_com_sbro_emucorea_xmb_XmbNative_nativeSetResourceRoot(JNIEnv *env, jclass, jstring path) {
	if (path == nullptr) {
		g_resource_root.clear();
		return;
	}
	const char *chars = env->GetStringUTFChars(path, nullptr);
	if (chars == nullptr) {
		return;
	}
	g_resource_root.assign(chars);
	env->ReleaseStringUTFChars(path, chars);
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
	default: break;
	}
}

extern "C" JNIEXPORT void JNICALL
Java_com_sbro_emucorea_xmb_XmbNative_nativeShutdown(JNIEnv *, jclass) {
	g_renderer.Shutdown();
}

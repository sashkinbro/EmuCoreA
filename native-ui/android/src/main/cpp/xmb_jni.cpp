#include <jni.h>

#include <android/log.h>

#include <string>

#include "xmb_renderer.h"

namespace {

constexpr const char *kLogTag = "EmuCoreXmb";
std::string g_resource_root;
xmb::XmbRenderer g_renderer;

}  // namespace

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
Java_com_sbro_emucorea_xmb_XmbNative_nativeShutdown(JNIEnv *, jclass) {
	g_renderer.Shutdown();
}

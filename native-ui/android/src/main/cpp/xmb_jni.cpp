#include <jni.h>

#include <android/log.h>

#include <string>

namespace {

constexpr const char *kLogTag = "EmuCoreXmb";
std::string g_resource_root;

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

#include <jni.h>
#include <string>
#include "ServerApp.hpp"

extern "C" JNIEXPORT jboolean JNICALL
Java_com_androserver_NativeLib_startServer(
        JNIEnv* env,
        jobject /* this */,
        jint port,
        jobjectArray root_dirs,
        jstring host,
        jstring authorization,
        jstring public_origin) {

    const auto readString = [env](jstring value) {
        const char* chars = env->GetStringUTFChars(value, nullptr);
        if (!chars) return std::string{};
        std::string result(chars);
        env->ReleaseStringUTFChars(value, chars);
        return result;
    };
    std::vector<std::string> roots;
    const auto count = env->GetArrayLength(root_dirs);
    for (jsize index = 0; index < count; ++index) {
        auto value = static_cast<jbyteArray>(env->GetObjectArrayElement(root_dirs, index));
        if (!value) return JNI_FALSE;
        std::string path(env->GetArrayLength(value), '\0');
        env->GetByteArrayRegion(value, 0, static_cast<jsize>(path.size()), reinterpret_cast<jbyte*>(path.data()));
        env->DeleteLocalRef(value);
        if (env->ExceptionCheck() || path.empty() || path.find('\0') != std::string::npos) return JNI_FALSE;
        roots.push_back(std::move(path));
    }
    const auto address = readString(host);
    const auto credential = readString(authorization);
    const auto origin = readString(public_origin);
    if (env->ExceptionCheck()) return JNI_FALSE;
    return ServerApp::getInstance().start(port, roots, address, credential, origin) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_androserver_NativeLib_stopServer(
        JNIEnv* env,
        jobject /* this */) {

    ServerApp::getInstance().stop();
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_androserver_NativeLib_isServerRunning(
        JNIEnv* env,
        jobject /* this */) {

    return ServerApp::getInstance().isRunning() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_androserver_NativeLib_getServerPort(
        JNIEnv* env,
        jobject /* this */) {

    return static_cast<jint>(ServerApp::getInstance().getPort());
}


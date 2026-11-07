# JNA (used by lazysodium to call into libsodium) looks classes and methods
# up by name at runtime, so R8 must not rename or strip them.
-dontwarn java.awt.**
-keep class com.sun.jna.** { *; }
-keepclassmembers class * extends com.sun.jna.** { public *; }
-keep class com.goterl.lazysodium.** { *; }

# WebRTC (calls): its native library finds these classes, fields and methods
# by name through JNI, and the AAR ships no rules of its own. Without this R8
# strips or renames them and every voice/video call fails in release builds.
-keep class org.webrtc.** { *; }
-dontwarn org.webrtc.**

# WebRTC's native code also looks up this class by name (org/jni_zero/JniInit)
# when a call starts. Without this, R8 removes it and the app exits on every call.
-keep class org.jni_zero.** { *; }
-dontwarn org.jni_zero.**

# Group calls (LiveKit): its copy of WebRTC lives under livekit.org.webrtc and its native
# code finds classes, fields and methods by name through JNI, exactly like org.webrtc above.
-keep class livekit.org.webrtc.** { *; }
-dontwarn livekit.org.webrtc.**
-keep class livekit.org.jni_zero.** { *; }
-dontwarn livekit.org.jni_zero.**
# Audio routing is CallAudio's: LiveKit's audioswitch is left out of the build (see build.gradle.kts)
# and its handler class is never created.
-dontwarn com.twilio.audioswitch.**

# The Tor gateway's JNI entry points (tor-core/src/jni_binding.rs) are looked up by name.
-keep class com.redvaultsystems.tor.NativeTor { native <methods>; *; }

# The per-device-keys engine's JNI entry point (crypto-core/src/jni_binding.rs) is looked up by name.
-keep class com.redvaultsystems.e2ee.NativeE2ee { native <methods>; *; }

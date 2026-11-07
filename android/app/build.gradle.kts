import java.net.URI
import java.util.Properties

plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.compose)
    alias(libs.plugins.kotlin.serialization)
}

val apiBase: String = (project.findProperty("redvaultApiBase") as String?)
    ?: "https://www.redvaultsystems.com/redvaultmessages/api/"
require(apiBase.endsWith("/")) { "redvaultApiBase must end with '/'" }

// Tor routing (docs/tor-routing-plan.md). With an onion API base the app embeds the Tor gateway
// (tor-core, Arti, built here with cargo for each ABI) and sends text, photos, files, Stories and
// voice messages only through it. redvaultTorOnly=true makes a build that refuses any other server:
// it only builds with an onion API base. Neither is on for production until it's approved.
val apiHost: String = URI(apiBase).host ?: ""
val torRouting: Boolean = apiHost.endsWith(".onion") || (project.findProperty("redvaultTorCore") as String?) == "true"
val torOnly: Boolean = (project.findProperty("redvaultTorOnly") as String?) == "true"

// Per-device keys ("protocol 3", docs/multi-device-threat-model.md): DEVELOPMENT ONLY. With
// -PredvaultE2eV3=true the shared Rust engine (crypto-core, JNI feature) is built with cargo for each ABI
// and packaged, and the Device keys screen shows when the server also has E2E_V3_ENABLED. Off by default:
// a normal build has neither the engine nor the screen.
val e2eV3: Boolean = (project.findProperty("redvaultE2eV3") as String?) == "true"
// rv-signal-v1 (docs/signal-integration/): -PredvaultE2eSignal=true adds libsignal (AGPL-3.0-only) to the
// engine. The owner chose AGPL-3.0-only (2026-11-07), but such a build must not be given to anyone until the
// release gates in docs/legal/REDVAULT_AGPL_COMPLIANCE_REPORT.md are cleared (legal review pending).
val e2eSignal: Boolean = (project.findProperty("redvaultE2eSignal") as String?) == "true"
require(!e2eSignal || e2eV3) { "redvaultE2eSignal needs redvaultE2eV3=true" }
val e2eFeatures: String = if (e2eSignal) "jni signal" else "jni"
require(!torOnly || apiHost.endsWith(".onion")) { "redvaultTorOnly needs an onion redvaultApiBase (http://<v3>.onion/.../api/)" }

// AGPL-3.0-only source offer (Settings > Open-Source Licenses and Source Code, see README).
// redvaultSourceCodeUrl: https URL of the source for this version ("{version}" is replaced with
// versionName); empty = not published yet, and the app says so instead of showing a link.
// redvaultSourceContact: where to report source-availability problems (an e-mail address or an https
// page); empty = the contact address in the Terms of Service.
// The same URL rules are in data/model/OpenSourceInfo.kt (isValidSourceUrl), tested in OpenSourceInfoTest.
val sourceCodeUrl: String = ((project.findProperty("redvaultSourceCodeUrl") as String?) ?: "").trim()
if (sourceCodeUrl.isNotEmpty()) {
    require(sourceCodeUrl.none { it.isWhitespace() || it == '"' || it == '\\' }) {
        "redvaultSourceCodeUrl must not contain spaces, quotes or backslashes: '$sourceCodeUrl'"
    }
    val sourceUri = runCatching { URI(sourceCodeUrl.replace("{version}", "0")) }.getOrNull()
    require(sourceUri != null && sourceUri.scheme.equals("https", ignoreCase = true) && !sourceUri.host.isNullOrEmpty()) {
        "redvaultSourceCodeUrl must be an https:// URL with a host (or empty while the source isn't published): '$sourceCodeUrl'"
    }
}
val sourceContact: String = ((project.findProperty("redvaultSourceContact") as String?) ?: "").trim()
require(sourceContact.none { it == '\n' || it == '\r' }) { "redvaultSourceContact must be one line" }
fun javaString(value: String): String = "\"" + value.replace("\\", "\\\\").replace("\"", "\\\"") + "\""

// Release signing is read from android/keystore.properties (git-ignored),
// see android/README.md. Without that file, release builds are unsigned.
val keystoreProps = Properties().apply {
    val file = rootProject.file("keystore.properties")
    if (file.exists()) file.inputStream().use { load(it) }
}

android {
    namespace = "com.redvaultsystems.redvaultmessage"
    compileSdk = 37

    defaultConfig {
        // Permanent once published on Google Play. Never change it.
        applicationId = "com.redvaultsystems.redvaultmessage"
        minSdk = 26
        targetSdk = 37
        versionCode = 8
        versionName = "0.4.0"
        // The Tor-only edition installs next to the normal app instead of replacing it.
        if (torOnly) {
            applicationIdSuffix = ".tor"
            versionNameSuffix = "-tor"
        }

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
        buildConfigField("String", "API_BASE", "\"$apiBase\"")
        buildConfigField("boolean", "TOR_ONLY", torOnly.toString())
        buildConfigField("boolean", "E2E_V3", e2eV3.toString())
        buildConfigField("boolean", "E2E_SIGNAL", e2eSignal.toString())
        buildConfigField("String", "SOURCE_CODE_URL", javaString(sourceCodeUrl))
        buildConfigField("String", "SOURCE_CONTACT", javaString(sourceContact))
    }

    signingConfigs {
        if (keystoreProps.isNotEmpty()) {
            create("release") {
                storeFile = rootProject.file(keystoreProps.getProperty("storeFile"))
                storePassword = keystoreProps.getProperty("storePassword")
                keyAlias = keystoreProps.getProperty("keyAlias")
                keyPassword = keystoreProps.getProperty("keyPassword")
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
            signingConfig = signingConfigs.findByName("release")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    buildFeatures {
        compose = true
        buildConfig = true
    }
    packaging {
        resources.excludes += "/META-INF/{AL2.0,LGPL2.1}"
    }
    // The Terms of Service and Privacy Policy, shared with the web and
    // desktop apps: legal/ at the repository root, bundled as assets.
    sourceSets {
        getByName("main") {
            assets.directories.add("../../legal")
            // The Tor gateway's native library, when this build routes through Tor (buildTorCore below).
            if (torRouting) jniLibs.directories.add(layout.buildDirectory.dir("torJniLibs").get().asFile.path)
            // The Java side of the engine's JNI binding (com.redvaultsystems.e2ee.NativeE2ee) and its library.
            java.directories.add("../../crypto-core/bindings/android")
            if (e2eV3) jniLibs.directories.add(layout.buildDirectory.dir("e2eeJniLibs").get().asFile.path)
        }
        // The vectors the web app's code made, shared with the JVM unit tests (androidTest/.../StoryDeviceTest.kt).
        getByName("androidTest") {
            assets.directories.add("src/test/resources/vectors")
        }
    }
}

dependencies {
    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.activity.compose)
    implementation(libs.androidx.lifecycle.runtime.ktx)
    implementation(libs.androidx.lifecycle.runtime.compose)
    implementation(libs.androidx.lifecycle.viewmodel.compose)
    implementation(platform(libs.androidx.compose.bom))
    implementation(libs.androidx.compose.ui)
    implementation(libs.androidx.compose.ui.graphics)
    implementation(libs.androidx.compose.ui.tooling.preview)
    implementation(libs.androidx.compose.material3)
    debugImplementation(libs.androidx.compose.ui.tooling)

    implementation(libs.retrofit)
    implementation(libs.retrofit.kotlinx.serialization)
    implementation(libs.okhttp)
    implementation(libs.kotlinx.serialization.json)
    implementation(libs.kotlinx.coroutines.android)

    // libsodium: the same library the web client uses, so keys, sealed
    // boxes and ciphertexts are byte-for-byte interchangeable with it.
    // 5.2.0 is the first release whose libsodium.so is 16 KB page-aligned,
    // which Google Play requires; don't go below it.
    implementation("com.goterl:lazysodium-android:5.2.0@aar")
    implementation("net.java.dev.jna:jna:5.17.0@aar")

    // QR codes for in-person safety-number verification. Pure Java, no Google
    // services: generation with zxing core, scanning with the embedded scanner.
    implementation(libs.zxing.embedded)
    implementation(libs.zxing.core)

    // Voice and video calls: Google's open-source WebRTC (BSD), built and
    // published by the webrtc-sdk project. Media is DTLS-SRTP end to end and
    // forced through the TURN relay; the offer/answer is sealed by CallCrypto.
    implementation("io.github.webrtc-sdk:android:144.7559.15")

    // Group calls: LiveKit's Android SDK (Apache 2.0), end-to-end encrypted
    // with a room key only the call's members hold (see calls/GroupCallManager.kt).
    // It ships its own copy of WebRTC under livekit.org.webrtc (the "-prefixed"
    // artifact, with its own liblkjingle_peerconnection_so.so), so it can't clash with the
    // org.webrtc above: the one-to-one calls keep the plain build. Both are pinned to
    // the same WebRTC release so a security fix lands in both at once.
    //
    // Its audio routing (audioswitch) is left out: CallAudio does the routing for every
    // call, and audioswitch would add Bluetooth permissions and a JitPack repository.
    implementation("io.livekit:livekit-android:2.28.2") {
        exclude(group = "com.github.davidliu", module = "audioswitch")
    }
    implementation("io.github.webrtc-sdk:android-prefixed:144.7559.15")

    // Content-free push notifications without Google services (see README > Notifications): UnifiedPush
    // (Apache 2.0) through whichever distributor app the user has, such as ntfy. The connector decrypts
    // the Web Push payload itself (RFC 8291, with Tink); without a distributor the app keeps its own
    // background connection instead (push/PushFallbackService.kt).
    // The connector asks for Tink's JVM build, which brings the full protobuf-java and clashes with the
    // protobuf-javalite LiveKit uses; it only needs Tink's subtle primitives (ECDH, HKDF, AES-GCM), so it
    // gets the same Tink release built for Android (Apache 2.0, protobuf shaded inside) instead.
    implementation(libs.unifiedpush.connector) {
        exclude(group = "com.google.crypto.tink", module = "tink")
    }
    implementation(libs.tink.android)
    implementation(libs.androidx.lifecycle.process)

    testImplementation(libs.junit)
    testImplementation(libs.okhttp.mockwebserver)
    // Unit tests run the app's own crypto on the JVM (e.g. MessageSignatureVectorTest): lazysodium-java is
    // the same library and version as lazysodium-android, the same LazySodium API, with desktop builds of
    // libsodium. It stands in for the Android one, which can't load off a device (see below).
    testImplementation("com.goterl:lazysodium-java:5.2.0")
    testImplementation("net.java.dev.jna:jna:5.17.0")
    androidTestImplementation(libs.androidx.junit)
    androidTestImplementation(libs.androidx.test.runner)
    androidTestImplementation(libs.androidx.test.rules)
}

// On the unit-test classpath lazysodium-java replaces the Android build: both ship the same classes, and the
// Android one's libsodium.so only loads on a device.
configurations.matching { it.name.endsWith("UnitTestRuntimeClasspath") }.configureEach {
    exclude(group = "com.goterl", module = "lazysodium-android")
}

// --- The Tor gateway (tor-core) for Android, built with cargo and the NDK ---
// Properties: redvaultTorAbis (default arm64-v8a,x86_64), redvaultNdkVersion, redvaultCargo.
// Needs the Rust targets (rustup target add aarch64-linux-android x86_64-linux-android ...).
if (torRouting) {
    val torCore = rootProject.file("../tor-core")
    val ndkVersion = (project.findProperty("redvaultNdkVersion") as String?) ?: "29.0.14206865"
    val abis = ((project.findProperty("redvaultTorAbis") as String?) ?: "arm64-v8a,x86_64").split(",").map { it.trim() }.filter { it.isNotEmpty() }
    val triples = mapOf(
        "arm64-v8a" to "aarch64-linux-android",
        "armeabi-v7a" to "armv7-linux-androideabi",
        "x86_64" to "x86_64-linux-android",
        "x86" to "i686-linux-android",
    )
    val sdkDir = Properties().apply {
        val file = rootProject.file("local.properties")
        if (file.exists()) file.inputStream().use { load(it) }
    }.getProperty("sdk.dir") ?: System.getenv("ANDROID_HOME") ?: ""
    val windows = System.getProperty("os.name").lowercase().contains("windows")
    val host = when {
        windows -> "windows-x86_64"
        System.getProperty("os.name").lowercase().contains("mac") -> "darwin-x86_64"
        else -> "linux-x86_64"
    }
    val toolchain = file("$sdkDir/ndk/$ndkVersion/toolchains/llvm/prebuilt/$host/bin")
    val cargo = (project.findProperty("redvaultCargo") as String?)
        ?: File(System.getProperty("user.home"), ".cargo/bin/" + if (windows) "cargo.exe" else "cargo").takeIf { it.exists() }?.path
        ?: "cargo"
    val outDir = layout.buildDirectory.dir("torJniLibs")
    val execs = providers

    val buildTorCore = tasks.register("buildTorCore") {
        group = "build"
        description = "Builds tor-core (the embedded Tor gateway) for $abis with cargo"
        inputs.dir(File(torCore, "src"))
        inputs.files(File(torCore, "Cargo.toml"), File(torCore, "Cargo.lock"))
        outputs.dir(outDir)
        doLast {
            require(toolchain.exists()) { "The NDK $ndkVersion isn't installed at $toolchain (set redvaultNdkVersion)" }
            for (abi in abis) {
                val triple = triples[abi] ?: error("Unknown ABI $abi")
                val clangTriple = if (triple == "armv7-linux-androideabi") "armv7a-linux-androideabi" else triple
                val clang = File(toolchain, "${clangTriple}26-clang" + if (windows) ".cmd" else "").path
                val key = triple.uppercase().replace('-', '_')
                execs.exec {
                    workingDir = torCore
                    commandLine(cargo, "build", "--release", "--locked", "--lib", "--target", triple, "--features", "jni")
                    environment("CARGO_TARGET_${key}_LINKER", clang)
                    environment("CC_${triple.replace('-', '_')}", clang)
                    environment("AR_${triple.replace('-', '_')}", File(toolchain, "llvm-ar" + if (windows) ".exe" else "").path)
                    // 16 KB pages, which Google Play requires.
                    environment("RUSTFLAGS", "-C link-arg=-Wl,-z,max-page-size=16384")
                }.result.get().assertNormalExitValue()
                File(torCore, "target/$triple/release/libredvault_tor.so")
                    .copyTo(outDir.get().dir(abi).file("libredvault_tor.so").asFile, overwrite = true)
            }
        }
    }
    tasks.named("preBuild") { dependsOn(buildTorCore) }
}

// --- The per-device-keys engine (crypto-core), for the device (cargo + the NDK) and for JVM unit tests ---
// Properties: redvaultE2eV3, redvaultE2eAbis (default arm64-v8a,x86_64), redvaultNdkVersion, redvaultCargo.
if (e2eV3) {
    val core = rootProject.file("../crypto-core")
    val ndkVersion = (project.findProperty("redvaultNdkVersion") as String?) ?: "29.0.14206865"
    val abis = ((project.findProperty("redvaultE2eAbis") as String?) ?: "arm64-v8a,x86_64").split(",").map { it.trim() }.filter { it.isNotEmpty() }
    val triples = mapOf(
        "arm64-v8a" to "aarch64-linux-android",
        "armeabi-v7a" to "armv7-linux-androideabi",
        "x86_64" to "x86_64-linux-android",
        "x86" to "i686-linux-android",
    )
    val sdkDir = Properties().apply {
        val file = rootProject.file("local.properties")
        if (file.exists()) file.inputStream().use { load(it) }
    }.getProperty("sdk.dir") ?: System.getenv("ANDROID_HOME") ?: ""
    val windows = System.getProperty("os.name").lowercase().contains("windows")
    val host = when {
        windows -> "windows-x86_64"
        System.getProperty("os.name").lowercase().contains("mac") -> "darwin-x86_64"
        else -> "linux-x86_64"
    }
    val toolchain = file("$sdkDir/ndk/$ndkVersion/toolchains/llvm/prebuilt/$host/bin")
    val cargo = (project.findProperty("redvaultCargo") as String?)
        ?: File(System.getProperty("user.home"), ".cargo/bin/" + if (windows) "cargo.exe" else "cargo").takeIf { it.exists() }?.path
        ?: "cargo"
    val outDir = layout.buildDirectory.dir("e2eeJniLibs")
    val execs = providers

    val buildE2eeCore = tasks.register("buildE2eeCore") {
        group = "build"
        description = "Builds crypto-core (the per-device-keys engine) for $abis with cargo"
        inputs.dir(File(core, "src"))
        inputs.files(File(core, "Cargo.toml"), File(core, "Cargo.lock"))
        outputs.dir(outDir)
        doLast {
            require(toolchain.exists()) { "The NDK $ndkVersion isn't installed at $toolchain (set redvaultNdkVersion)" }
            for (abi in abis) {
                val triple = triples[abi] ?: error("Unknown ABI $abi")
                val clangTriple = if (triple == "armv7-linux-androideabi") "armv7a-linux-androideabi" else triple
                val clang = File(toolchain, "${clangTriple}26-clang" + if (windows) ".cmd" else "").path
                val key = triple.uppercase().replace('-', '_')
                execs.exec {
                    workingDir = core
                    commandLine(cargo, "build", "--release", "--locked", "--lib", "--target", triple, "--features", e2eFeatures)
                    environment("CARGO_TARGET_${key}_LINKER", clang)
                    environment("CC_${triple.replace('-', '_')}", clang)
                    environment("AR_${triple.replace('-', '_')}", File(toolchain, "llvm-ar" + if (windows) ".exe" else "").path)
                    // 16 KB pages, which Google Play requires.
                    environment("RUSTFLAGS", "-C link-arg=-Wl,-z,max-page-size=16384")
                }.result.get().assertNormalExitValue()
                File(core, "target/$triple/release/libredvault_e2ee.so")
                    .copyTo(outDir.get().dir(abi).file("libredvault_e2ee.so").asFile, overwrite = true)
            }
        }
    }
    tasks.named("preBuild") { dependsOn(buildE2eeCore) }
}

// The engine for the JVM unit tests (EngineDeviceTest): built for this computer, found through -Drv.lib.
// Without cargo or the library those tests are skipped, not failed.
run {
    val windows = System.getProperty("os.name").lowercase().contains("windows")
    val cargo = (project.findProperty("redvaultCargo") as String?)
        ?: File(System.getProperty("user.home"), ".cargo/bin/" + if (windows) "cargo.exe" else "cargo").takeIf { it.exists() }?.path
    val core = rootProject.file("../crypto-core")
    val hostLib = File(core, "target/release/" + if (windows) "redvault_e2ee.dll" else if (System.getProperty("os.name").lowercase().contains("mac")) "libredvault_e2ee.dylib" else "libredvault_e2ee.so")
    val buildHost = tasks.register("buildE2eeHostLib") {
        description = "Builds crypto-core for this computer, for the JVM unit tests"
        onlyIf { cargo != null }
        inputs.dir(File(core, "src"))
        inputs.files(File(core, "Cargo.toml"), File(core, "Cargo.lock"))
        outputs.file(hostLib)
        val execs = providers
        doLast {
            execs.exec {
                workingDir = core
                commandLine(cargo!!, "build", "--release", "--locked", "--lib", "--features", e2eFeatures)
            }.result.get().assertNormalExitValue()
        }
    }
    tasks.withType<Test>().configureEach {
        dependsOn(buildHost)
        systemProperty("rv.lib", hostLib.absolutePath)
    }
}

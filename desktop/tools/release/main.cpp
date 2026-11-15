// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// redvault_release: creates the offline release key and signs update
// manifests for the desktop app's auto-update. Run on the release machine
// only; the key file never goes to the server or into git.
//
//   redvault_release keygen [--key FILE] [--embed desktop/app/src/update_key.hpp]
//   redvault_release sign --installer FILE --version X.Y.Z --out latest.json [--notes TEXT] [--key FILE]
//                         [--base-url URL] [--valid-days N]
//   redvault_release sign-linux --appimage FILE --deb FILE --version X.Y.Z --out-dir DIR [--notes TEXT] [--key FILE]
//                         [--base-url URL] [--valid-days N]
//       Writes linux-appimage-latest.json and linux-deb-latest.json into DIR (one passphrase for both).
//   redvault_release verify --manifest latest.json --public-key BASE64 [--current X.Y.Z] [--product NAME]
//
// The key file defaults to %USERPROFILE%\.redvault\release-signing-key.json
// and is encrypted with a passphrase you choose (asked for, never on the
// command line).
//
// A manifest says when it stops being news (--valid-days, 90 by default). After
// that the app warns that a newer version may exist, so sign again before then,
// even if there's no new release: same installer, same version, new date.

#include <chrono>
#include <format>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

#include "redvault/crypto/base64.hpp"
#include "redvault/crypto/bytes.hpp"
#include "redvault/update/update.hpp"

using namespace redvault;
namespace fs = std::filesystem;

namespace {

constexpr const char* kDefaultBaseUrl = "https://www.redvaultsystems.com/redvaultmessages/desktop/";
constexpr std::size_t kMinPassphrase = 12;
constexpr int kDefaultValidDays = 90;

fs::path defaultKeyPath() {
#ifdef _WIN32
    const char* home = std::getenv("USERPROFILE");
#else
    const char* home = std::getenv("HOME");
#endif
    return fs::path(home ? home : ".") / ".redvault" / "release-signing-key.json";
}

std::string readHidden(const std::string& prompt) {
    std::cerr << prompt << std::flush;
    std::string line;
#ifdef _WIN32
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    const bool console = GetConsoleMode(input, &mode) != 0;
    if (console) SetConsoleMode(input, mode & ~static_cast<DWORD>(ENABLE_ECHO_INPUT));
    std::getline(std::cin, line);
    if (console) SetConsoleMode(input, mode);
#else
    termios old{};
    const bool terminal = tcgetattr(STDIN_FILENO, &old) == 0;
    if (terminal) {
        termios quiet = old;
        quiet.c_lflag &= ~static_cast<tcflag_t>(ECHO);
        tcsetattr(STDIN_FILENO, TCSANOW, &quiet);
    }
    std::getline(std::cin, line);
    if (terminal) tcsetattr(STDIN_FILENO, TCSANOW, &old);
#endif
    std::cerr << "\n";
    return line;
}

std::string readFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw update::UpdateError("Can't read " + path.string());
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

void writeFile(const fs::path& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out || !(out << content)) throw update::UpdateError("Can't write " + path.string());
}

std::map<std::string, std::string> options(const std::vector<std::string>& args, std::size_t from) {
    std::map<std::string, std::string> out;
    for (std::size_t i = from; i + 1 < args.size(); i += 2) {
        if (!args[i].starts_with("--")) throw update::UpdateError("Unexpected argument: " + args[i]);
        out[args[i].substr(2)] = args[i + 1];
    }
    if ((args.size() - from) % 2 != 0) throw update::UpdateError("Missing value for " + args.back());
    return out;
}

std::string isoAfterDays(int days) {
    const auto when = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()) + std::chrono::days(days);
    return std::format("{:%Y-%m-%dT%H:%M:%SZ}", when);
}

std::string nowIso() {
    return isoAfterDays(0);
}

std::string sha256Hex(const fs::path& file) {
    // Reuse the app's own check: a release whose size/hash we computed must pass it.
    const std::string bytes = readFile(file);
    return crypto::toHex(crypto::sha256(crypto::ByteView(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size())));
}

int keygen(const std::map<std::string, std::string>& opts) {
    const fs::path keyPath = opts.contains("key") ? fs::path(opts.at("key")) : defaultKeyPath();
    if (fs::exists(keyPath)) {
        std::cerr << "A release key already exists at " << keyPath.string()
                  << ".\nReplacing it would stop every installed copy from accepting updates, so this won't overwrite it.\n";
        return 1;
    }
    std::cerr << "Choose a passphrase for the release key (at least " << kMinPassphrase << " characters).\n"
              << "Without it the key can't sign anything, so store it in your password manager.\n";
    const std::string passphrase = readHidden("Passphrase: ");
    if (passphrase.size() < kMinPassphrase) {
        std::cerr << "Too short.\n";
        return 1;
    }
    if (readHidden("Repeat it: ") != passphrase) {
        std::cerr << "The passphrases don't match.\n";
        return 1;
    }
    std::cerr << "Creating the key (a moment)...\n";
    const auto keys = update::generateSigningKey();
    fs::create_directories(keyPath.parent_path());
    writeFile(keyPath, update::sealSigningKey(keys, passphrase));
    const std::string publicKey = crypto::base64Encode(keys.publicKey);

    if (opts.contains("embed")) {
        const fs::path header = opts.at("embed");
        std::string text = readFile(header);
        static const std::regex kLine(R"(kPublicKeyB64 = "[^"]*";)");
        if (!std::regex_search(text, kLine)) throw update::UpdateError("No kPublicKeyB64 line in " + header.string());
        text = std::regex_replace(text, kLine, "kPublicKeyB64 = \"" + publicKey + "\";");
        writeFile(header, text);
        std::cout << "Public key written into " << header.string() << "\n";
    }
    std::cout << "\nRelease key saved to: " << keyPath.string() << "\n"
              << "Public key: " << publicKey << "\n\n"
              << "Back up that key file AND its passphrase somewhere safe and separate (e.g. an encrypted USB\n"
              << "drive and your password manager). If either is lost, installed copies can never be updated\n"
              << "automatically again. Never put the key file on the server or in git.\n";
    return 0;
}

int sign(const std::map<std::string, std::string>& opts) {
    for (const char* required : {"installer", "version", "out"}) {
        if (!opts.contains(required)) throw update::UpdateError(std::string("sign needs --") + required);
    }
    const fs::path installer = opts.at("installer");
    const fs::path keyPath = opts.contains("key") ? fs::path(opts.at("key")) : defaultKeyPath();
    std::string baseUrl = opts.contains("base-url") ? opts.at("base-url") : kDefaultBaseUrl;
    if (!baseUrl.ends_with('/')) baseUrl += '/';
    if (!update::parseVersion(opts.at("version"))) throw update::UpdateError("Version must look like 1.2.3");
    if (!fs::exists(keyPath)) throw update::UpdateError("No release key at " + keyPath.string() + " (run keygen first)");
    const std::string validDays = opts.contains("valid-days") ? opts.at("valid-days") : std::to_string(kDefaultValidDays);
    if (!std::regex_match(validDays, std::regex("[1-9][0-9]{0,3}"))) throw update::UpdateError("--valid-days must be a number of days, 1 to 9999");

    update::Release release;
    release.version = opts.at("version");
    release.url = baseUrl + installer.filename().string();
    release.sha256 = sha256Hex(installer);
    release.size = fs::file_size(installer);
    release.notes = opts.contains("notes") ? opts.at("notes") : "";
    release.published = nowIso();
    release.expires = isoAfterDays(std::stoi(validDays));

    const auto keys = update::openSigningKey(readFile(keyPath), readHidden("Release key passphrase: "));
    const std::string manifest = update::signManifest(release, keys.secretKey);

    // Check it exactly the way the app will (as seen from the oldest possible version).
    if (!update::verifyManifest(manifest, keys.publicKey, "0.0.0", baseUrl)) {
        throw update::UpdateError("The signed manifest didn't verify (is the version above 0.0.0?)");
    }
    writeFile(opts.at("out"), manifest);
    std::cout << "Signed " << release.version << " -> " << opts.at("out") << "\n"
              << "  installer: " << release.url << "\n"
              << "  sha256:    " << release.sha256 << "\n"
              << "  size:      " << release.size << " bytes\n"
              << "  expires:   " << release.expires << " (sign again before then)\n";
    return 0;
}

/// The Android app's manifest (android-latest.json), signed with the same release key as the desktop's.
int signAndroid(const std::map<std::string, std::string>& opts) {
    for (const char* required : {"apk", "version", "version-code", "out"}) {
        if (!opts.contains(required)) throw update::UpdateError(std::string("sign-android needs --") + required);
    }
    const fs::path apk = opts.at("apk");
    if (apk.extension() != ".apk") throw update::UpdateError("--apk must be an .apk file");
    const fs::path keyPath = opts.contains("key") ? fs::path(opts.at("key")) : defaultKeyPath();
    if (!update::parseVersion(opts.at("version"))) throw update::UpdateError("Version must look like 1.2.3");
    if (!std::regex_match(opts.at("version-code"), std::regex("[1-9][0-9]{0,9}"))) throw update::UpdateError("--version-code must be a positive number");
    if (!fs::exists(keyPath)) throw update::UpdateError("No release key at " + keyPath.string() + " (run keygen first)");
    const std::string validDays = opts.contains("valid-days") ? opts.at("valid-days") : std::to_string(kDefaultValidDays);
    if (!std::regex_match(validDays, std::regex("[1-9][0-9]{0,3}"))) throw update::UpdateError("--valid-days must be a number of days, 1 to 9999");

    update::Release release;
    release.product = std::string(update::kAndroidProduct);
    release.version = opts.at("version");
    release.versionCode = std::stoll(opts.at("version-code"));
    // The name it's published under on the website (the file itself may still be app-release.apk).
    release.url = std::string(kDefaultBaseUrl) + (opts.contains("published-name") ? opts.at("published-name") : apk.filename().string());
    release.sha256 = sha256Hex(apk);
    release.size = fs::file_size(apk);
    release.notes = opts.contains("notes") ? opts.at("notes") : "";
    release.published = nowIso();
    release.expires = isoAfterDays(std::stoi(validDays));

    const auto keys = update::openSigningKey(readFile(keyPath), readHidden("Release key passphrase: "));
    const std::string manifest = update::signManifest(release, keys.secretKey);
    const auto checked = update::verifyAndroidManifest(manifest, keys.publicKey, kDefaultBaseUrl);
    if (checked.versionCode != release.versionCode || checked.sha256 != release.sha256) {
        throw update::UpdateError("The signed Android manifest didn't verify");
    }
    writeFile(opts.at("out"), manifest);
    std::cout << "Signed Android " << release.version << " (versionCode " << release.versionCode << ") -> " << opts.at("out") << "\n"
              << "  apk:     " << release.url << "\n"
              << "  sha256:  " << release.sha256 << "\n"
              << "  size:    " << release.size << " bytes\n"
              << "  expires: " << release.expires << " (sign again before then)\n";
    return 0;
}

/// The Linux desktop app's two manifests, one per way it's installed, both signed with the same release key.
int signLinux(const std::map<std::string, std::string>& opts) {
    for (const char* required : {"appimage", "deb", "version", "out-dir"}) {
        if (!opts.contains(required)) throw update::UpdateError(std::string("sign-linux needs --") + required);
    }
    const fs::path appImage = opts.at("appimage");
    const fs::path deb = opts.at("deb");
    if (appImage.extension() != ".AppImage") throw update::UpdateError("--appimage must be an .AppImage file");
    if (deb.extension() != ".deb") throw update::UpdateError("--deb must be a .deb file");
    const std::string version = opts.at("version");
    if (!update::parseVersion(version)) throw update::UpdateError("Version must look like 1.2.3");
    // Both files carry the version in their names: a mismatch means the wrong file was picked.
    for (const fs::path& file : {appImage, deb}) {
        if (file.filename().string().find(version) == std::string::npos) {
            throw update::UpdateError(file.filename().string() + " doesn't look like version " + version);
        }
    }
    const fs::path keyPath = opts.contains("key") ? fs::path(opts.at("key")) : defaultKeyPath();
    if (!fs::exists(keyPath)) throw update::UpdateError("No release key at " + keyPath.string() + " (run keygen first)");
    const std::string validDays = opts.contains("valid-days") ? opts.at("valid-days") : std::to_string(kDefaultValidDays);
    if (!std::regex_match(validDays, std::regex("[1-9][0-9]{0,3}"))) throw update::UpdateError("--valid-days must be a number of days, 1 to 9999");
    const fs::path outDir = opts.at("out-dir");
    fs::create_directories(outDir);
    std::string baseUrl = opts.contains("base-url") ? opts.at("base-url") : kDefaultBaseUrl;
    if (!baseUrl.ends_with('/')) baseUrl += '/';

    const auto keys = update::openSigningKey(readFile(keyPath), readHidden("Release key passphrase: "));
    struct Item {
        fs::path file;
        std::string_view product;
        const char* manifestName;
    };
    for (const Item& item : {Item{appImage, update::kLinuxAppImageProduct, "linux-appimage-latest.json"},
                             Item{deb, update::kLinuxDebProduct, "linux-deb-latest.json"}}) {
        update::Release release;
        release.product = std::string(item.product);
        release.version = version;
        release.url = baseUrl + item.file.filename().string();
        release.sha256 = sha256Hex(item.file);
        release.size = fs::file_size(item.file);
        release.notes = opts.contains("notes") ? opts.at("notes") : "";
        release.published = nowIso();
        release.expires = isoAfterDays(std::stoi(validDays));
        const std::string manifest = update::signManifest(release, keys.secretKey);
        // Checked exactly the way the app will.
        const auto checked = update::verifyManifest(manifest, keys.publicKey, "0.0.0", baseUrl, item.product);
        if (!checked || checked->sha256 != release.sha256) throw update::UpdateError("The signed manifest didn't verify");
        const fs::path out = outDir / item.manifestName;
        writeFile(out, manifest);
        std::cout << "Signed Linux " << version << " -> " << out.string() << "\n"
                  << "  file:    " << release.url << "\n"
                  << "  sha256:  " << release.sha256 << "\n"
                  << "  size:    " << release.size << " bytes\n"
                  << "  expires: " << release.expires << " (sign again before then)\n";
    }
    return 0;
}

int verify(const std::map<std::string, std::string>& opts) {
    for (const char* required : {"manifest", "public-key"}) {
        if (!opts.contains(required)) throw update::UpdateError(std::string("verify needs --") + required);
    }
    const auto publicKey = crypto::base64Decode(opts.at("public-key"));
    const std::string current = opts.contains("current") ? opts.at("current") : "0.0.0";
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const std::string product = opts.contains("product") ? opts.at("product") : std::string(update::kProduct);
    const auto check = update::checkManifest(readFile(opts.at("manifest")), publicKey, current, kDefaultBaseUrl, now, product);
    if (check.freshness == update::Freshness::Expired) std::cout << "EXPIRED: the app will warn that a newer version may exist.\n";
    if (check.freshness == update::Freshness::NoExpiry) std::cout << "NO EXPIRY: signed before manifests carried a date; the app warns about this too.\n";
    const auto& release = check.release;
    if (!release) {
        std::cout << "Valid signature; not newer than " << current << ".\n";
    } else {
        std::cout << "Valid: " << release->version << " at " << release->url << "\n";
    }
    return 0;
}

void usage() {
    std::cerr << "Usage:\n"
                 "  redvault_release keygen [--key FILE] [--embed desktop/app/src/update_key.hpp]\n"
                 "  redvault_release sign --installer FILE --version X.Y.Z --out latest.json [--notes TEXT] [--key FILE] [--valid-days N]\n"
                 "  redvault_release sign-android --apk FILE --version X.Y.Z --version-code N --out android-latest.json\n"
                 "                   [--published-name RedVault-Messages-X.Y.Z.apk] [--notes TEXT] [--key FILE] [--valid-days N]\n"
                 "  redvault_release sign-linux --appimage FILE --deb FILE --version X.Y.Z --out-dir DIR\n"
                 "                   [--notes TEXT] [--key FILE] [--base-url URL] [--valid-days N]\n"
                 "  redvault_release verify --manifest latest.json --public-key BASE64 [--current X.Y.Z] [--product NAME]\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty()) {
        usage();
        return 2;
    }
    try {
        const auto opts = options(args, 1);
        if (args[0] == "keygen") return keygen(opts);
        if (args[0] == "sign") return sign(opts);
        if (args[0] == "sign-android") return signAndroid(opts);
        if (args[0] == "sign-linux") return signLinux(opts);
        if (args[0] == "verify") return verify(opts);
        usage();
        return 2;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}

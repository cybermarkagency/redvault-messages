// Refusals that protect people on the desktop: which files open with one click, where the app may connect, what the
// app leaves readable on disk. Each attack has a control next to it showing the ordinary case still works.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <filesystem>
#include <fstream>
#include <string>

#include "redvault/model/file_message.hpp"
#include "redvault/net/curl_transport.hpp"
#include "redvault/net/http.hpp"
#include "redvault/util/private_files.hpp"
#include "redvault/util/text.hpp"

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace redvault;
namespace fs = std::filesystem;
using model::file_message::opensInertly;

namespace {

const std::string kOnion = "http://" + std::string(56, 'a') + ".onion/redvaultmessages/api/";

}  // namespace

// --- Files that open with one click (M-17) ---

TEST_CASE("everything that can run, render scripts or install is save-only", "[hardening][files]") {
    const std::string name = GENERATE(as<std::string>{},
        // The old deny-list had these already.
        "setup.exe", "a.com", "a.scr", "a.bat", "a.cmd", "a.ps1", "a.vbs", "a.js", "a.hta", "a.jar", "a.lnk", "a.url", "a.reg", "a.msi",
        "a.iso", "a.dll", "a.chm", "a.library-ms",
        // The ones it missed: Windows.
        "invoice.html", "invoice.htm", "logo.svg", "archive.mht", "data.xml", "work.rdp", "tool.py", "tool.pyw", "a.wsc", "a.sct",
        "dark.theme", "app.appinstaller", "a.cab", "macro.docm", "macro.xlsm",
        // ... and Linux.
        "launcher.desktop", "install.sh", "install.run", "app.AppImage", "app.deb", "app.rpm",
        // Anything nobody thought of.
        "a.xyz", "a.zip", "report.docx", "a.wmv", "a.mhtml", "a.xhtml", "a.shtml", "a.tar.gz");
    CAPTURE(name);
    CHECK_FALSE(opensInertly(name));
}

TEST_CASE("a name can't talk its way onto the allow-list", "[hardening][files]") {
    const std::string name = GENERATE(as<std::string>{},
        "noextension",
        ".txt",                 // dots are stripped, leaving a name with no extension
        "photo.jpg.exe",        // the last extension is what runs
        "photo.JPG.Exe",
        "photo.exe.",           // Windows drops a trailing dot, and so does the sanitizer
        "photo.exe   ",
        "safe.txt:evil.exe",    // an NTFS stream
        "..\\..\\evil.exe",
        "../../evil.sh",
        "good.txt/evil.exe",
        "photo\xE2\x80\xAEgpj.exe");  // right-to-left override: renders as "photo" + "exe.jpg"
    CAPTURE(name);
    CHECK_FALSE(opensInertly(name));
}

TEST_CASE("control: pictures, audio, video, text and PDF open", "[hardening][files]") {
    const std::string name = GENERATE(as<std::string>{},
        "photo.jpg", "photo.jpeg", "PHOTO.PNG", "a.gif", "a.webp", "a.bmp", "a.heic", "a.avif",
        "a.mp3", "a.m4a", "a.aac", "a.ogg", "a.oga", "a.opus", "a.wav", "a.flac",
        "a.mp4", "a.m4v", "a.mov", "a.webm", "a.mkv",
        "notes.txt", "report.final.pdf", "holiday photo.jpg");
    CAPTURE(name);
    CHECK(opensInertly(name));
}

// --- Where the app may connect (L-2) ---

TEST_CASE("plain http is refused except to this computer or an onion service", "[hardening][net]") {
    const std::string url = GENERATE(as<std::string>{},
        "http://www.redvaultsystems.com/redvaultmessages/api/",
        "http://192.168.1.10/api/",
        "http://10.0.0.2:3000/api/",
        "HTTP://example.com/api/",
        "http://127.0.0.1.example.com/api/",  // starts like loopback
        "http://localhost.example.com/api/",
        "http://127.0.0.256/api/",
        "http://127.1/api/",
        "http://[::2]/api/",
        "http://example.onion/api/",          // not a v3 address
        "http://localhost@example.com/api/",  // credentials
        "ftp://localhost/api/",
        "not a url", "");
    CAPTURE(url);
    CHECK_FALSE(net::isAllowedServerUrl(url, false));
}

TEST_CASE("control: https anywhere, loopback and onion over http", "[hardening][net]") {
    const std::string url = GENERATE_COPY(values<std::string>({
        "https://www.redvaultsystems.com/redvaultmessages/api/", "https://staging.example.com/api/", "http://localhost:3000/api/",
        "http://127.0.0.1:8080/api/", "http://127.9.9.9/api/", "http://[::1]:3000/api/", kOnion}));
    CAPTURE(url);
    CHECK(net::isAllowedServerUrl(url, false));
}

TEST_CASE("only a debug build may use plain http elsewhere, and never for a run-time address", "[hardening][net]") {
    CHECK(net::isAllowedServerUrl("http://192.168.1.10/api/", true));   // what a debug build passes for its default
    CHECK_FALSE(net::isAllowedServerUrl("http://192.168.1.10/api/", false));  // what main() passes for --api
    CHECK_FALSE(net::isAllowedServerUrl("ftp://example.com/", true));
}

#ifdef NDEBUG
TEST_CASE("the transport itself won't send plain http off this computer in a release build", "[hardening][net]") {
    net::CurlTransport transport;
    CHECK_THROWS_AS(transport.send(net::HttpRequest{"GET", "http://192.0.2.1/", {}, {}}), net::NetworkError);
}
#endif

// --- Where a checkout link may go (L-6) ---

TEST_CASE("a checkout link must be Stripe's own page over https", "[hardening][net]") {
    const std::string url = GENERATE(as<std::string>{},
        "http://buy.stripe.com/abc",
        "https://evil.example/abc",
        "https://buy.stripe.com.evil.example/abc",
        "https://evil.example/buy.stripe.com/",
        "https://buy.stripe.com@evil.example/abc",
        "https://evilbuy.stripe.com/abc",
        "javascript:alert(1)",
        "file:///etc/passwd",
        "smb://buy.stripe.com/abc",
        "");
    CAPTURE(url);
    CHECK_FALSE(net::isCheckoutUrl(url));
}

TEST_CASE("control: the links the server really hands out", "[hardening][net]") {
    CHECK(net::isCheckoutUrl("https://buy.stripe.com/test_00wcN7boW3tD6lYaQg2B200"));
    CHECK(net::isCheckoutUrl("https://buy.stripe.com/test_premium?client_reference_id=abc"));
    CHECK(net::isCheckoutUrl("https://checkout.stripe.com/c/pay/cs_test_a1"));
}

// --- Where a Stripe link from the earnings pages may go ---

TEST_CASE("a page that takes payment details only opens over https", "[hardening][net]") {
    for (const char* url : {"http://connect.stripe.com/setup", "HTTP://checkout.stripe.com/c/pay", "file:///etc/passwd", "javascript:alert(1)",
                            "smb://evil.example/x", "https://user@evil.example/x", ""}) {
        CAPTURE(url);
        CHECK_FALSE(net::isHttpsUrl(url));
    }
    // Control: the links the server hands out.
    CHECK(net::isHttpsUrl("https://connect.stripe.com/setup/e/acct_1/abc"));
    CHECK(net::isHttpsUrl("https://checkout.stripe.com/c/pay/cs_test_a1"));
}

// --- What's left readable on disk (M-18) ---

#ifndef _WIN32

namespace {

int modeOf(const fs::path& path) {
    struct stat info {};
    REQUIRE(::lstat(path.c_str(), &info) == 0);
    return info.st_mode & 07777;
}

struct TempTree {
    fs::path root = fs::temp_directory_path() / ("redvault-modes-" + util::randomUuid());
    mode_t saved = ::umask(022);  // a normal desktop's umask: what's tested is that the code doesn't depend on it
    TempTree() { fs::create_directories(root); }
    ~TempTree() {
        ::umask(saved);
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
};

}  // namespace

TEST_CASE("private folders and files are this user's alone, whatever the umask", "[hardening][modes]") {
    TempTree t;
    const fs::path deep = t.root / "session-1" / "decrypted" / "abc";
    util::makePrivateDir(deep);
    CHECK(modeOf(t.root / "session-1") == 0700);
    CHECK(modeOf(t.root / "session-1" / "decrypted") == 0700);
    CHECK(modeOf(deep) == 0700);

    // One that exists already is tightened too.
    const fs::path open = t.root / "open";
    fs::create_directories(open);
    REQUIRE(modeOf(open) == 0755);
    util::makePrivateDir(open);
    CHECK(modeOf(open) == 0700);

    const fs::path file = deep / "secret.txt";
    util::makePrivateFile(file);
    std::ofstream(file) << "plain text";
    CHECK(modeOf(file) == 0600);

    // Control: a stream on its own, under this umask, is what the code above is protecting against.
    const fs::path ordinary = t.root / "ordinary.txt";
    std::ofstream(ordinary) << "x";
    CHECK(modeOf(ordinary) == 0644);
}

TEST_CASE("the umask covers everything created after it, by anything", "[hardening][modes]") {
    TempTree t;
    util::restrictNewFiles();
    const fs::path folder = t.root / "made-by-qt";
    fs::create_directories(folder);
    std::ofstream(folder / "Redvault.conf") << "[appLock]\n";
    CHECK(modeOf(folder) == 0700);
    CHECK(modeOf(folder / "Redvault.conf") == 0600);
}

TEST_CASE("crash leftovers and old installs are tightened, and links are left alone", "[hardening][modes]") {
    TempTree t;
    const fs::path outside = t.root / "outside.txt";
    std::ofstream(outside) << "someone else's";
    fs::create_directories(t.root / "outside-folder");

    const fs::path cache = t.root / "cache";
    fs::create_directories(cache / "post-media" / "deep");
    std::ofstream(cache / "post-media" / "deep" / "photo.jpg") << "decrypted";
    std::ofstream(cache / "ring-in.wav") << "tone";
    fs::create_directory_symlink(t.root / "outside-folder", cache / "link-folder");
    fs::create_symlink(outside, cache / "link-file");
    REQUIRE(modeOf(cache) == 0755);
    REQUIRE(modeOf(cache / "post-media" / "deep" / "photo.jpg") == 0644);

    util::tightenTree(cache);
    CHECK(modeOf(cache) == 0700);
    CHECK(modeOf(cache / "post-media") == 0700);
    CHECK(modeOf(cache / "post-media" / "deep") == 0700);
    CHECK(modeOf(cache / "post-media" / "deep" / "photo.jpg") == 0600);
    CHECK(modeOf(cache / "ring-in.wav") == 0600);

    // The targets of links aren't this app's to change.
    CHECK(modeOf(outside) == 0644);
    CHECK(modeOf(t.root / "outside-folder") == 0755);

    // A missing folder (a first run) is no problem, nor is a link as the root.
    util::tightenTree(t.root / "not-there");
    util::tightenTree(cache / "link-folder");
    CHECK(modeOf(t.root / "outside-folder") == 0755);
}

#endif

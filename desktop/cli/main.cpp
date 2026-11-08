// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// redvault_cli: a small command-line check of the core against a real
// server, before there's a desktop window on top of it. Every run signs in
// fresh (the session lives in memory only) and signs out again at the end.
//
//   redvault_cli [--api URL] ping                              reach the server, no account needed
//   redvault_cli [--api URL] chats <handle>                    list your chats
//   redvault_cli [--api URL] read <handle> <n>                 show chat n, decrypted
//   redvault_cli [--api URL] send <handle> <n> <text…>         send a message to chat n
//   redvault_cli [--api URL] sendfile <handle> <n> <path>      send a file to chat n
//   redvault_cli [--api URL] download <handle> <n> [folder]    save chat n's files, decrypted

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

#include "redvault/api/api_client.hpp"
#include "redvault/data/auth_repository.hpp"
#include "redvault/data/conversation_repository.hpp"
#include "redvault/data/file_repository.hpp"
#include "redvault/model/file_message.hpp"
#include "redvault/net/curl_transport.hpp"
#include "redvault/util/private_files.hpp"
#include "redvault/util/text.hpp"

using namespace redvault;
namespace fs = std::filesystem;

namespace {

// Same default as android/gradle.properties.
constexpr const char* kDefaultApiBase = "https://www.redvaultsystems.com/redvaultmessages/api/";

void usage() {
    std::cerr << "Usage:\n"
                 "  redvault_cli [--api URL] ping\n"
                 "  redvault_cli [--api URL] chats <handle>\n"
                 "  redvault_cli [--api URL] read <handle> <chat number>\n"
                 "  redvault_cli [--api URL] send <handle> <chat number> <message text>\n"
                 "  redvault_cli [--api URL] sendfile <handle> <chat number> <file path>\n"
                 "  redvault_cli [--api URL] download <handle> <chat number> [folder]\n"
                 "\n"
                 "The API base defaults to REDVAULT_API_BASE, then "
              << kDefaultApiBase << "\n";
}

/// Reads a line without echoing it, for passwords.
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

std::string readLine(const std::string& prompt) {
    std::cerr << prompt << std::flush;
    std::string line;
    std::getline(std::cin, line);
    return line;
}

data::SelfIdentity signIn(data::AuthRepository& auth, const std::string& handle) {
    const std::string password = readHidden("Password for " + handle + ": ");
    std::cerr << "Signing in (unlocking your key takes a moment)...\n";
    auto outcome = auth.login(handle, password);
    if (auto* identity = std::get_if<data::SelfIdentity>(&outcome)) return std::move(*identity);
    const std::string code = readLine("Two-factor code: ");
    return auth.verifyMfa(handle, password, code);
}

/**
 * Message text comes from other people: drop control characters (except
 * newline and tab) so a message can't smuggle escape sequences that
 * rewrite the terminal.
 */
std::string safeForTerminal(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (const unsigned char c : text) {
        if ((c < 0x20 && c != '\n' && c != '\t') || c == 0x7f) continue;
        out.push_back(static_cast<char>(c));
    }
    return out;
}

/// Unix ms -> "2026-09-24 09:24" (UTC).
std::string shortTime(std::int64_t ms) {
    using namespace std::chrono;
    const auto time = sys_time<milliseconds>{milliseconds{ms}};
    const auto day = floor<days>(time);
    const year_month_day date{day};
    const hh_mm_ss clock{floor<minutes>(time - day)};
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%04d-%02u-%02u %02ld:%02ld", static_cast<int>(date.year()),
                  static_cast<unsigned>(date.month()), static_cast<unsigned>(date.day()),
                  static_cast<long>(clock.hours().count()), static_cast<long>(clock.minutes().count()));
    return buffer;
}

struct Session {
    std::shared_ptr<api::ApiClient> api;
    std::shared_ptr<data::ConversationRepository> conversations;
    data::SelfIdentity me;
};

/// Hydrates every chat, as the chat list does. Chats whose key can't be opened are listed but empty.
std::vector<std::optional<data::ConversationMeta>> loadChats(Session& s) {
    std::vector<std::optional<data::ConversationMeta>> chats;
    for (const auto& dto : s.conversations->listConversations()) {
        try {
            chats.push_back(s.conversations->hydrateMeta(dto, s.me));
        } catch (const crypto::CryptoError&) {
            chats.push_back(std::nullopt);
        }
    }
    return chats;
}

const data::ConversationMeta* pickChat(const std::vector<std::optional<data::ConversationMeta>>& chats,
                                       std::size_t number) {
    if (number < 1 || number > chats.size()) {
        std::cerr << "There's no chat " << number << ". You have " << chats.size() << ".\n";
        return nullptr;
    }
    if (!chats[number - 1]) {
        std::cerr << "This chat's key couldn't be unlocked.\n";
        return nullptr;
    }
    return &*chats[number - 1];
}

int runChats(Session& s) {
    const auto chats = loadChats(s);
    std::cout << "Signed in as " << s.me.handle << " (" << s.me.subscriptionTier << ")\n\n";
    if (chats.empty()) std::cout << "No chats yet.\n";
    for (std::size_t i = 0; i < chats.size(); ++i) {
        std::cout << "  " << (i + 1) << ". ";
        if (!chats[i]) {
            std::cout << "(can't unlock this chat's key)\n";
            continue;
        }
        std::cout << safeForTerminal(chats[i]->handle) << (chats[i]->isGroup ? "  [group]" : "") << "\n";
    }
    if (!chats.empty()) std::cout << "\nShow one with: redvault_cli read " << s.me.handle << " <number>\n";
    return 0;
}

int runRead(Session& s, std::size_t number) {
    const auto chats = loadChats(s);
    const auto* meta = pickChat(chats, number);
    if (meta == nullptr) return 1;

    const auto result = s.conversations->fetchMessages(*meta, s.me.userId);
    std::cout << "== " << safeForTerminal(meta->handle) << " ==";
    if (!meta->isGroup) {
        std::cout << "\nSafety number:";
        for (const auto& group : meta->safetyNumber) std::cout << " " << group;
    }
    std::cout << "\n\n";
    if (result.messages.empty()) std::cout << "No messages.\n";
    for (const auto& message : result.messages) {
        const std::string sender = message.fromMe ? "you" : meta->isGroup ? meta->handleOf(message.senderId) : meta->handle;
        std::cout << shortTime(message.createdAtMs) << "  " << safeForTerminal(sender) << ": ";
        if (message.replyTo) std::cout << "(reply to \"" << safeForTerminal(message.replyTo->preview) << "\") ";
        if (message.kind == data::MessageKind::File && message.file) {
            std::cout << "[file: " << safeForTerminal(message.text) << ", "
                      << model::file_message::formatBytes(message.file->originalSize) << "]";
            if (message.fileStatus && *message.fileStatus != "available") std::cout << " (" << *message.fileStatus << ")";
        } else if (message.kind == data::MessageKind::Undecryptable) {
            std::cout << "[" << message.text << "]";
        } else {
            std::cout << safeForTerminal(message.text);
        }
        for (const auto& reaction : message.reactions) {
            std::cout << "  " << reaction.emoji << (reaction.count > 1 ? std::to_string(reaction.count) : "");
        }
        if (message.fromMe) std::cout << (message.seen ? "  (seen)" : message.delivered ? "  (delivered)" : "");
        std::cout << "\n";
    }
    std::cout << "\n" << result.unread << " unread. (Reading here doesn't send read receipts.)\n";
    return 0;
}

void printSecurity(const data::Message& sent) {
    std::cout << "Sent. Encrypted on this device, relayed as ciphertext";
    if (sent.security && sent.security->verification == data::VerificationStatus::Verified) {
        std::cout << ", and the server's copy matches ours.\n";
    } else if (sent.security && sent.security->verification == data::VerificationStatus::Failed) {
        std::cout << ".\nWARNING: the server's copy differs from what this device sent.\n";
    } else {
        std::cout << " (no verification record from the server).\n";
    }
}

int runSend(Session& s, std::size_t number, const std::string& text) {
    const auto chats = loadChats(s);
    const auto* meta = pickChat(chats, number);
    if (meta == nullptr) return 1;
    printSecurity(s.conversations->send(*meta, text));
    return 0;
}

/// UTF-8 text -> path, without Windows' ANSI code page in between.
fs::path pathFromUtf8(const std::string& utf8) {
    return fs::path(std::u8string(utf8.begin(), utf8.end()));
}

std::string utf8(const fs::path& path) {
    const auto u8 = path.u8string();
    return std::string(u8.begin(), u8.end());
}

/// The command line as UTF-8. On Windows argv is in the ANSI code page, which can't hold every name or emoji.
std::vector<std::string> utf8Arguments(int argc, char** argv) {
#ifdef _WIN32
    (void)argc;
    (void)argv;
    int count = 0;
    LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &count);
    std::vector<std::string> out;
    for (int i = 1; wide != nullptr && i < count; ++i) {
        const int size = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
        std::string arg(static_cast<std::size_t>(size > 0 ? size - 1 : 0), '\0');
        if (size > 1) WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, arg.data(), size, nullptr, nullptr);
        out.push_back(std::move(arg));
    }
    LocalFree(wide);
    return out;
#else
    return std::vector<std::string>(argv + 1, argv + argc);
#endif
}

fs::path cacheDirectory() {
    return fs::temp_directory_path() / ("redvault-cli-" + util::randomUuid());
}

int runSendFile(Session& s, std::size_t number, const fs::path& file) {
    const auto chats = loadChats(s);
    const auto* meta = pickChat(chats, number);
    if (meta == nullptr) return 1;
    data::FileRepository files(s.api, s.conversations, cacheDirectory());
    const auto sent = files.send(*meta, file, [](data::FileSendStage stage) {
        std::cerr << (stage == data::FileSendStage::Encrypting  ? "Encrypting...\n"
                      : stage == data::FileSendStage::Uploading ? "Uploading ciphertext...\n"
                                                                : "Sending...\n");
    });
    files.clearCache();
    printSecurity(sent);
    return 0;
}

/// `name`, or "name (2).ext" and so on if that's taken: never overwrite.
fs::path freePath(const fs::path& folder, const fs::path& name) {
    fs::path candidate = folder / name;
    for (int n = 2; fs::exists(candidate); ++n) {
        candidate = folder / (name.stem().u8string() + u8" (" + fs::path(std::to_string(n)).u8string() + u8")" +
                              name.extension().u8string());
    }
    return candidate;
}

int runDownload(Session& s, std::size_t number, const fs::path& folder) {
    const auto chats = loadChats(s);
    const auto* meta = pickChat(chats, number);
    if (meta == nullptr) return 1;
    const auto result = s.conversations->fetchMessages(*meta, s.me.userId);
    fs::create_directories(folder);

    data::FileRepository files(s.api, s.conversations, cacheDirectory());
    int saved = 0;
    int failed = 0;
    for (const auto& message : result.messages) {
        if (message.kind != data::MessageKind::File || !message.file) continue;
        std::cerr << "Downloading " << safeForTerminal(message.text) << "... ";
        try {
            const auto decrypted = files.download(*message.file);
            const auto target = freePath(folder, decrypted.filename());
            fs::copy_file(decrypted, target);
            std::cerr << "saved as " << target.string() << "\n";
            ++saved;
        } catch (const api::UserFacingError& e) {
            std::cerr << e.what() << "\n";
            ++failed;
        }
    }
    files.clearCache();
    std::cout << saved << " file(s) saved to " << utf8(fs::absolute(folder));
    if (failed > 0) std::cout << ", " << failed << " couldn't be downloaded";
    std::cout << ".\n";
    return failed > 0 ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    util::restrictNewFiles();
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
    std::vector<std::string> args = utf8Arguments(argc, argv);
    std::string apiBase = std::getenv("REDVAULT_API_BASE") ? std::getenv("REDVAULT_API_BASE") : kDefaultApiBase;
    if (args.size() >= 2 && args[0] == "--api") {
        apiBase = args[1];
        args.erase(args.begin(), args.begin() + 2);
    }
    if (args.empty()) {
        usage();
        return 2;
    }
    if (!net::isAllowedServerUrl(apiBase, false)) {
        std::cerr << "Refusing " << apiBase << ": the server address must be https, this computer, or an onion service.\n";
        return 2;
    }

    try {
        auto transport = std::make_shared<net::CurlTransport>();
        auto cookies = std::make_shared<net::SessionCookieJar>();
        auto api = std::make_shared<api::ApiClient>(apiBase, transport, cookies);
        const std::string& command = args[0];

        if (command == "ping" && args.size() == 1) {
            std::cerr << "Contacting " << api->baseUrl() << " ...\n";
            const auto feed = api->transparencyFeed(1);
            std::cout << "OK: the server answered (" << feed.size() << " public transparency event(s) returned).\n";
            return 0;
        }

        const bool known = (command == "chats" && args.size() == 2) || (command == "read" && args.size() == 3) ||
                           (command == "send" && args.size() >= 4) || (command == "sendfile" && args.size() == 4) ||
                           (command == "download" && (args.size() == 3 || args.size() == 4));
        if (!known) {
            usage();
            return 2;
        }
        std::size_t number = 0;
        if (command != "chats") {
            try {
                number = std::stoul(args[2]);
            } catch (const std::exception&) {
                usage();
                return 2;
            }
        }

        data::AuthRepository auth(api);
        auto me = signIn(auth, args[1]);
        // Nothing is saved to disk, so key pins last for this run. A key refused in that time still isn't used.
        auto keys = std::make_shared<data::MessageKeys>(api, data::MessageKeys::PinStore{});
        keys->begin(me);
        Session session{api, std::make_shared<data::ConversationRepository>(api, data::ConversationRepository::HiddenIds{}, keys), std::move(me)};
        int status = 1;
        try {
            if (command == "chats") {
                status = runChats(session);
            } else if (command == "read") {
                status = runRead(session, number);
            } else if (command == "send") {
                std::string text = args[3];
                for (std::size_t i = 4; i < args.size(); ++i) text += " " + args[i];
                status = runSend(session, number, text);
            } else if (command == "sendfile") {
                status = runSendFile(session, number, pathFromUtf8(args[3]));
            } else {
                status = runDownload(session, number, pathFromUtf8(args.size() == 4 ? args[3] : "."));
            }
        } catch (...) {
            auth.logout();
            throw;
        }
        auth.logout();
        return status;
    } catch (const api::ApiError& e) {
        std::cerr << "Error: " << e.what() << " [" << (e.status() ? std::to_string(e.status()) + " " : "")
                  << e.code() << "]\n";
    } catch (const api::UserFacingError& e) {
        std::cerr << "Error: " << e.what() << "\n";
    } catch (const crypto::CryptoError& e) {
        std::cerr << "Error: couldn't unlock your key (" << e.what() << ")\n";
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
    }
    return 1;
}

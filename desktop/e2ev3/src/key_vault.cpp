// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "redvault/e2ev3/key_vault.hpp"

#include <cstring>
#include <stdexcept>
#include <system_error>

#include "fs_util.hpp"
#include "redvault/crypto/base64.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wincrypt.h>
#else
#include <dlfcn.h>
#endif

namespace redvault::e2ev3 {

namespace {

using crypto::Bytes;
using crypto::ByteView;
using crypto::SecretBytes;

/// A file name for any id: no path characters, nothing to guess from.
std::string fileStem(const std::string& id) {
    return crypto::toHex(crypto::sha256(crypto::toBytes(id))).substr(0, 40);
}

class FileVault final : public KeyVault {
public:
    explicit FileVault(std::filesystem::path dir) : dir_(std::move(dir)) {}
    Protection protection() const override { return Protection::File; }
    void put(const std::string& id, ByteView secret) override {
        fsutil::writeAtomic(path(id), crypto::base64Encode(secret));
    }
    std::optional<SecretBytes> get(const std::string& id) override {
        const auto text = fsutil::readAll(path(id));
        if (!text) return std::nullopt;
        try {
            const Bytes raw = crypto::base64Decode(*text);
            return SecretBytes::copyOf(raw);
        } catch (const crypto::CryptoError&) {
            return std::nullopt;
        }
    }
    void erase(const std::string& id) override {
        std::error_code ec;
        std::filesystem::remove(path(id), ec);
    }

private:
    std::filesystem::path path(const std::string& id) const { return dir_ / (fileStem(id) + ".key"); }
    std::filesystem::path dir_;
};

#ifdef _WIN32

class DpapiVault final : public KeyVault {
public:
    explicit DpapiVault(std::filesystem::path dir) : dir_(std::move(dir)) {}
    Protection protection() const override { return Protection::Dpapi; }

    void put(const std::string& id, ByteView secret) override {
        DATA_BLOB in{static_cast<DWORD>(secret.size()), const_cast<BYTE*>(secret.data())};
        Bytes entropyBytes = entropy(id);
        DATA_BLOB extra{static_cast<DWORD>(entropyBytes.size()), entropyBytes.data()};
        DATA_BLOB out{};
        if (!CryptProtectData(&in, L"Redvault device key", &extra, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
            throw std::runtime_error("Windows couldn't protect the device key (DPAPI).");
        }
        const std::string blob(reinterpret_cast<const char*>(out.pbData), out.cbData);
        LocalFree(out.pbData);
        fsutil::writeAtomic(path(id), blob);
    }

    std::optional<SecretBytes> get(const std::string& id) override {
        const auto blob = fsutil::readAll(path(id));
        if (!blob || blob->empty()) return std::nullopt;
        DATA_BLOB in{static_cast<DWORD>(blob->size()), reinterpret_cast<BYTE*>(const_cast<char*>(blob->data()))};
        Bytes entropyBytes = entropy(id);
        DATA_BLOB extra{static_cast<DWORD>(entropyBytes.size()), entropyBytes.data()};
        DATA_BLOB out{};
        if (!CryptUnprotectData(&in, nullptr, &extra, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return std::nullopt;
        SecretBytes secret = SecretBytes::copyOf(ByteView(out.pbData, out.cbData));
        SecureZeroMemory(out.pbData, out.cbData);
        LocalFree(out.pbData);
        return secret;
    }

    void erase(const std::string& id) override {
        std::error_code ec;
        std::filesystem::remove(path(id), ec);
    }

private:
    // The id is the DPAPI "optional entropy": a blob copied to another device id doesn't unprotect.
    static Bytes entropy(const std::string& id) { return crypto::toBytes(std::string("redvault-v3-storage-key") + '\0' + id); }
    std::filesystem::path path(const std::string& id) const { return dir_ / (fileStem(id) + ".dpapi"); }
    std::filesystem::path dir_;
};

#else

// libsecret's C API, declared here and resolved with dlopen so the app has no
// link-time dependency on it: a machine without GNOME Keyring or KWallet (or
// without the library) simply takes the file fallback.
struct SecretSchemaAttribute {
    const char* name;
    int type;  // SECRET_SCHEMA_ATTRIBUTE_STRING
};
struct SecretSchema {
    const char* name;
    int flags;  // SECRET_SCHEMA_NONE
    SecretSchemaAttribute attributes[32];
    int reserved;
    void* reserved1;
    void* reserved2;
    void* reserved3;
    void* reserved4;
    void* reserved5;
    void* reserved6;
    void* reserved7;
};
struct GError {
    unsigned domain;
    int code;
    char* message;
};

struct Libsecret {
    void* handle = nullptr;
    void* glib = nullptr;
    int (*store)(const SecretSchema*, const char*, const char*, const char*, void*, GError**, ...) = nullptr;
    char* (*lookup)(const SecretSchema*, void*, GError**, ...) = nullptr;
    int (*clear)(const SecretSchema*, void*, GError**, ...) = nullptr;
    void (*freePassword)(char*) = nullptr;
    void (*freeError)(GError*) = nullptr;
    SecretSchema schema{};

    bool load() {
        handle = dlopen("libsecret-1.so.0", RTLD_LAZY | RTLD_LOCAL);
        if (!handle) return false;
        glib = dlopen("libglib-2.0.so.0", RTLD_LAZY | RTLD_LOCAL);
        store = reinterpret_cast<decltype(store)>(dlsym(handle, "secret_password_store_sync"));
        lookup = reinterpret_cast<decltype(lookup)>(dlsym(handle, "secret_password_lookup_sync"));
        clear = reinterpret_cast<decltype(clear)>(dlsym(handle, "secret_password_clear_sync"));
        freePassword = reinterpret_cast<decltype(freePassword)>(dlsym(handle, "secret_password_free"));
        if (glib) freeError = reinterpret_cast<decltype(freeError)>(dlsym(glib, "g_error_free"));
        if (!store || !lookup || !clear || !freePassword) return false;
        schema.name = "com.redvaultsystems.devicekey";
        schema.flags = 0;
        schema.attributes[0] = {"account", 0};
        schema.attributes[1] = {nullptr, 0};
        return true;
    }
    void drop(GError* error) const {
        if (error && freeError) freeError(error);
    }
};

class SecretServiceVault final : public KeyVault {
public:
    explicit SecretServiceVault(std::shared_ptr<Libsecret> lib) : lib_(std::move(lib)) {}
    Protection protection() const override { return Protection::SecretService; }

    /// Whether a Secret Service answers: stores and clears one throwaway item.
    bool probe() {
        try {
            put("probe", crypto::toBytes("probe"));
            erase("probe");
            return true;
        } catch (const std::exception&) {
            return false;
        }
    }

    void put(const std::string& id, ByteView secret) override {
        const std::string value = crypto::base64Encode(secret);
        GError* error = nullptr;
        const int ok = lib_->store(&lib_->schema, "default", "Redvault device key", value.c_str(), nullptr, &error, "account", id.c_str(),
                                   static_cast<const char*>(nullptr));
        if (!ok) {
            lib_->drop(error);
            throw std::runtime_error("The Secret Service didn't accept the device key.");
        }
    }

    std::optional<SecretBytes> get(const std::string& id) override {
        GError* error = nullptr;
        char* value = lib_->lookup(&lib_->schema, nullptr, &error, "account", id.c_str(), static_cast<const char*>(nullptr));
        lib_->drop(error);
        if (!value) return std::nullopt;
        std::optional<SecretBytes> out;
        try {
            out = SecretBytes::copyOf(crypto::base64Decode(value));
        } catch (const crypto::CryptoError&) {
        }
        std::memset(value, 0, std::strlen(value));
        lib_->freePassword(value);
        return out;
    }

    void erase(const std::string& id) override {
        GError* error = nullptr;
        lib_->clear(&lib_->schema, nullptr, &error, "account", id.c_str(), static_cast<const char*>(nullptr));
        lib_->drop(error);
    }

private:
    std::shared_ptr<Libsecret> lib_;
};

#endif

}  // namespace

std::unique_ptr<KeyVault> makeFileVault(const std::filesystem::path& dir) {
    return std::make_unique<FileVault>(dir);
}

std::unique_ptr<KeyVault> makeDpapiVault([[maybe_unused]] const std::filesystem::path& dir) {
#ifdef _WIN32
    return std::make_unique<DpapiVault>(dir);
#else
    return nullptr;
#endif
}

std::unique_ptr<KeyVault> makeSecretServiceVault() {
#ifdef _WIN32
    return nullptr;
#else
    auto lib = std::make_shared<Libsecret>();
    if (!lib->load()) return nullptr;
    auto vault = std::make_unique<SecretServiceVault>(lib);
    if (!vault->probe()) return nullptr;
    return vault;
#endif
}

std::unique_ptr<KeyVault> makeDefaultVault(const std::filesystem::path& dir) {
#ifdef _WIN32
    return makeDpapiVault(dir);
#else
    if (auto secretService = makeSecretServiceVault()) return secretService;
    return makeFileVault(dir);
#endif
}

std::string describeProtection(Protection protection) {
    switch (protection) {
        case Protection::Dpapi:
            return "Protected by Windows (DPAPI) for your Windows account. Anything running as you can still use it.";
        case Protection::SecretService:
            return "Kept in your system keyring (Secret Service), unlocked with your login.";
        case Protection::File:
            return "No system keyring was found, so the key is a file only your user can read. Anyone who can read your files can read it.";
    }
    return {};
}

}  // namespace redvault::e2ev3

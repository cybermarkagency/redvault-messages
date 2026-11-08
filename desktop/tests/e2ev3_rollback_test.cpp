// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>

#include "redvault/crypto/bytes.hpp"
#include "redvault/e2ev3/device.hpp"
#include "redvault/e2ev3/device_store.hpp"
#include "redvault/e2ev3/key_vault.hpp"
#include "redvault/util/text.hpp"

// Rollback detection for the stored device state (docs/signal-integration/07-step1-default-path.md
// section 4): a counter authenticated inside the state file and mirrored in its own vault entry.

namespace e2 = redvault::e2ev3;
using e2::Json;

namespace {

namespace fs = std::filesystem;

struct TempDir {
    fs::path path;
    TempDir() {
        path = fs::temp_directory_path() / ("rv-e2ev3-rollback-" + redvault::util::randomUuid());
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void writeFile(const fs::path& p, const std::string& data) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << data;
}

/// The one account folder under the store's directory (named by a hash of the user id).
fs::path accountFolder(const fs::path& devices) {
    for (const auto& entry : fs::directory_iterator(devices)) {
        if (entry.is_directory()) return entry.path();
    }
    throw std::runtime_error("no account folder");
}

std::uint64_t counterInFile(const fs::path& devices) {
    const Json j = Json::parse(readFile(accountFolder(devices) / "state"));
    return j.at("counter").get<std::uint64_t>();
}

/// A file vault that can be told to refuse writing the counter's entry (a crash between the two writes).
class FlakyVault final : public e2::KeyVault {
public:
    explicit FlakyVault(const fs::path& dir) : inner_(e2::makeFileVault(dir)) {}
    e2::Protection protection() const override { return inner_->protection(); }
    void put(const std::string& id, redvault::crypto::ByteView secret) override {
        if (failCounter && id.ends_with("/counter")) throw std::runtime_error("vault unavailable");
        inner_->put(id, secret);
    }
    std::optional<redvault::crypto::SecretBytes> get(const std::string& id) override { return inner_->get(id); }
    void erase(const std::string& id) override { inner_->erase(id); }
    bool failCounter = false;

private:
    std::unique_ptr<e2::KeyVault> inner_;
};

struct Store {
    TempDir dir;
    fs::path devices = dir.path / "devices";
    fs::path keys = dir.path / "keys";
    std::shared_ptr<FlakyVault> vault = std::make_shared<FlakyVault>(keys);
    std::unique_ptr<e2::DeviceStore> store = std::make_unique<e2::DeviceStore>(devices, vault);
    /// A new store over the same folders: what the app sees at its next start.
    void restart() { store = std::make_unique<e2::DeviceStore>(devices, vault); }
};

}  // namespace

TEST_CASE("rollback: every save raises a counter inside the authenticated state and in its own vault entry", "[e2ev3][store][rollback]") {
    Store s;
    const auto made = s.store->create("alice");
    s.store->saveState("alice", made.deviceId, "STATE-1");
    REQUIRE(counterInFile(s.devices) == 1);
    s.store->saveState("alice", made.deviceId, "STATE-2");
    REQUIRE(counterInFile(s.devices) == 2);
    // The file holds the state and its counter, never the storage key.
    REQUIRE(readFile(accountFolder(s.devices) / "state").find(made.storageKey) == std::string::npos);
    // The counter is mirrored in a vault entry of its own, beside the storage key (2 files in the file vault).
    REQUIRE(s.vault->get(e2::DeviceStore::counterId("alice", made.deviceId)).has_value());
    std::size_t entries = 0;
    for ([[maybe_unused]] const auto& entry : fs::directory_iterator(s.keys)) ++entries;
    REQUIRE(entries == 2);

    // Equal counters: the state opens, after a restart too.
    s.restart();
    const auto loaded = s.store->load("alice");
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->state == "STATE-2");
    REQUIRE(s.store->events("alice").empty());
    // And the counter carries on from where it was.
    s.store->saveState("alice", made.deviceId, "STATE-3");
    REQUIRE(counterInFile(s.devices) == 3);
}

TEST_CASE("rollback: an older copy of the state is refused and recorded once", "[e2ev3][store][rollback]") {
    Store s;
    const auto made = s.store->create("alice");
    s.store->saveState("alice", made.deviceId, "STATE-1");
    const fs::path stateFile = accountFolder(s.devices) / "state";
    const std::string oldCopy = readFile(stateFile);
    s.store->saveState("alice", made.deviceId, "STATE-2");
    s.store->saveState("alice", made.deviceId, "STATE-3");

    // Restoring the state folder alone doesn't bring the vault's counter back.
    writeFile(stateFile, oldCopy);
    s.restart();
    REQUIRE_FALSE(s.store->load("alice").has_value());
    auto events = s.store->events("alice");
    REQUIRE(events.size() == 1);
    REQUIRE(events[0].kind == "state_rolled_back");
    REQUIRE(events[0].deviceId == made.deviceId);
    REQUIRE(events[0].stateCounter == 1);
    REQUIRE(events[0].storedCounter == 3);
    REQUIRE(events[0].atMs > 0);

    // Still refused at the next start, and not recorded again.
    s.restart();
    REQUIRE_FALSE(s.store->load("alice").has_value());
    REQUIRE(s.store->events("alice").size() == 1);

    // The device can only be linked again: a new one replaces it, and the record of why stays.
    const auto again = s.store->create("alice");
    REQUIRE(again.deviceId != made.deviceId);
    REQUIRE_FALSE(s.vault->get(e2::DeviceStore::counterId("alice", made.deviceId)).has_value());
    s.store->saveState("alice", again.deviceId, "NEW-1");
    REQUIRE(counterInFile(s.devices) == 1);
    s.restart();
    REQUIRE(s.store->load("alice").has_value());
    REQUIRE(s.store->events("alice").size() == 1);
}

TEST_CASE("rollback: editing the counter or the state is detected", "[e2ev3][store][rollback]") {
    Store s;
    const auto made = s.store->create("alice");
    s.store->saveState("alice", made.deviceId, "STATE-1");
    const fs::path stateFile = accountFolder(s.devices) / "state";
    const std::string good = readFile(stateFile);

    SECTION("a raised counter with the old MAC") {
        Json j = Json::parse(good);
        j["counter"] = 99;
        writeFile(stateFile, j.dump());
    }
    SECTION("another state under the same counter and MAC") {
        Json j = Json::parse(good);
        j["state"] = "SOMETHING-ELSE";
        writeFile(stateFile, j.dump());
    }
    SECTION("the counter taken out") {
        Json j = Json::parse(good);
        j.erase("counter");
        writeFile(stateFile, j.dump());
    }
    s.restart();
    REQUIRE_FALSE(s.store->load("alice").has_value());
    const auto events = s.store->events("alice");
    REQUIRE(events.size() == 1);
    REQUIRE(events[0].kind == "state_unverified");

    // Put back as it was, it opens.
    writeFile(stateFile, good);
    s.restart();
    REQUIRE(s.store->load("alice").has_value());
}

TEST_CASE("rollback: a state ahead of the vault (a crash between the writes) is accepted and the vault raised", "[e2ev3][store][rollback]") {
    Store s;
    const auto made = s.store->create("alice");
    s.store->saveState("alice", made.deviceId, "STATE-1");
    const fs::path stateFile = accountFolder(s.devices) / "state";
    const std::string copyAt1 = readFile(stateFile);

    // The state is written first: when the counter's write fails, the state is already saved with the new counter.
    s.vault->failCounter = true;
    REQUIRE_NOTHROW(s.store->saveState("alice", made.deviceId, "STATE-2"));
    REQUIRE(counterInFile(s.devices) == 2);
    s.vault->failCounter = false;

    s.restart();
    const auto loaded = s.store->load("alice");
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->state == "STATE-2");
    REQUIRE(s.store->events("alice").empty());

    // Loading raised the vault's counter to 2, so the copy from counter 1 is now an older copy.
    writeFile(stateFile, copyAt1);
    s.restart();
    REQUIRE_FALSE(s.store->load("alice").has_value());
    REQUIRE(s.store->events("alice").at(0).kind == "state_rolled_back");
}

TEST_CASE("rollback: the vault's counter exists from the moment the device is made", "[e2ev3][store][rollback]") {
    Store s;
    const auto made = s.store->create("alice");
    REQUIRE(s.vault->get(e2::DeviceStore::counterId("alice", made.deviceId)).has_value());
}

TEST_CASE("rollback: a state with a counter but no vault counter is refused", "[e2ev3][store][rollback]") {
    Store s;
    const auto made = s.store->create("alice");
    s.store->saveState("alice", made.deviceId, "STATE-1");
    const fs::path stateFile = accountFolder(s.devices) / "state";
    const std::string copyAt1 = readFile(stateFile);
    s.store->saveState("alice", made.deviceId, "STATE-2");

    SECTION("the vault's counter deleted and an older state restored") {
        s.vault->erase(e2::DeviceStore::counterId("alice", made.deviceId));
        writeFile(stateFile, copyAt1);
    }
    SECTION("the vault's counter deleted, the newest state left as it is") {
        s.vault->erase(e2::DeviceStore::counterId("alice", made.deviceId));
    }
    SECTION("the vault's counter replaced by one that doesn't verify") {
        s.vault->put(e2::DeviceStore::counterId("alice", made.deviceId), redvault::crypto::randomBytes(40));
    }
    s.restart();
    REQUIRE_FALSE(s.store->load("alice").has_value());
    const auto events = s.store->events("alice");
    REQUIRE(events.size() == 1);
    REQUIRE(events[0].kind == "counter_missing");
    // Not initialised behind the refusal: it stays refused.
    s.restart();
    REQUIRE_FALSE(s.store->load("alice").has_value());
    REQUIRE(s.store->events("alice").size() == 1);
}

TEST_CASE("rollback: a state saved before counters opens once, then can't be restored over a newer one", "[e2ev3][store][rollback]") {
    Store s;
    const auto made = s.store->create("alice");
    // What an earlier version wrote: the bare state, and no counter in the vault.
    s.vault->erase(e2::DeviceStore::counterId("alice", made.deviceId));
    const fs::path stateFile = accountFolder(s.devices) / "state";
    writeFile(stateFile, "LEGACY-STATE");
    s.restart();
    const auto loaded = s.store->load("alice");
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->state == "LEGACY-STATE");
    // The vault's counter was initialised from it, and counting starts.
    REQUIRE(s.vault->get(e2::DeviceStore::counterId("alice", made.deviceId)).has_value());
    REQUIRE(s.store->events("alice").empty());

    s.store->saveState("alice", made.deviceId, "STATE-1");
    REQUIRE(counterInFile(s.devices) == 1);
    writeFile(stateFile, "LEGACY-STATE");
    s.restart();
    REQUIRE_FALSE(s.store->load("alice").has_value());
    REQUIRE(s.store->events("alice").at(0).kind == "state_rolled_back");
}

TEST_CASE("rollback: a real engine device restored from an older copy isn't opened (not linked)", "[e2ev3][store][rollback]") {
    TempDir dir;
    e2::Backend backend;
    auto vault = std::shared_ptr<e2::KeyVault>(e2::makeFileVault(dir.path / "keys"));
    backend.store = std::make_shared<e2::DeviceStore>(dir.path / "devices", vault);
    auto device = e2::V3Device::create("alice", "createDevice", Json::object(), backend);
    REQUIRE(device);
    const fs::path stateFile = accountFolder(dir.path / "devices") / "state";
    const std::string oldCopy = readFile(stateFile);
    const std::uint64_t before = counterInFile(dir.path / "devices");
    REQUIRE(device->must("prekeys", Json{{"count", 2}}).value("ok", false));
    REQUIRE(counterInFile(dir.path / "devices") > before);
    device.reset();

    backend.store = std::make_shared<e2::DeviceStore>(dir.path / "devices", vault);
    REQUIRE(e2::V3Device::open("alice", backend) != nullptr);

    writeFile(stateFile, oldCopy);
    backend.store = std::make_shared<e2::DeviceStore>(dir.path / "devices", vault);
    REQUIRE(e2::V3Device::open("alice", backend) == nullptr);
    REQUIRE(backend.store->events("alice").at(0).kind == "state_rolled_back");
}

#ifdef _WIN32
TEST_CASE("rollback: on Windows the counter is its own DPAPI entry", "[e2ev3][store][rollback][vault]") {
    TempDir dir;
    e2::DeviceStore store(dir.path / "devices", std::shared_ptr<e2::KeyVault>(e2::makeDpapiVault(dir.path / "keys")));
    const auto made = store.create("alice");
    store.saveState("alice", made.deviceId, "STATE-1");
    std::size_t blobs = 0;
    for (const auto& entry : fs::directory_iterator(dir.path / "keys")) {
        if (entry.path().extension() == ".dpapi") ++blobs;
    }
    REQUIRE(blobs == 2);
    const std::string copyAt1 = readFile(accountFolder(dir.path / "devices") / "state");
    store.saveState("alice", made.deviceId, "STATE-2");
    writeFile(accountFolder(dir.path / "devices") / "state", copyAt1);
    e2::DeviceStore again(dir.path / "devices", std::shared_ptr<e2::KeyVault>(e2::makeDpapiVault(dir.path / "keys")));
    REQUIRE_FALSE(again.load("alice").has_value());
}
#endif

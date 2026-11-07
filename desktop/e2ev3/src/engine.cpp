#include "redvault/e2ev3/engine.hpp"

#include <cstdint>
#include <string>

#include "redvault_e2ee.h"

namespace redvault::e2ev3 {

Json runEngine(const Json& request) {
    // The request holds the storage key: wiped when this call is done.
    std::string text = request.dump();
    std::size_t length = 0;
    std::uint8_t* raw = rv_e2ee_call(reinterpret_cast<const std::uint8_t*>(text.data()), text.size(), &length);
    volatile char* wipe = text.data();
    for (std::size_t i = 0; i < text.size(); ++i) wipe[i] = 0;
    Json response = Json::parse(reinterpret_cast<const char*>(raw), reinterpret_cast<const char*>(raw) + length, nullptr, false);
    rv_e2ee_free(raw, length);
    if (response.is_discarded() || !response.is_object()) {
        return Json{{"ok", false}, {"error", "internal"}, {"message", "the engine's answer wasn't JSON"}};
    }
    return response;
}

}  // namespace redvault::e2ev3

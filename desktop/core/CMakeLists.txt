# Qt-free core: crypto, HTTP, API client/DTOs and repositories, shared by the
# desktop UI and the tests. Keeping it free of Qt means it builds and tests
# with nothing but a compiler, libsodium, libcurl and nlohmann/json.
add_library(redvault_core STATIC
    src/bytes.cpp
    src/base64.cpp
    src/e2e_crypto.cpp
    src/commitment.cpp
    src/file_crypto.cpp
    src/recovery_key.cpp
    src/safety_number_qr.cpp
    src/call_crypto.cpp
    src/group_call_crypto.cpp
    src/group_invite_crypto.cpp
    src/channel_crypto.cpp
    src/community_crypto.cpp
    src/story_crypto.cpp
    src/post_crypto.cpp
    src/vault_crypto.cpp
    src/drive_crypto.cpp
    src/share_crypto.cpp
    src/net/http.cpp
    src/net/cookie_jar.cpp
    src/net/curl_transport.cpp
    src/api/dtos.cpp
    src/api/api_client.cpp
    src/api/live_api.cpp
    src/api/group_call_api.cpp
    src/api/group_invite_api.cpp
    src/api/channel_api.cpp
    src/api/community_api.cpp
    src/api/story_api.cpp
    src/api/earnings_api.cpp
    src/api/drive_api.cpp
    src/api/share_api.cpp
    src/data/account_repository.cpp
    src/data/auth_repository.cpp
    src/data/sign_in.cpp
    src/data/chat_models.cpp
    src/data/conversation_repository.cpp
    src/data/key_trust.cpp
    src/data/message_keys.cpp
    src/data/community_keys.cpp
    src/data/friend_repository.cpp
    src/data/file_repository.cpp
    src/data/transfer.cpp
    src/data/drive_repository.cpp
    src/data/share_repository.cpp
    src/data/file_key_repository.cpp
    src/data/file_lock.cpp
    src/data/story_drafts.cpp
    src/data/vault_repository.cpp
    src/data/outbox.cpp
    src/model/rich_message.cpp
    src/model/file_message.cpp
    src/model/text_format.cpp
    src/model/plan_choices.cpp
    src/model/group_call_link.cpp
    src/model/group_call_keys.cpp
    src/model/channels.cpp
    src/model/channel_posts.cpp
    src/model/stories.cpp
    src/model/story_stickers.cpp
    src/model/story_drafts.cpp
    src/model/earnings.cpp
    src/util/text.cpp
    src/util/private_files.cpp
    src/update/update.cpp
)

target_include_directories(redvault_core PUBLIC include)
target_link_libraries(redvault_core
    PUBLIC redvault_sodium nlohmann_json::nlohmann_json
    PRIVATE CURL::libcurl
)
# The Tor transport's socket guard (net/curl_transport.cpp) uses Winsock directly.
if(WIN32)
    target_link_libraries(redvault_core PUBLIC ws2_32)
endif()

if(MSVC)
    target_compile_options(redvault_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(redvault_core PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
endif()

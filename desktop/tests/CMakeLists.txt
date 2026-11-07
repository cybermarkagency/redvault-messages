find_package(Catch2 3 REQUIRED)
include(Catch)

add_executable(redvault_core_tests
    e2e_crypto_compat_test.cpp
    file_crypto_test.cpp
    recovery_key_test.cpp
    safety_number_qr_test.cpp
    net_test.cpp
    dto_parsing_test.cpp
    api_client_test.cpp
    account_repository_test.cpp
    auth_repository_test.cpp
    sign_in_test.cpp
    message_format_test.cpp
    text_format_test.cpp
    plan_choices_test.cpp
    group_call_link_test.cpp
    group_call_test.cpp
    group_invite_test.cpp
    channels_test.cpp
    channel_crypto_test.cpp
    community_crypto_test.cpp
    community_test.cpp
    community_api_test.cpp
    stories_test.cpp
    story_interactive_test.cpp
    story_interactive_api_test.cpp
    story_drafts_test.cpp
    earnings_test.cpp
    conversation_repository_test.cpp
    file_repository_test.cpp
    update_test.cpp
    privacy_test.cpp
    call_crypto_test.cpp
    vault_crypto_test.cpp
    file_lock_test.cpp
    channel_posts_test.cpp
    group_keys_test.cpp
    outbox_test.cpp
    tor_transport_test.cpp
    drive_resumable_test.cpp
    share_crypto_test.cpp
    share_repository_test.cpp
    hardening_test.cpp
    key_trust_test.cpp
    key_trust_flow_test.cpp
    vault_binding_test.cpp
    kdf_settings_test.cpp
)
target_link_libraries(redvault_core_tests PRIVATE redvault_core Catch2::Catch2WithMain)

# The web-produced file vector is shared with the Android tests rather than
# copied, so all clients stay pinned to one source of truth.
target_compile_definitions(redvault_core_tests PRIVATE
    REDVAULT_TEST_VECTORS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../android/app/src/androidTest/assets"
    # privacy_test scans the shipped sources.
    REDVAULT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/.."
    # Call crypto vectors produced by the web app's own code (see the test).
    REDVAULT_CALL_VECTORS="${CMAKE_CURRENT_SOURCE_DIR}/vectors/call_vectors.json"
    # Group call room-key vectors produced by the web app's own code (vectors/make_group_call_vectors.ts).
    REDVAULT_GROUP_CALL_VECTORS="${CMAKE_CURRENT_SOURCE_DIR}/vectors/group_call_vectors.json"
    # Group invite link vectors produced by the web app's own code (vectors/make_group_invite_vectors.ts).
    REDVAULT_GROUP_INVITE_VECTORS="${CMAKE_CURRENT_SOURCE_DIR}/vectors/group_invite_vectors.json"
    # Private channel link and forwarded-post vectors produced by the web app's own code (vectors/make_channel_vectors.ts).
    REDVAULT_CHANNEL_VECTORS="${CMAKE_CURRENT_SOURCE_DIR}/vectors/channel_vectors.json"
    # Message signature vectors produced by the web app's own code (vectors/make_message_signature_vectors.ts).
    REDVAULT_SIGNATURE_VECTORS="${CMAKE_CURRENT_SOURCE_DIR}/vectors/message_signature_vectors.json"
    # Community epoch-key vectors produced by the web app's own code (vectors/make_community_vectors.ts).
    REDVAULT_COMMUNITY_VECTORS="${CMAKE_CURRENT_SOURCE_DIR}/vectors/community_vectors.json"
    # Story vectors produced by the web app's own code (vectors/make_story_vectors.ts).
    REDVAULT_STORY_VECTORS="${CMAKE_CURRENT_SOURCE_DIR}/vectors/story_vectors.json"
    # Sticker, sealed answer and sealed totals vectors produced by the web app's own code (vectors/make_story_interactive_vectors.ts).
    REDVAULT_STORY_INTERACTIVE_VECTORS="${CMAKE_CURRENT_SOURCE_DIR}/vectors/story_interactive_vectors.json"
    # Draft vectors produced by the web app's own code (vectors/make_story_draft_vectors.ts).
    REDVAULT_STORY_DRAFT_VECTORS="${CMAKE_CURRENT_SOURCE_DIR}/vectors/story_draft_vectors.json"
    # Exclusive-post vectors produced by the web app's own code (vectors/make_post_vectors.ts).
    REDVAULT_POST_VECTORS="${CMAKE_CURRENT_SOURCE_DIR}/vectors/post_vectors.json"
    # Public share link vectors produced by the web app's own code (vectors/make_share_vectors.ts).
    REDVAULT_SHARE_VECTORS="${CMAKE_CURRENT_SOURCE_DIR}/vectors/share_vectors.json"
    # Group key notices, shared with the web and Android tests (ui/src/test/keyTrust.test.ts).
    REDVAULT_KEY_NOTICE_VECTORS="${CMAKE_CURRENT_SOURCE_DIR}/vectors/key_notice_vectors.json"
)
# privacy_test runs a loopback server to capture what libcurl really sends.
if(WIN32)
    target_link_libraries(redvault_core_tests PRIVATE ws2_32)
endif()

catch_discover_tests(redvault_core_tests)

# The call engine: two engines on this machine call each other (host
# candidates, no relay) and exchange real Opus audio and VP8 video.
if(TARGET redvault_rtc)
    # rtc_relay_test runs real ICE agents against libjuice's own TURN server on loopback.
    add_executable(redvault_rtc_tests rtc_loopback_test.cpp rtc_relay_test.cpp)
    target_link_libraries(redvault_rtc_tests PRIVATE redvault_rtc redvault_core LibJuice::LibJuiceStatic Catch2::Catch2WithMain)
    if(WIN32)
        target_link_libraries(redvault_rtc_tests PRIVATE ws2_32)
    endif()
    catch_discover_tests(redvault_rtc_tests)
endif()

# The channel page's real QML over the real post model, with stand-ins for the C++ session types. Runs on Qt's offscreen platform.
if(TARGET Qt6::Quick)
    add_executable(redvault_channel_page_tests
        channel_page_test.cpp
        ../app/src/channel_post_model.cpp
        ../app/src/channel_post_model.hpp
        ../app/src/chat_models.cpp
        ../app/src/chat_models.hpp
    )
    set_target_properties(redvault_channel_page_tests PROPERTIES AUTOMOC ON)
    target_include_directories(redvault_channel_page_tests PRIVATE ../app/src)
    target_link_libraries(redvault_channel_page_tests PRIVATE redvault_core Qt6::Quick Qt6::Qml Catch2::Catch2WithMain)
    target_compile_definitions(redvault_channel_page_tests PRIVATE
        REDVAULT_QML_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../app/qml"
        REDVAULT_CHANNEL_QML_STUBS="${CMAKE_CURRENT_SOURCE_DIR}/qml_channel_stubs")
    catch_discover_tests(redvault_channel_page_tests)

# The bits of the app with no window: the real AppLock against real QSettings, and the in-call chat panel's real QML
# against stand-ins for the app's types. Runs on Qt's offscreen platform.
    add_executable(redvault_app_tests
        app_logic_test.cpp
        chat_guard_test.cpp
        open_source_test.cpp
        vault_session_test.cpp
        ../app/src/app_lock.cpp
        ../app/src/app_lock.hpp
        ../app/src/chat_images.cpp
        ../app/src/chat_images.hpp
        ../app/src/chat_models.cpp
        ../app/src/chat_models.hpp
        ../app/src/chat_session.cpp
        ../app/src/chat_session.hpp
        ../app/src/tor_controller.cpp
        ../app/src/tor_controller.hpp
        ../app/src/vault_session.cpp
        ../app/src/vault_session.hpp
        ../app/src/windows_hello.cpp
        ../app/src/windows_hello.hpp
    )
    set_target_properties(redvault_app_tests PROPERTIES AUTOMOC ON)
    target_include_directories(redvault_app_tests PRIVATE ../app/src)
    target_link_libraries(redvault_app_tests PRIVATE redvault_core Qt6::Quick Qt6::Qml Catch2::Catch2WithMain)
    if(WIN32)
        target_link_libraries(redvault_app_tests PRIVATE runtimeobject)  # windows_hello.cpp
    endif()
    target_compile_definitions(redvault_app_tests PRIVATE
        REDVAULT_QML_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../app/qml"
        REDVAULT_QML_STUBS="${CMAKE_CURRENT_SOURCE_DIR}/qml_stubs")
    # The licence and third-party notices, built in the same way as in the app (app/CMakeLists.txt).
    qt_add_resources(redvault_app_tests "open_source_documents"
        PREFIX "/legal"
        BASE "${CMAKE_CURRENT_SOURCE_DIR}/../app/legal"
        FILES
            "${CMAKE_CURRENT_SOURCE_DIR}/../app/legal/LICENSE.txt"
            "${CMAKE_CURRENT_SOURCE_DIR}/../app/legal/THIRD_PARTY_NOTICES.txt"
    )
    catch_discover_tests(redvault_app_tests)
endif()

# Group call media: the audio mixer, and the call logic over a fake room (no LiveKit library needed).
if(TARGET redvault_groupcall)
    add_executable(redvault_groupcall_tests
        audio_mixer_test.cpp
        group_call_session_test.cpp
    )
    target_link_libraries(redvault_groupcall_tests PRIVATE redvault_groupcall redvault_core Catch2::Catch2WithMain)
    catch_discover_tests(redvault_groupcall_tests)
endif()

# Device keys (protocol 3): key storage, the device logic and pairing against the real engine and a fake server.
if(TARGET redvault_e2ev3)
    add_executable(redvault_e2ev3_tests e2ev3_test.cpp e2ev3_content_test.cpp)
    target_link_libraries(redvault_e2ev3_tests PRIVATE redvault_e2ev3 redvault_core Catch2::Catch2WithMain)
    # Reactions, edits, deletes and files: the cases every client must pass (the web loads the same file).
    target_compile_definitions(redvault_e2ev3_tests PRIVATE
        REDVAULT_V3_CONTENT_VECTORS="${CMAKE_CURRENT_SOURCE_DIR}/vectors/v3_chat_content_vectors.json")
    catch_discover_tests(redvault_e2ev3_tests)
endif()

# The configure-time check on REDVAULT_SOURCE_CODE_URL / REDVAULT_SOURCE_CONTACT (cmake/source_info.cmake):
# empty and https:// are accepted, http:// and anything else stop the configure step.
set(_redvault_source_check "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/source_info.cmake")
add_test(NAME source_settings_unset COMMAND ${CMAKE_COMMAND} -P "${_redvault_source_check}")
add_test(NAME source_settings_https COMMAND ${CMAKE_COMMAND}
    "-DREDVAULT_SOURCE_CODE_URL=https://git.example.org/redvault/-/tree/v{version}"
    "-DREDVAULT_SOURCE_CONTACT=source@example.org" -P "${_redvault_source_check}")
add_test(NAME source_settings_rejects_http COMMAND ${CMAKE_COMMAND}
    "-DREDVAULT_SOURCE_CODE_URL=http://git.example.org/redvault" -P "${_redvault_source_check}")
add_test(NAME source_settings_rejects_other_schemes COMMAND ${CMAKE_COMMAND}
    "-DREDVAULT_SOURCE_CODE_URL=javascript:alert(1)" -P "${_redvault_source_check}")
add_test(NAME source_settings_rejects_bad_contact COMMAND ${CMAKE_COMMAND}
    "-DREDVAULT_SOURCE_CONTACT=http://example.org/source" -P "${_redvault_source_check}")
set_tests_properties(source_settings_rejects_http source_settings_rejects_other_schemes
    source_settings_rejects_bad_contact PROPERTIES PASS_REGULAR_EXPRESSION "must be an (https|email)"
    FAIL_REGULAR_EXPRESSION "Source settings accepted")
set_tests_properties(source_settings_unset source_settings_https PROPERTIES PASS_REGULAR_EXPRESSION "Source settings accepted")

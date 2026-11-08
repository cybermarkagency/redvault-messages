# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

# The Qt 6 / QML desktop app on top of redvault_core. All crypto, HTTP and
# data logic stays in the core; this layer is views, models and threading.
qt_standard_project_setup(REQUIRES 6.5)
qt_policy(SET QTP0004 NEW)

# Must be set before qt_add_qml_module reads the file list.
set_source_files_properties(qml/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_executable(redvault_desktop WIN32
    src/main.cpp
)

# The version everywhere (window, --version, updates, the .exe's details)
# comes from project(... VERSION ...) in the top-level CMakeLists.txt.
target_compile_definitions(redvault_desktop PRIVATE REDVAULT_VERSION="${PROJECT_VERSION}")
# Settings > About, "Open-Source Licenses and Source Code": see REDVAULT_SOURCE_CODE_URL and
# REDVAULT_SOURCE_CONTACT in the top-level CMakeLists.txt (empty = not published / Terms of Service contact).
target_compile_definitions(redvault_desktop PRIVATE
    REDVAULT_SOURCE_CODE_URL="${REDVAULT_SOURCE_CODE_URL}"
    REDVAULT_SOURCE_CONTACT="${REDVAULT_SOURCE_CONTACT}")

if(WIN32)
    # The .exe's own icon and version info (Explorer, taskbar, installer).
    set(REDVAULT_ICON "${CMAKE_CURRENT_SOURCE_DIR}/icons/redvault.ico")
    configure_file(redvault.rc.in "${CMAKE_CURRENT_BINARY_DIR}/redvault.rc" @ONLY)
    target_sources(redvault_desktop PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/redvault.rc")
    # The resource compiler doesn't report the .ico as an input, so a new logo wouldn't rebuild it otherwise.
    set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/redvault.rc" PROPERTIES OBJECT_DEPENDS "${REDVAULT_ICON}")
endif()

# One picture per size, drawn for that size by brand/make_icons.py (the window, tray and Linux menus pick the nearest).
set(REDVAULT_APP_ICONS "")
foreach(size 16 20 22 24 28 30 32 36 40 42 48 54 56 60 64 72 80 84 96 128 256)
    list(APPEND REDVAULT_APP_ICONS icons/app/redvault-${size}.png)
endforeach()

qt_add_qml_module(redvault_desktop
    URI Redvault
    VERSION 1.0
    SOURCES
        src/async.hpp
        src/app_controller.hpp src/app_controller.cpp
        src/chat_session.hpp src/chat_session.cpp
        src/chat_models.hpp src/chat_models.cpp
        src/people_session.hpp src/people_session.cpp
        src/account_session.hpp src/account_session.cpp
        src/avatar_jpeg.hpp src/avatar_jpeg.cpp
        src/local_store.hpp
        src/file_safety.hpp
        src/app_lock.hpp src/app_lock.cpp
        src/desktop_shell.hpp src/desktop_shell.cpp
        src/call_notice.hpp
        src/image_providers.hpp src/image_providers.cpp
        src/updater.hpp src/updater.cpp
        src/update_key.hpp
        src/prefs.hpp
        src/live_session.hpp src/live_session.cpp
        src/call_controller.hpp src/call_controller.cpp
        src/group_call_controller.hpp src/group_call_controller.cpp
        src/media_io.hpp src/media_io.cpp
        src/video_convert.hpp src/video_convert.cpp
        src/security_settings.hpp src/security_settings.cpp
        src/files_session.hpp src/files_session.cpp
        src/shares_session.hpp src/shares_session.cpp
        src/files_locks.cpp
        src/vault_session.hpp src/vault_session.cpp
        src/inbox_session.hpp src/inbox_session.cpp
        src/device_keys_session.hpp src/device_keys_session.cpp
        src/group_invite_session.hpp src/group_invite_session.cpp
        src/channels_session.hpp src/channels_session.cpp
        src/channel_post_model.hpp src/channel_post_model.cpp
        src/channel_uploads.hpp src/channel_uploads.cpp
        src/channel_media.hpp src/channel_media.cpp
        src/community_session.hpp src/community_session.cpp
        src/community_admin_session.hpp src/community_admin_session.cpp
        src/stories_session.hpp src/stories_session.cpp
        src/earnings_session.hpp src/earnings_session.cpp
        src/chat_images.hpp src/chat_images.cpp
        src/windows_hello.hpp src/windows_hello.cpp
        src/tor_controller.hpp src/tor_controller.cpp
        src/source_info.hpp
    RESOURCES
        ${REDVAULT_APP_ICONS}
        icons/app/redvault.svg
        icons/lucide/check-check.svg
        icons/lucide/check.svg
        icons/lucide/copy.svg
        icons/lucide/download.svg
        icons/lucide/external-link.svg
        icons/lucide/eye-off.svg
        icons/lucide/eye.svg
        icons/lucide/file.svg
        icons/lucide/image.svg
        icons/lucide/inbox.svg
        icons/lucide/pin.svg
        icons/lucide/ticket.svg
        icons/lucide/chart-bar.svg
        icons/lucide/info.svg
        icons/lucide/lock.svg
        icons/lucide/log-out.svg
        icons/lucide/message-square.svg
        icons/lucide/paperclip.svg
        icons/lucide/qr-code.svg
        icons/lucide/reply.svg
        icons/lucide/search.svg
        icons/lucide/send-horizontal.svg
        icons/lucide/settings.svg
        icons/lucide/shield-check.svg
        icons/lucide/smile-plus.svg
        icons/lucide/trash-2.svg
        icons/lucide/triangle-alert.svg
        icons/lucide/user-plus.svg
        icons/lucide/users.svg
        icons/lucide/x.svg
        icons/ui/arrow-down-left.svg
        icons/ui/arrow-up-right.svg
        icons/ui/ban.svg
        icons/ui/bell-off.svg
        icons/ui/bell.svg
        icons/ui/chat.svg
        icons/ui/check.svg
        icons/ui/chevron-down.svg
        icons/ui/chevron-left.svg
        icons/ui/chevron-right.svg
        icons/ui/clock.svg
        icons/ui/close.svg
        icons/ui/copy.svg
        icons/ui/dots.svg
        icons/ui/download.svg
        icons/ui/external.svg
        icons/ui/file.svg
        icons/ui/flag.svg
        icons/ui/globe.svg
        icons/ui/headphones.svg
        icons/ui/image.svg
        icons/ui/info.svg
        icons/ui/key.svg
        icons/ui/link.svg
        icons/ui/lock.svg
        icons/ui/log-out.svg
        icons/ui/log.svg
        icons/ui/maximize.svg
        icons/ui/mic-off.svg
        icons/ui/mic.svg
        icons/ui/monitor-up.svg
        icons/ui/monitor.svg
        icons/ui/panel-right.svg
        icons/ui/paperclip.svg
        icons/ui/pause.svg
        icons/ui/pen.svg
        icons/ui/phone-off.svg
        icons/ui/phone.svg
        icons/ui/play.svg
        icons/ui/plus.svg
        icons/ui/qr.svg
        icons/ui/refresh.svg
        icons/ui/search.svg
        icons/ui/send.svg
        icons/ui/share.svg
        icons/ui/shield-check.svg
        icons/ui/shield.svg
        icons/ui/signal.svg
        icons/ui/sliders.svg
        icons/ui/smile.svg
        icons/ui/speaker.svg
        icons/ui/star.svg
        icons/ui/heart.svg
        icons/ui/stories.svg
        icons/ui/speaker-off.svg
        icons/ui/timer.svg
        icons/ui/trash.svg
        icons/ui/upload.svg
        icons/ui/user-plus.svg
        icons/ui/user.svg
        icons/ui/users.svg
        icons/ui/video-off.svg
        icons/ui/eye-off.svg
        icons/ui/eye.svg
        icons/ui/user-check.svg
        icons/ui/lock-open.svg
        icons/ui/note.svg
        icons/ui/wifi.svg
        icons/ui/credit-card.svg
        icons/ui/fingerprint.svg
        icons/ui/alert-circle.svg
        icons/ui/repeat.svg
        icons/ui/cloud.svg
        icons/ui/folder-plus.svg
        icons/ui/folder.svg
        icons/ui/video.svg
        fonts/bricolage-grotesque-latin-700-normal.ttf
        fonts/ibm-plex-mono-latin-400-normal.ttf
        fonts/ibm-plex-mono-latin-500-normal.ttf
        fonts/ibm-plex-sans-latin-400-normal.ttf
        fonts/ibm-plex-sans-latin-500-normal.ttf
        fonts/ibm-plex-sans-latin-600-normal.ttf
    QML_FILES
        qml/Main.qml
        qml/Theme.qml
        qml/RvButton.qml
        qml/RvTextField.qml
        qml/Avatar.qml
        qml/LoginView.qml
        qml/ChatsView.qml
        qml/ThreadView.qml
        qml/MessageDelegate.qml
        qml/RvDialog.qml
        qml/ConfirmDialog.qml
        qml/MessageRequestsDialog.qml
        qml/InboxView.qml
        qml/ChannelsView.qml
        qml/ChannelPage.qml
        qml/ChannelAvatar.qml
        qml/ChannelPostContent.qml
        qml/ChannelInfoPanel.qml
        qml/ChannelOwnerExtras.qml
        qml/ChannelPostBubble.qml
        qml/ChannelReaderBar.qml
        qml/ChannelComposer.qml
        qml/ChannelSuggestions.qml
        qml/ChannelUploadList.qml
        qml/ChannelScheduleField.qml
        qml/ChannelPhotoViewer.qml
        qml/CommunitiesView.qml
        qml/CommunityPage.qml
        qml/CommunityCreate.qml
        qml/CommunityManage.qml
        qml/CreatorDashboard.qml
        qml/CommunityReportDialog.qml
        qml/AdminView.qml
        qml/RvComboBox.qml
        qml/Pill.qml
        qml/PostField.qml
        qml/GroupInviteSection.qml
        qml/JoinGroupDialog.qml
        qml/ForwardDialog.qml
        qml/ImageViewer.qml
        qml/SendFilesDialog.qml
        qml/PollDialog.qml
        qml/EmojiPicker.qml
        qml/EmojiData.js
        qml/RvSwitch.qml
        qml/Banner.qml
        qml/RecoveryKeyPanel.qml
        qml/RecoveryKeyView.qml
        qml/FriendsDialog.qml
        qml/FriendPicker.qml
        qml/NewGroupDialog.qml
        qml/GroupInfoDialog.qml
        qml/SafetyNumberDialog.qml
        qml/LockView.qml
        qml/Icon.qml
        qml/UpdateBanner.qml
        qml/AppShell.qml
        qml/NavRail.qml
        qml/IconButton.qml
        qml/SearchField.qml
        qml/Chip.qml
        qml/Card.qml
        qml/ChatDetails.qml
        qml/CallOverlay.qml
        qml/CallBar.qml
        qml/GroupCallBar.qml
        qml/GroupCallOverlay.qml
        qml/GroupCallTile.qml
        qml/DevicePicker.qml
        qml/MiniChat.qml
        qml/CallsView.qml
        qml/ContactsView.qml
        qml/SettingsView.qml
        qml/LinkedDevicesSection.qml
        qml/RelayServersSection.qml
        qml/NetworkRoutingSection.qml
        qml/TorStatusChip.qml
        qml/TransparencyLogView.qml
        qml/FilesView.qml
        qml/PasswordsView.qml
        qml/VaultItemDialog.qml
        qml/ContactPickerDialog.qml
        qml/LockDialog.qml
        qml/InfoCard.qml
        qml/LockPasswordField.qml
        qml/KeyPicker.qml
        qml/KeyCreateDialog.qml
        qml/KeyImportDialog.qml
        qml/UploadLockDialog.qml
        qml/UnlockFileDialog.qml
        qml/FileKeysView.qml
        qml/StrengthBar.qml
        qml/CountdownRing.qml
        qml/ConsentBox.qml
        qml/LegalDocumentDialog.qml
        qml/PlanCard.qml
        qml/StoryRing.qml
        qml/StoryTray.qml
        qml/StoryFrame.qml
        qml/StoryViewer.qml
        qml/StoryComposer.qml
        qml/StorySticker.qml
        qml/StickerEditor.qml
        qml/StorySheet.qml
        qml/StoryAnswersSheet.qml
        qml/StoryReportSheet.qml
        qml/StoryCombo.qml
        qml/StorySettingsSection.qml
        qml/DeviceKeysSection.qml
        qml/PaidMessageBar.qml
        qml/TipDialog.qml
        qml/ExclusivePostCard.qml
        qml/CreatorOfferSection.qml
        qml/EarningsView.qml
        qml/HighlightsRow.qml
        qml/RvChoice.qml
        qml/ShareLinkDialog.qml
        qml/ShareLinkEditDialog.qml
        qml/ShareLinksView.qml
        qml/IncomingSharesView.qml
        qml/OpenSourceSection.qml
        qml/PlainTextDocumentDialog.qml
)

# The Terms of Service and Privacy Policy shown at sign-up: legal/ at the
# repository root (the web and Android apps read the same files), built in
# as :/legal/terms-of-service.md and :/legal/privacy-policy.md.
set(REDVAULT_LEGAL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../legal")
qt_add_resources(redvault_desktop "legal_documents"
    PREFIX "/legal"
    BASE "${REDVAULT_LEGAL_DIR}"
    FILES
        "${REDVAULT_LEGAL_DIR}/terms-of-service.md"
        "${REDVAULT_LEGAL_DIR}/privacy-policy.md"
)

# This app's licence (AGPL-3.0-only) and the third-party notices (generated by
# tools/compliance/third_party_notices.py), built in as :/legal/LICENSE.txt and
# :/legal/THIRD_PARTY_NOTICES.txt for Settings > About, and installed in licenses/ below.
set(REDVAULT_OPEN_SOURCE_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/legal/LICENSE.txt"
    "${CMAKE_CURRENT_SOURCE_DIR}/legal/THIRD_PARTY_NOTICES.txt")
qt_add_resources(redvault_desktop "open_source_documents"
    PREFIX "/legal"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/legal"
    FILES ${REDVAULT_OPEN_SOURCE_FILES}
)

# The generated QML type registration includes the headers by bare name.
target_include_directories(redvault_desktop PRIVATE src)

# QR codes for two-factor setup and safety-number verification.
find_package(PkgConfig REQUIRED)
pkg_check_modules(QRENCODE REQUIRED IMPORTED_TARGET libqrencode)

# Echo cancellation, noise suppression and gain control for calls (WebRTC's
# audio processing module). Optional: without it, calls still work, but
# speakers without headphones can echo.
pkg_check_modules(WEBRTC_APM IMPORTED_TARGET webrtc-audio-processing-1)
if(WEBRTC_APM_FOUND)
    target_link_libraries(redvault_desktop PRIVATE PkgConfig::WEBRTC_APM)
    target_compile_definitions(redvault_desktop PRIVATE REDVAULT_HAS_APM)
    message(STATUS "Calls: echo cancellation on (webrtc-audio-processing ${WEBRTC_APM_VERSION})")
else()
    message(STATUS "Calls: webrtc-audio-processing-1 not found, building without echo cancellation")
endif()

if(WIN32)
    # Windows Hello for unlocking the password vault (WinRT).
    target_link_libraries(redvault_desktop PRIVATE runtimeobject)
endif()

target_link_libraries(redvault_desktop PRIVATE
    redvault_core
    redvault_rtc
    redvault_groupcall
    PkgConfig::QRENCODE
    Qt6::Multimedia
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Widgets
    Qt6::Svg
)

set_target_properties(redvault_desktop PROPERTIES OUTPUT_NAME "redvaultmessages")

# LiveKit's media library for group calls, loaded at run time from next to the program.
redvault_copy_livekit_ffi(redvault_desktop)

# Per-device encryption keys (protocol 3 preview), when built in: see ../e2ev3/CMakeLists.txt.
if(REDVAULT_E2E_V3)
    target_link_libraries(redvault_desktop PRIVATE redvault_e2ev3)
    target_compile_definitions(redvault_desktop PRIVATE REDVAULT_HAS_E2E_V3=1)
endif()

# The Tor gateway (tor-core), when built in: see ../tor/CMakeLists.txt.
if(REDVAULT_TOR)
    target_link_libraries(redvault_desktop PRIVATE redvault_tor)
    target_compile_definitions(redvault_desktop PRIVATE REDVAULT_HAS_TOR=1)
    redvault_copy_tor(redvault_desktop)
endif()
if(REDVAULT_TOR_ONLY)
    target_compile_definitions(redvault_desktop PRIVATE REDVAULT_TOR_ONLY=1 REDVAULT_ONION_API_BASE="${REDVAULT_ONION_API_BASE}")
endif()

if(MSVC)
    target_compile_options(redvault_desktop PRIVATE /W4 /permissive-)
else()
    target_compile_options(redvault_desktop PRIVATE -Wall -Wextra -Wpedantic)
endif()

# --- Installing (used by packaging/package.ps1 and packaging/package-linux.sh) ---
# On Windows, `cmake --install` lays out a self-contained folder: Redvault.exe, the Qt
# libraries, plugins and QML modules (via windeployqt), and every other DLL
# the exe needs from the toolchain (libsodium, libcurl and its TLS stack, the
# MinGW runtime), so it runs with a double-click and no PATH changes.
# On Linux it installs the binary, .desktop entry and icon; linuxdeploy then
# bundles Qt and the libraries into an AppImage.
install(TARGETS redvault_desktop RUNTIME DESTINATION bin)

if(WIN32)
    qt_generate_deploy_qml_app_script(
        TARGET redvault_desktop
        OUTPUT_SCRIPT redvault_deploy_script
        NO_TRANSLATIONS
        DEPLOY_TOOL_OPTIONS --no-compiler-runtime --no-system-d3d-compiler --no-opengl-sw
    )
    install(SCRIPT ${redvault_deploy_script})
elseif(UNIX AND NOT APPLE)
    install(FILES ../packaging/linux/redvaultmessages.desktop DESTINATION share/applications)
    foreach(size 16 22 24 32 48 64 96 128 256 512)
        install(FILES icons/app/redvault-${size}.png DESTINATION share/icons/hicolor/${size}x${size}/apps RENAME redvaultmessages.png)
    endforeach()
    install(FILES icons/app/redvault.svg DESTINATION share/icons/hicolor/scalable/apps RENAME redvaultmessages.svg)
endif()

# Group calls: LiveKit's prebuilt client library (Apache-2.0), opened at run time. Windows keeps it next to the
# program; on Linux it goes in lib/, which the AppImage carries and the program looks in (../lib).
if(WIN32)
    install(FILES "${REDVAULT_LIVEKIT_FFI_LIBRARY}" DESTINATION bin)
else()
    install(FILES "${REDVAULT_LIVEKIT_FFI_LIBRARY}" DESTINATION lib)
endif()
install(FILES "${REDVAULT_LIVEKIT_FFI_LICENSE}" DESTINATION licenses RENAME livekit-ffi-LICENSE.md)

# The Tor gateway: next to the program on Windows, in lib/ on Linux (the program's install rpath is $ORIGIN/../lib).
if(REDVAULT_TOR)
    if(WIN32)
        install(FILES "${REDVAULT_TOR_LIBRARY}" DESTINATION bin)
    else()
        install(FILES "${REDVAULT_TOR_LIBRARY}" DESTINATION lib)
    endif()
    install(FILES "${REDVAULT_TOR_CORE_DIR}/patches/saturating-time/PATCHED.md" DESTINATION licenses RENAME tor-core-patches.md)
endif()

# The app's own licence and every third-party notice, next to the program: dist/redvaultmessages/licenses
# (the Windows installer) and usr/licenses in the AppImage.
install(FILES ${REDVAULT_OPEN_SOURCE_FILES} DESTINATION licenses)
# Lucide's ISC license asks that its notice ship with the icons.
install(FILES icons/lucide/LICENSE DESTINATION licenses RENAME lucide-LICENSE.txt)
# The fonts are under the SIL Open Font License, which asks the same.
install(FILES fonts/IBMPlex-OFL.txt fonts/BricolageGrotesque-OFL.txt DESTINATION licenses)

if(WIN32)
    get_filename_component(REDVAULT_TOOLCHAIN_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
    install(CODE "
        if(POLICY CMP0207)
            cmake_policy(SET CMP0207 NEW)
        endif()
        # Qt's plugins and QML modules (deployed just before) have dependencies of their own, e.g. qjpeg needs libjpeg.
        file(GLOB_RECURSE redvault_modules \"\${CMAKE_INSTALL_PREFIX}/share/*.dll\")
        file(GET_RUNTIME_DEPENDENCIES
            EXECUTABLES \"$<TARGET_FILE:redvault_desktop>\"
            MODULES \${redvault_modules}
            RESOLVED_DEPENDENCIES_VAR redvault_deps
            UNRESOLVED_DEPENDENCIES_VAR redvault_unresolved
            DIRECTORIES \"${REDVAULT_TOOLCHAIN_BIN}\"
            PRE_EXCLUDE_REGEXES \"^api-ms-\" \"^ext-ms-\"
            POST_EXCLUDE_REGEXES \"[Ww][Ii][Nn][Dd][Oo][Ww][Ss][/\\\\][Ss][Yy][Ss][Tt][Ee][Mm]32\"
        )
        foreach(dep IN LISTS redvault_deps)
            file(INSTALL \"\${dep}\" DESTINATION \"\${CMAKE_INSTALL_PREFIX}/bin\")
        endforeach()
    ")
endif()

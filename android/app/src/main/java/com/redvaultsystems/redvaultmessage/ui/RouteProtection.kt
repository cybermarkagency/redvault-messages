// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.ui

/**
 * Screens that show or take a secret: they stay out of screenshots and Recents whatever "Hide app content"
 * says. Passwords, file keys and their passwords, the two-factor key, recovery keys and device keys.
 * Written out in full, with no else, so a new screen has to be decided on here before the app compiles.
 */
internal fun Route.holdsSecrets(): Boolean = when (this) {
    is Route.Home -> tab == Tabs.PASSWORDS
    is Route.PasswordItem, is Route.PasswordEdit, is Route.PasswordShare,
    Route.FileKeys, is Route.FileKey, Route.KeyCreate, Route.KeyImport, is Route.KeyPassword,
    Route.UploadLock, is Route.ChangeLock, is Route.UnlockFile,
    Route.Mfa, Route.RecoveryKeySettings, Route.DeviceKeys,
    // Share links carry their decryption key after the #, and a link's password is typed here.
    is Route.ShareLink, Route.SharedLinks, is Route.ShareLinkEdit,
    -> true

    is Route.Thread, Route.AddFriend, Route.FriendRequests, Route.MessageRequests, Route.Inbox, Route.Settings,
    Route.ChangeUsername, Route.ChangePassword, is Route.Verify, is Route.MessageDetails, Route.Transparency,
    Route.AppLockSettings, Route.PrivacyMode, Route.Blocked, Route.DeleteAccount, Route.NewGroup,
    is Route.GroupInfo, is Route.JoinGroup, is Route.Channel,
    is Route.StoryViewer, Route.StoryComposer, Route.StorySettings,
    is Route.FileDetail, is Route.ShareFile,
    is Route.Contact, Route.ContactCode, is Route.AddFriendAs, is Route.SharedMedia, Route.ShareTo,
    Route.LinkedDevices, Route.RelayServers, Route.TransparencyLog, Route.NetworkRouting,
    is Route.Communities, is Route.Community, Route.Incoming,
    -> false
}

/**
 * Screens where one tap does something that can't be taken back or weakens the account: they ignore touches
 * that come through another app's window. Not every screen, because screen dimmers and similar overlays
 * would then silently stop the whole app from responding.
 */
internal fun Route.guardsTouches(): Boolean = holdsSecrets() || when (this) {
    Route.DeleteAccount, Route.ChangePassword, Route.AppLockSettings, Route.LinkedDevices, is Route.Verify -> true
    else -> false
}

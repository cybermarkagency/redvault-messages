package com.redvaultsystems.redvaultmessage.ui

import org.junit.Assert.assertEquals
import org.junit.Test

class RouteTest {
    @Test
    fun everyRouteSurvivesEncodeAndDecode() {
        // Touches the companion first, the order that crashed on sign-in.
        assertEquals(Route.Home(Tabs.CHATS), Route.decode("home:chats"))
        val all = listOf(
            Route.Home(Tabs.CHATS), Route.Home(Tabs.CALLS), Route.Home(Tabs.CONTACTS), Route.Home(Tabs.CHANNELS), Route.Home(Tabs.FILES),
            Route.Home(Tabs.PASSWORDS),
            Route.Thread("c1"), Route.AddFriend, Route.FriendRequests,
            Route.Settings, Route.ChangeUsername, Route.ChangePassword, Route.Mfa,
            Route.Verify("c1"), Route.MessageDetails("c1", "m1"), Route.Transparency, Route.AppLockSettings,
            Route.PrivacyMode, Route.Blocked, Route.RecoveryKeySettings, Route.DeleteAccount, Route.NewGroup,
            Route.GroupInfo("g1"),
            // Invite links and channels
            Route.JoinGroup("Ab_-9", "AwoRGB8mLTQ7QklQV15lbHN6gYiPlp2kq7K5wMfO1dw"), Route.JoinGroup("L1", ""),
            Route.Channel("11111111-2222-4333-8444-555555555555"),
            Route.Channel("11111111-2222-4333-8444-555555555555", "-_v7-_v7-_v7-_v7-_v7-_v7-_v7-_v7-_v7-_v7-_s"),
            // Stories
            Route.StoryViewer("me"), Route.StoryViewer("5c1d0f7e-2b3a-4c8d-9e6f-a1b2c3d4e5f6"), Route.StoryComposer,
            // Files
            Route.FileDetail("f1"), Route.ShareFile("f1"), Route.UploadLock, Route.ChangeLock("f1", "none"), Route.ChangeLock("f1", ""),
            Route.UnlockFile("f1", "changeLock"), Route.FileKeys, Route.FileKey("k1"), Route.KeyCreate, Route.KeyImport,
            Route.KeyPassword("k1", "set"),
            // Passwords
            Route.PasswordItem("p1"), Route.PasswordEdit("p1", ""), Route.PasswordEdit("", "card"), Route.PasswordShare("p1"),
            // Contacts
            Route.Contact("u1"), Route.ContactCode, Route.AddFriendAs("alice.b"),
            // Settings › security
            Route.LinkedDevices, Route.RelayServers, Route.TransparencyLog, Route.SharedMedia("c1"),
            // Share to RedVault
            Route.ShareTo,
            // Communities
            Route.Communities(), Route.Communities("memberships"), Route.Community("3f2a9c1e-7b4d-4e8a-9c2f-1a2b3c4d5e6f"),
        )
        for (route in all) assertEquals(route, Route.decode(route.encode()))
    }

    @Test
    fun unknownValueFallsBackToChats() {
        assertEquals(Route.Home(), Route.decode("something-else"))
    }
}

package com.redvaultsystems.redvaultmessage.ui

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class RouteProtectionTest {
    private val secret = listOf(
        // The password vault
        Route.Home(Tabs.PASSWORDS), Route.PasswordItem("p1"), Route.PasswordEdit("p1", ""), Route.PasswordEdit("", "card"), Route.PasswordShare("p1"),
        // File keys, and the sheets that take a file's password
        Route.FileKeys, Route.FileKey("k1"), Route.KeyCreate, Route.KeyImport, Route.KeyPassword("k1", "set"),
        Route.UploadLock, Route.ChangeLock("f1", ""), Route.UnlockFile("f1", "changeLock"),
        // The TOTP secret and recovery keys (account and device keys)
        Route.Mfa, Route.RecoveryKeySettings, Route.DeviceKeys,
        // Share links (their key is in the link) and the sheet that sets a link's password
        Route.ShareLink("f1"), Route.SharedLinks, Route.ShareLinkEdit("l1", "password"),
    )

    @Test
    fun everyScreenWithASecretIsCoveredWhateverTheSetting() {
        for (route in secret) assertTrue("$route holds a secret", route.holdsSecrets())
    }

    @Test
    fun theDefaultForChatsIsNotChanged() {
        for (route in listOf(Route.Home(Tabs.CHATS), Route.Home(Tabs.FILES), Route.Thread("c1"), Route.Settings, Route.Channel("c1"))) {
            assertFalse("$route follows the Hide app content setting", route.holdsSecrets())
            assertFalse("$route takes every touch", route.guardsTouches())
        }
    }

    @Test
    fun confirmationsThatCantBeUndoneIgnoreObscuredTouches() {
        for (route in listOf(Route.DeleteAccount, Route.ChangePassword, Route.AppLockSettings, Route.LinkedDevices, Route.Verify("c1"))) {
            assertTrue("$route is guarded", route.guardsTouches())
        }
        for (route in secret) assertTrue("$route is guarded", route.guardsTouches())
    }
}

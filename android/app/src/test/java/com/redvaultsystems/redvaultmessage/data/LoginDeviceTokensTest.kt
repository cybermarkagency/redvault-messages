// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.data

import com.redvaultsystems.redvaultmessage.data.api.SessionCookieJar
import com.redvaultsystems.redvaultmessage.devicekeys.SoftwareKeyWrapper
import okhttp3.Cookie
import okhttp3.HttpUrl.Companion.toHttpUrl
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.nio.file.Files
import java.util.Base64

/** RV-1115-01: the trusted-device token is kept wrapped, per handle, and survives a new store (an app restart). */
class LoginDeviceTokensTest {
    private val dir = Files.createTempDirectory("rv-login-devices").toFile()
    private val wrapper = SoftwareKeyWrapper()
    private fun store() = WrappedLoginDeviceTokens(dir, wrapper, { Base64.getEncoder().encodeToString(it) }, { Base64.getDecoder().decode(it) })

    @Test
    fun keepsATokenPerHandleAcrossRestarts() {
        store().save("Alice", "token-a")
        store().save("bob", "token-b")
        assertEquals("token-a", store().load("alice"))
        assertEquals("token-b", store().load("BOB"))
        assertNull(store().load("carol"))
        store().save("alice", "token-a2")
        assertEquals("token-a2", store().load("alice"))
    }

    @Test
    fun writesNoHandleOrTokenInTheClear() {
        store().save("alice", "secret-token-value")
        val files = dir.listFiles()!!.filter { it.isFile }
        assertEquals(1, files.size)
        assertFalse(files[0].name.contains("alice"))
        assertFalse(files[0].readText().contains("secret-token-value"))
    }

    @Test
    fun aLostKeystoreKeyOnlyMeansNoToken() {
        store().save("alice", "token-a")
        wrapper.delete(WrappedLoginDeviceTokens.ALIAS)
        assertNull(store().load("alice"))
    }

    @Test
    fun cookieJarFindsAndRestoresTheDeviceCookie() {
        val jar = SessionCookieJar()
        val url = "https://example.com/api/auth/login".toHttpUrl()
        jar.saveFromResponse(url, listOfNotNull(Cookie.parse(url, "rv_device=abc; Path=/; Expires=Fri, 13 Nov 2099 00:00:00 GMT")))
        assertEquals("abc", jar.find("rv_device")?.value)
        jar.clear()
        assertNull(jar.find("rv_device"))
        jar.restore(Cookie.Builder().name("rv_device").value("xyz").hostOnlyDomain("example.com").path("/").build())
        assertTrue(jar.loadForRequest(url).any { it.name == "rv_device" && it.value == "xyz" })
    }
}

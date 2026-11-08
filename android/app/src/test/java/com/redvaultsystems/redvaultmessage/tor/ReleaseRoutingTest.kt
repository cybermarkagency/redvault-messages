// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.tor

import com.redvaultsystems.redvaultmessage.BuildConfig
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File
import javax.xml.parsers.DocumentBuilderFactory

/**
 * Tor by default, one app (docs/tor-by-default.md): a release build only ever uses the onion API base. Checked at
 * start-up (ReleaseRouting) and by the build itself (checkTorOnlyRelease in app/build.gradle.kts, read here as
 * source), with the one app id and the cleartext rule that allows only the onion host. Runs with android/app as the
 * working directory, like PrivacyManifestTest.
 */
class ReleaseRoutingTest {
    private val build = File("build.gradle.kts").readText()

    @Test
    fun aReleaseBuildRefusesAnyServerButTheOnionService() {
        for (base in listOf(
            "https://www.redvaultsystems.com/redvaultmessages/api/",
            "http://www.redvaultsystems.com/redvaultmessages/api/",
            "http://10.0.2.2:3000/api/",
            "http://localhost:3000/api/",
            "https://${ReleaseRouting.ONION_HOST}/redvaultmessages/api/",
            "http://onion/api/",
            "http://short.onion/api/",
            "http://${ReleaseRouting.ONION_HOST}.example.com/api/",
            "not a url",
        )) {
            assertNotNull("a release build must refuse $base", ReleaseRouting.refusal(base, debug = false))
        }
        assertNull(ReleaseRouting.refusal(ReleaseRouting.ONION_API_BASE, debug = false))
    }

    @Test
    fun onlyADebugBuildMayUseALocalTestServer() {
        assertNull(ReleaseRouting.refusal("http://10.0.2.2:3000/api/", debug = true))
        assertNotNull(ReleaseRouting.refusal("http://10.0.2.2:3000/api/", debug = false))
    }

    @Test
    fun theDefaultIsTheLiveOnionService() {
        assertTrue(ReleaseRouting.isV3Onion(ReleaseRouting.ONION_HOST))
        assertTrue(build.contains("val onionApiBase = \"${ReleaseRouting.ONION_API_BASE}\""))
        val props = File("../gradle.properties").readLines().single { it.startsWith("redvaultApiBase=") }
        assertEquals("redvaultApiBase=${ReleaseRouting.ONION_API_BASE}", props)
        // These unit tests are built with the defaults: Tor on, the onion base.
        assertEquals(ReleaseRouting.ONION_API_BASE, BuildConfig.API_BASE)
        assertTrue(BuildConfig.TOR_ONLY)
        assertEquals("https://www.redvaultsystems.com/redvaultmessages/", BuildConfig.WEB_APP_BASE)
    }

    @Test
    fun theReleaseBuildChecksItsApiBaseBeforeCompiling() {
        assertTrue(build.contains("tasks.matching { it.name == \"preReleaseBuild\" }.configureEach { dependsOn(checkTorOnlyRelease) }"))
        val check = build.substringAfter("val checkTorOnlyRelease = tasks.register(\"checkTorOnlyRelease\")").substringBefore("\n}\n")
        assertTrue(check.contains("require(onion && base.startsWith(\"http://\"))"))
        assertTrue(check.contains("network_security_config.xml must allow cleartext to"))
        // Tor's gateway is built into every build, not only some.
        assertFalse(build.contains("if (torRouting)"))
        assertTrue(build.contains("jniLibs.directories.add(layout.buildDirectory.dir(\"torJniLibs\")"))
    }

    @Test
    fun oneAppNoTorEdition() {
        assertFalse(build.contains("applicationIdSuffix"))
        assertFalse(build.contains("versionNameSuffix"))
        assertFalse(build.contains("redvaultTorOnly"))
        assertTrue(build.contains("applicationId = \"com.redvaultsystems.redvaultmessage\""))
        assertEquals("com.redvaultsystems.redvaultmessage", BuildConfig.APPLICATION_ID)
    }

    @Test
    fun cleartextIsAllowedOnlyToTheOnionHost() {
        val doc = DocumentBuilderFactory.newInstance().newDocumentBuilder().parse(File("src/main/res/xml/network_security_config.xml"))
        val base = doc.getElementsByTagName("base-config").item(0) as org.w3c.dom.Element
        assertEquals("false", base.getAttribute("cleartextTrafficPermitted"))
        val domains = doc.getElementsByTagName("domain-config")
        val cleartext = (0 until domains.length).map { domains.item(it) as org.w3c.dom.Element }
            .filter { it.getAttribute("cleartextTrafficPermitted") == "true" }
            .flatMap { config ->
                val names = config.getElementsByTagName("domain")
                (0 until names.length).map { names.item(it) as org.w3c.dom.Element }
            }
        assertEquals(listOf(ReleaseRouting.ONION_HOST), cleartext.map { it.textContent.trim() })
        assertEquals("false", cleartext.single().getAttribute("includeSubdomains"))
    }
}

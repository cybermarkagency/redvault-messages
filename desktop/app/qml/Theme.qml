// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

pragma Singleton
import QtQuick
import QtCore

// Design tokens for the RedVaultMessages redesign: graphite surfaces
// (#101114, #17191E, #20232A), a restrained burgundy accent (#A72640) and
// crisp dividers, with a light palette that has the same roles (the Android
// app's Theme.kt has the same values). Settings > Appearance chooses System,
// Dark (the default) or Light; the choice is kept on this computer.
//
// The burgundy is too dark to read as text on graphite, so it has two roles:
// accentFill (#A72640, white text on it) for buttons, outgoing bubbles,
// badges and selections, and accent, a lighter raspberry for text and icons.
// "ok" (green) is kept for confirmed states such as Verified and Tor
// connected, so they never look like an ordinary accent; danger is coral so
// errors never look like the accent either. Every state colour is paired
// with an icon or words where it's used.
QtObject {
    id: theme

    /// "system", "dark" or "light".
    readonly property string mode: store.mode
    function setMode(value) {
        if (value === "system" || value === "dark" || value === "light") store.mode = value
    }
    readonly property bool dark: mode === "light" ? false
                               : mode === "dark" ? true
                               : Application.styleHints.colorScheme !== Qt.ColorScheme.Light

    property Settings store: Settings {
        category: "appearance"
        property string mode: "dark"
    }

    function pick(darkValue, lightValue) { return theme.dark ? darkValue : lightValue }

    /// Message rich text from the core (model/text_format) uses the dark palette's colours for links,
    /// mentions and hidden spoilers: swap them for this theme's.
    /// linkColor: optional, for text on a filled surface (white on your burgundy bubbles).
    function themedHtml(html, linkColor) {
        if (html.length === 0) return html
        return html.split("#86D9AE").join(String(linkColor !== undefined ? linkColor : theme.accent))
                   .split("#244536").join(String(theme.accentBorder))
                   .split("#3A443F").join(String(theme.borderStrong))
    }

    // Surfaces, darkest to lightest
    readonly property color rail: pick("#0C0D10", "#ECECEF")
    readonly property color bg: pick("#101114", "#F6F6F8")
    readonly property color panel: pick("#14161A", "#F1F1F4")
    readonly property color surface: pick("#17191E", "#FFFFFF")   // cards, settings groups, chat list
    readonly property color surface2: pick("#20232A", "#EFEFF2")  // inputs, secondary buttons, chips ("raised")
    readonly property color surface3: pick("#282B33", "#E4E4E9")  // call controls, tiles ("control"), hover on raised
    readonly property color selected: pick("#22252C", "#ECE7EA")  // selected row
    readonly property color hover: pick("#1B1D22", "#F0EFF2")     // hovered row
    readonly property color border: pick("#25282F", "#E2E2E7")
    readonly property color borderStrong: pick("#323640", "#CBCBD3")

    // Text
    readonly property color text: pick("#EDEEF0", "#15161A")
    readonly property color text2: pick("#C3C6CD", "#3E4049")
    readonly property color text3: pick("#9599A3", "#646772")     // metadata, timestamps
    readonly property color placeholder: pick("#858A94", "#7A7D87")

    // Accent: primary actions, selection, your own messages
    readonly property color accent: pick("#E7677F", "#A72640")         // text and icons
    readonly property color accentFill: "#A72640"                      // fills, with accentContrast on them
    readonly property color accentFillHover: pick("#B92D4A", "#931F37")
    readonly property color accentContrast: "#FFFFFF"
    readonly property color accentSoft: pick("#2A1820", "#F8E7EB")
    readonly property color accentBorder: pick("#5C2635", "#EBC1CB")
    readonly property color focusRing: pick("#F08AA0", "#A72640")      // keyboard focus outline
    readonly property color bubbleIn: pick("#20232A", "#FFFFFF")       // charcoal incoming bubble
    readonly property color bubbleInBorder: pick("#2A2D35", "#E2E2E7")
    readonly property color bubbleOut: pick("#8E2238", "#A72640")      // burgundy outgoing bubble
    readonly property color bubbleOutText: "#FFFFFF"
    readonly property color bubbleOutMeta: pick("#F0C3CD", "#F7D9E0")

    // Confirmed states: Verified, Tor connected, saved
    readonly property color ok: pick("#6FCF97", "#1E7A4C")
    readonly property color okSoft: pick("#15241C", "#E3F2E9")
    readonly property color okBorder: pick("#24432F", "#B7DCC6")

    // States
    readonly property color danger: pick("#F68C72", "#B3361F")        // text: missed calls, Block, Report, errors
    readonly property color dangerStrong: "#C9402B"                   // fills: end / decline call
    readonly property color dangerSoft: pick("#2B1814", "#FCEAE5")
    readonly property color dangerBorder: pick("#5E2C22", "#F1C2B6")
    readonly property color warn: pick("#EBBE7A", "#94590A")
    readonly property color warnSoft: pick("#29231A", "#FCF1DF")
    readonly property color warnBorder: pick("#4B3F27", "#EED3A8")
    readonly property color warnSurface: pick("#30291B", "#FBEBD3")
    readonly property color warnSurface2: pick("#28231A", "#FDF3E2")
    readonly property color yoursBg: pick("#2A3352", "#DCE3FA")
    readonly property color yoursText: pick("#C6D0F5", "#2A3B8F")
    readonly property color toggleOnBg: pick("#EDEEF0", "#15161A")
    readonly property color toggleOnFg: pick("#101114", "#FFFFFF")
    readonly property color callBg: "#0B0C0F"                         // calls stay dark in both themes
    readonly property color disabledText: pick("#5F636C", "#A6A8B0")  // disabled controls: dimmed, never only a colour change
    // Extra locks on files: key and password pills, locked cards
    readonly property color lockBg: pick("#1C2133", "#E8EDFF")
    readonly property color lockText: pick("#C6D0F5", "#2F44A8")
    readonly property color lockSurface: pick("#151927", "#F2F5FF")
    readonly property color lockBorder: pick("#2B3352", "#C4CFF5")

    // Stories: the ring around someone with a new story (close friends: green),
    // the stage behind a story, and the named colours a story can use (its
    // encrypted details name one; the web app's --story-* tokens are the same).
    readonly property var storyRing: ["#F2C38B", "#F29B90", "#D66BB7", "#8F7CF2"]
    readonly property color storyRingClose: "#4CD07D"
    readonly property color storyRingSeen: pick("#3A3D45", "#C9C9D0")
    readonly property color storyStage: "#000000"
    readonly property color storyChrome: "#EBFFFFFF"
    readonly property color storyTrack: "#59FFFFFF"
    readonly property color storyScrim: "#80000000"
    readonly property var storyColors: ({ white: "#FFFFFF", black: "#111111", red: "#EF4444", orange: "#F97316", yellow: "#FACC15",
                                          green: "#22C55E", teal: "#14B8A6", blue: "#3B82F6", purple: "#A855F7", pink: "#EC4899" })
    readonly property var storyLightColors: ["white", "yellow", "orange", "green", "teal"]
    readonly property var storyBackgrounds: ({ sunset: ["#F97316", "#EC4899"], ocean: ["#0EA5E9", "#312E81"], forest: ["#22C55E", "#134E4A"],
                                               berry: ["#A855F7", "#BE185D"], night: ["#1E293B", "#020617"], ember: ["#FACC15", "#DC2626"],
                                               mint: ["#99F6E4", "#0D9488"], slate: ["#64748B", "#1F2937"] })

    // Avatar tints, with dark initials
    readonly property color avatarInk: "#141413"
    readonly property var avatarTints: ["#C9B6E4", "#9CC9E8", "#F2C38B", "#B9D98C", "#E8A7A0", "#E6D48A"]
    function tintFor(key) {
        let h = 0
        const s = String(key)
        for (let i = 0; i < s.length; i++)
            h = (h * 31 + s.charCodeAt(i)) >>> 0
        return avatarTints[h % avatarTints.length]
    }

    // Radii
    readonly property int radiusSmall: 8      // small buttons, inputs
    readonly property int radiusControl: 10   // buttons, rows
    readonly property int radiusCard: 14      // cards
    readonly property int radiusDialog: 18    // dialogs, pop-ups
    readonly property int radiusBubble: 16    // message bubbles

    // Layout
    readonly property int railWidth: 76
    readonly property int listWidth: 340
    readonly property int listWidthCompact: 280   // narrow windows
    readonly property int detailsWidth: 320
    readonly property int headerHeight: 64

    // Type
    readonly property string fontFamily: "Inter"
    readonly property string monoFamily: "IBM Plex Mono"
    readonly property string displayFamily: "Inter"         // headings: Inter Bold, like the rest
    readonly property int fontSize: 14
    readonly property int fontSmall: 12
    readonly property int fontTitle: 17
    readonly property int fontDisplay: 22
}

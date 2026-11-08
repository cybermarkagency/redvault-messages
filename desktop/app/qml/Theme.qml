// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

pragma Singleton
import QtQuick

// Design tokens from the desktop UI handoff, the same as the web app's
// ui/src/styles/tokens.css: dark, with a green accent. The app is dark-only,
// like the design. The older names (surface2, text3, …) map onto the new
// palette so every screen shares it.
QtObject {
    readonly property bool dark: true

    // Surfaces, darkest to lightest
    readonly property color rail: "#0B0E0D"
    readonly property color bg: "#0F1311"
    readonly property color panel: "#121715"
    readonly property color surface: "#171C1A"   // cards, settings groups
    readonly property color surface2: "#1B211E"  // inputs, secondary buttons, chips ("raised")
    readonly property color surface3: "#1F2623"  // call controls, tiles ("control")
    readonly property color selected: "#1D2421"  // selected row, incoming bubble
    readonly property color border: "#1F2623"
    readonly property color borderStrong: "#26302B"

    // Text
    readonly property color text: "#ECEAE3"
    readonly property color text2: "#C9CFCA"
    readonly property color text3: "#9AA39D"     // metadata, timestamps
    readonly property color placeholder: "#8A938D"

    // Accent: security, verified, primary actions
    readonly property color accent: "#86D9AE"
    readonly property color accentContrast: "#0B2A1C"
    readonly property color accentSoft: "#16271F"
    readonly property color accentBorder: "#244536"
    readonly property color bubbleOut: "#1E4434"

    // States
    readonly property color danger: "#F29B90"        // text: missed calls, Block, Report
    readonly property color dangerStrong: "#C8433B"  // fills: end / decline call
    readonly property color dangerSoft: "#2A1714"
    readonly property color warn: "#F2C38B"
    readonly property color warnSoft: "#2A2217"
    readonly property color yoursBg: "#2A3B6A"
    readonly property color yoursText: "#C6D4FF"
    readonly property color toggleOnBg: "#ECEAE3"
    readonly property color toggleOnFg: "#0F1311"
    readonly property color callBg: "#0A0D0C"
    // Extra locks on files: key and password pills, locked cards
    readonly property color lockBg: "#1B2236"
    readonly property color lockText: "#C6D4FF"
    readonly property color lockSurface: "#141A29"
    readonly property color lockBorder: "#2A3556"

    // Stories: the ring around someone with a new story (close friends: green),
    // the stage behind a story, and the named colours a story can use (its
    // encrypted details name one; the web app's --story-* tokens are the same).
    readonly property var storyRing: ["#F2C38B", "#F29B90", "#D66BB7", "#8F7CF2"]
    readonly property color storyRingClose: "#4CD07D"
    readonly property color storyRingSeen: "#3A433E"
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
    readonly property int radiusSmall: 10     // small buttons, inputs
    readonly property int radiusControl: 12   // buttons, rows
    readonly property int radiusCard: 16      // cards
    readonly property int radiusDialog: 22    // dialogs, pop-ups

    // Layout
    readonly property int railWidth: 76
    readonly property int listWidth: 360
    readonly property int detailsWidth: 320
    readonly property int headerHeight: 72

    // Type
    readonly property string fontFamily: "IBM Plex Sans"
    readonly property string monoFamily: "IBM Plex Mono"
    readonly property string displayFamily: "Bricolage Grotesque"
    readonly property int fontSize: 14
    readonly property int fontSmall: 12
    readonly property int fontTitle: 17
    readonly property int fontDisplay: 26
}

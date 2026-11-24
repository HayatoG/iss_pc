pragma Singleton
import QtQuick
import Launcher

// Every color, size and font of the launcher lives here (single source for the visual identity).
QtObject {
    property Item glassBackdrop: null
    property real glassRevision: 0
    property color surfaceAccent: "#ff6a2b"
    readonly property int motionDuration: Backend.reducedMotion ? 0 : 180
    // surfaces
    readonly property color bg: "#101216"
    readonly property color rail: "#0d0f12"
    readonly property color railLine: "#1e2228"
    readonly property color navHover: "#1a1d23"
    readonly property color navOn: "#1d2027"
    readonly property color navOnLine: "#2c313a"
    readonly property color card: "#202c3a"
    readonly property color cardLine: Qt.rgba(surfaceAccent.r, surfaceAccent.g, surfaceAccent.b, 0.34)
    readonly property color chip: "#171a20"
    readonly property color chipLine: "#2a2f38"
    readonly property color ghost: "#394451"
    readonly property color ghostHover: "#4a5664"
    readonly property color ghostLine: "#64707e"
    readonly property color ghostLineHover: "#9aa5b1"
    // text
    readonly property color text: "#e9e6df"
    // 2026-11-22: the secondary texts (descriptions, hints, notes) brightened so they stand out from
    // the dark cards; the order (soft > muted > dim > faint) is kept. Before: c9c5bd d8d5ce a3a9b3 8e949f 7d838e.
    readonly property color textSoft: "#e2ded6"
    readonly property color textChip: "#e6e3dc"
    readonly property color textMuted: "#c8ccd3"
    readonly property color textDim: "#bcc1c9"
    readonly property color textFaint: "#a9afb9"
    // states
    readonly property color ok: "#35d07f"
    readonly property color warn: "#f5c400"   // the checkpoint-tape yellow
    readonly property color bad: "#ff4d4d"
    readonly property color off: "#5a606b"
    // one accent per section
    readonly property color smoke: "#ff6a2b"  // Play, Fixes
    readonly property color video: "#4d8bff"  // Graphics
    readonly property color neon: "#ff3d9a"   // Controls
    readonly property color cyan: "#19e3ff"   // Support
    readonly property var accents: ({ play: "#ff6a2b", graphics: "#4d8bff", controls: "#ff3d9a",
                                      fixes: "#ff6a2b", saves: "#35d07f", guides: "#ff3d9a", support: "#19e3ff",
                                      updates: "#f5c400", advanced: "#8e7bff", mods: "#2bd9c4", trophies: "#eebf4c" })
    // type
    property string fontDisplay: fontText
    property string fontText: "Segoe UI Variable Text"
    readonly property int railWidth: 236
    readonly property int heroHeight: 320
    // line icons, 24x24 viewBox (circles written as arcs)
    readonly property var icons: ({
        play: "M7 4.5v15l12-7.5z",
        graphics: "M4 7h10M18 7h2M4 17h2M10 17h10 M18 7a2 2 0 1 0-4 0a2 2 0 1 0 4 0 M10 17a2 2 0 1 0-4 0a2 2 0 1 0 4 0",
        controls: "M7 8h10a4 4 0 0 1 4 4l-1 5a2 2 0 0 1-3.4 1L15 16H9l-1.6 2A2 2 0 0 1 4 17l-1-5a4 4 0 0 1 4-4z",
        fixes: "M12 3l8 3v6c0 4.5-3.2 7.8-8 9-4.8-1.2-8-4.5-8-9V6z M8.5 12l2.5 2.5 4.5-5",
        guides: "M7 8h10a4 4 0 0 1 4 4l-1 5a2 2 0 0 1-3.4 1L15 16H9l-1.6 2A2 2 0 0 1 4 17l-1-5a4 4 0 0 1 4-4z M8 10.5v3M6.5 12h3 M15.5 11.5h.01M17.5 13.5h.01",
        saves: "M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z M12 10.5v5M9.5 13.5 12 16l2.5-2.5",
        support: "M21 12a9 9 0 1 0-18 0a9 9 0 1 0 18 0 M15.5 12a3.5 3.5 0 1 0-7 0a3.5 3.5 0 1 0 7 0 M5.6 5.6l3.9 3.9M14.5 14.5l3.9 3.9M18.4 5.6l-3.9 3.9M9.5 14.5l-3.9 3.9",
        updates: "M12 4v11M7.5 11 12 15.5 16.5 11M5 20h14",
        advanced: "M4 6h16M4 12h16M4 18h16 M9 6a2 2 0 1 0-4 0a2 2 0 1 0 4 0 M19 12a2 2 0 1 0-4 0a2 2 0 1 0 4 0 M12 18a2 2 0 1 0-4 0a2 2 0 1 0 4 0",
        mods: "M6 9h12a3 3 0 0 1 0 6H6a3 3 0 0 1 0-6z M17.2 12a2.2 2.2 0 1 0-4.4 0a2.2 2.2 0 1 0 4.4 0",
        trophies: "M8 4h8v5a4 4 0 0 1-8 0z M8 6H5.5a2.5 2.5 0 0 0 2.6 4 M16 6h2.5a2.5 2.5 0 0 1-2.6 4 M12 13v3.5 M9 20h6 M10 16.5h4v3.5h-4z"
    })

    // Translation: reading Backend.lang inside makes every binding that calls it re-evaluate
    // when the player switches language.
    function tr(key) { var l = Backend.lang; return Backend.t(key) }
}

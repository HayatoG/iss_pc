import QtQuick
import QtQuick.Effects
import Launcher

// Local, reusable glass material. Only sample a separate backdrop, never an
// ancestor containing this surface: that would create a render feedback loop.
Item {
    id: glass
    property Item sourceItem: Theme.glassBackdrop
    property real radius: 22
    property color tint: "#162130"
    property real density: 0.48
    property color accent: Theme.surfaceAccent
    property bool highlighted: false
    property bool pressed: false
    property bool focused: false
    property bool blurEnabled: true
    property real sampleOffset: 0
    property point light: Qt.point(0.28, 0.0)
    // Referencing the scene dimensions also updates the crop on resize.
    readonly property point sampleOrigin: {
        var revision = sampleOffset + Theme.glassRevision
        if (!sourceItem) return Qt.point(0, 0)
        var size = sourceItem.width + sourceItem.height + width + height
        return mapToItem(sourceItem, 0, 0)
    }

    Rectangle {
        x: 0; y: 4; width: glass.width; height: glass.height
        radius: glass.radius; color: "#24000000"
    }
    ShaderEffectSource {
        id: sample
        sourceItem: glass.blurEnabled ? glass.sourceItem : null
        sourceRect: Qt.rect(glass.sampleOrigin.x, glass.sampleOrigin.y, glass.width, glass.height)
        textureSize: Qt.size(Math.max(1, glass.width / 2), Math.max(1, glass.height / 2))
        live: glass.visible && glass.blurEnabled; hideSource: false; visible: false
    }
    Rectangle {
        id: mask
        width: glass.width; height: glass.height
        radius: glass.radius; color: "white"
        layer.enabled: glass.blurEnabled; visible: false
    }
    MultiEffect {
        anchors.fill: parent
        visible: glass.sourceItem !== null && glass.blurEnabled
        source: sample
        blurEnabled: true; blurMax: 24; blur: 0.66
        saturation: 0.65; contrast: 0.08; brightness: -0.08
        autoPaddingEnabled: false
        maskEnabled: true; maskSource: mask
    }
    Rectangle {
        anchors.fill: parent; radius: glass.radius
        color: Qt.rgba(glass.tint.r, glass.tint.g, glass.tint.b, glass.density)
        border.width: 1
        border.color: glass.focused ? "#f0ffffff" : Qt.rgba(glass.accent.r, glass.accent.g, glass.accent.b, glass.highlighted ? 0.82 : 0.46)
        Behavior on border.color { ColorAnimation { duration: Theme.motionDuration } }
        gradient: Gradient {
            GradientStop { position: 0; color: Qt.rgba(glass.tint.r, glass.tint.g, glass.tint.b, glass.density * 0.80) }
            GradientStop { position: 0.25; color: Qt.rgba(glass.tint.r, glass.tint.g, glass.tint.b, glass.density * 0.90) }
            GradientStop { position: 0.72; color: Qt.rgba(glass.tint.r, glass.tint.g, glass.tint.b, glass.density) }
            GradientStop { position: 1; color: Qt.rgba(glass.tint.r, glass.tint.g, glass.tint.b, glass.density * 0.88) }
        }
    }
    Rectangle {
        anchors.fill: parent; anchors.margins: 1; radius: Math.max(0, glass.radius - 1)
        gradient: Gradient {
            GradientStop { position: 0; color: Qt.rgba(1, 1, 1, glass.pressed ? 0.14 : glass.highlighted ? 0.48 : 0.32) }
            GradientStop { position: 0.06; color: "#18ffffff" }
            GradientStop { position: 0.22; color: "#00ffffff" }
            GradientStop { position: 0.78; color: "#00ffffff" }
            GradientStop { position: 1; color: Qt.rgba(glass.accent.r, glass.accent.g, glass.accent.b, glass.highlighted ? 0.20 : 0.10) }
        }
    }
    Rectangle {
        anchors.fill: parent; anchors.margins: 2; radius: Math.max(0, glass.radius - 2)
        color: "transparent"; border.width: 1; border.color: "#16ffffff"
    }
    Rectangle {
        x: glass.radius; y: 1; width: Math.max(0, glass.width - 2 * glass.radius); height: 1
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: 0; color: "#00ffffff" }
            GradientStop { position: Math.max(0.01, Math.min(0.99, glass.light.x)); color: glass.highlighted ? "#ffffffff" : "#d8ffffff" }
            GradientStop { position: 1; color: "#00ffffff" }
        }
    }
}

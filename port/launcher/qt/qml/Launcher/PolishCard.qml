import QtQuick
import Launcher

// The Play tab's card look for every other tab (2026-11-23): a deep gradient body, a soft shadow under
// it so it lifts off the page, a faint rim and a thin highlight along the top edge. A drop-in for the
// plain card Rectangle: pages keep setting radius, border.color and their contents; a page's `color`
// is ignored (the gradient wins).
Rectangle {
    id: card
    property color shadeTop: Qt.rgba(Theme.surfaceAccent.r * 0.20 + 0.025, Theme.surfaceAccent.g * 0.20 + 0.025, Theme.surfaceAccent.b * 0.20 + 0.025, 0.46)
    property color shadeBottom: Qt.rgba(Theme.surfaceAccent.r * 0.08 + 0.015, Theme.surfaceAccent.g * 0.08 + 0.015, Theme.surfaceAccent.b * 0.08 + 0.015, 0.70)
    radius: 14
    border.width: 1
    border.color: Qt.rgba(Theme.surfaceAccent.r, Theme.surfaceAccent.g, Theme.surfaceAccent.b, 0.40)
    gradient: Gradient {
        GradientStop { position: 0; color: card.shadeTop }
        GradientStop { position: 1; color: card.shadeBottom }
    }
    Rectangle {
        anchors.fill: parent; anchors.margins: 1; radius: Math.max(0, card.radius - 1)
        gradient: Gradient {
            GradientStop { position: 0; color: "#42ffffff" }
            GradientStop { position: 0.045; color: "#0affffff" }
            GradientStop { position: 0.16; color: "#00ffffff" }
            GradientStop { position: 0.85; color: "#00ffffff" }
            GradientStop { position: 1; color: Qt.rgba(Theme.surfaceAccent.r, Theme.surfaceAccent.g, Theme.surfaceAccent.b, 0.08) }
        }
    }
    Rectangle {   // shadow, behind the card (negative z draws under the parent)
        z: -1
        x: 0; y: 4; width: card.width; height: card.height; radius: card.radius
        color: Qt.rgba(0, 0, 0, 0.28)
        visible: card.visible
    }
    Rectangle {   // highlight on the top edge
        z: 10
        x: card.radius; y: 1; width: Math.max(0, card.width - 2 * card.radius); height: 1
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: 0; color: Qt.rgba(1, 1, 1, 0) }
            GradientStop { position: 0.5; color: Qt.rgba(1, 1, 1, 0.68) }
            GradientStop { position: 1; color: Qt.rgba(1, 1, 1, 0) }
        }
    }
}

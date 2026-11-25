import QtQuick
import QtQuick.Shapes
import Launcher

// Spotlight rays behind an earned Platinum trophy (2026-11-22): golden-white beams fanning out from the
// centre, slowly turning a full 360 degrees, with a soft pulsing glow in the middle. Still when the
// player asked for reduced motion. Centre it on the trophy; `radius` is how far the beams reach.
Item {
    id: root
    property real radius: 120
    property int beams: 12
    property real spread: 8.5          // half-width of a beam, in degrees
    property bool running: true
    property color color: "#ffe39a"
    width: 2 * radius; height: 2 * radius

    function beamPath() {
        var p = "", c = root.radius, r = root.radius
        for (var i = 0; i < root.beams; i++) {
            var a = (i / root.beams) * 2 * Math.PI, s = root.spread * Math.PI / 180
            p += "M " + c + " " + c
               + " L " + (c + r * Math.cos(a - s)) + " " + (c + r * Math.sin(a - s))
               + " L " + (c + r * Math.cos(a + s)) + " " + (c + r * Math.sin(a + s)) + " Z "
        }
        return p
    }

    Repeater {   // pulsing glow in the middle
        model: 4
        Rectangle {
            anchors.centerIn: parent
            width: root.radius * (0.55 + index * 0.22) * (1 + 0.06 * glowPulse.value); height: width; radius: width / 2
            color: Qt.rgba(1, 0.85, 0.45, 0.20 - index * 0.04)
        }
    }
    QtObject { id: glowPulse; property real value: 0 }
    SequentialAnimation {
        running: root.running && root.visible && !Backend.reducedMotion; loops: Animation.Infinite
        NumberAnimation { target: glowPulse; property: "value"; from: 0; to: 1; duration: 1400; easing.type: Easing.InOutSine }
        NumberAnimation { target: glowPulse; property: "value"; from: 1; to: 0; duration: 1400; easing.type: Easing.InOutSine }
    }

    Item {   // the beams, two layers turning in opposite directions for depth
        anchors.fill: parent
        Shape {
            id: beamsA
            anchors.fill: parent
            preferredRendererType: Shape.CurveRenderer
            opacity: 1.0
            ShapePath {
                strokeWidth: -1
                fillGradient: RadialGradient {
                    centerX: root.radius; centerY: root.radius; centerRadius: root.radius
                    focalX: root.radius; focalY: root.radius
                    GradientStop { position: 0.0; color: Qt.rgba(1, 0.98, 0.88, 1.0) }
                    GradientStop { position: 0.4; color: Qt.rgba(1, 0.86, 0.45, 0.75) }
                    GradientStop { position: 1.0; color: Qt.rgba(1, 0.80, 0.30, 0.0) }
                }
                PathSvg { path: root.beamPath() }
            }
            RotationAnimation on rotation {
                running: root.running && root.visible && !Backend.reducedMotion
                from: 0; to: 360; duration: 16000; loops: Animation.Infinite
            }
        }
        Shape {
            anchors.fill: parent
            preferredRendererType: Shape.CurveRenderer
            opacity: 0.6
            rotation: 360 / root.beams / 2
            ShapePath {
                strokeWidth: -1
                fillGradient: RadialGradient {
                    centerX: root.radius; centerY: root.radius; centerRadius: root.radius * 0.8
                    focalX: root.radius; focalY: root.radius
                    GradientStop { position: 0.0; color: Qt.rgba(1, 1, 1, 0.7) }
                    GradientStop { position: 1.0; color: Qt.rgba(1, 0.85, 0.40, 0.0) }
                }
                PathSvg { path: root.beamPath() }
            }
            RotationAnimation on rotation {
                running: root.running && root.visible && !Backend.reducedMotion
                from: 360 / root.beams / 2; to: 360 / root.beams / 2 - 360; duration: 26000; loops: Animation.Infinite
            }
        }
    }
}

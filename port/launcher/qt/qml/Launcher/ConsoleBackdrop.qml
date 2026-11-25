import QtQuick
import Launcher

Item {
    id: scene
    property bool artwork: true
    property real phase: 0
    property int displayedIndex: Backend.gameIndex
    Rectangle { anchors.fill: parent; color: "#003d86" }
    property url secondSonArt: ""
    property url firstLightArt: ""
    property bool secondSonAnimated: false
    property bool firstLightAnimated: false
    function rememberArtwork() {
        var source = Backend.artSource, animated = Backend.artAnimated
        if (Backend.gameIndex === 1) { firstLightArt = source; firstLightAnimated = animated }
        else { secondSonArt = source; secondSonAnimated = animated }
    }
    Component.onCompleted: rememberArtwork()
    Connections { target: Backend; function onGameDirChanged() { scene.rememberArtwork() } }
    Repeater {
        model: 2
        Item {
            anchors.fill: parent
            readonly property bool chosen: scene.artwork && Backend.gameIndex === index
            readonly property url art: index === 0 ? scene.secondSonArt : scene.firstLightArt
            readonly property bool animated: index === 0 ? scene.secondSonAnimated : scene.firstLightAnimated
            readonly property bool ready: animated ? moving.status === Image.Ready : still.status === Image.Ready
            onChosenChanged: if (chosen && ready) scene.displayedIndex = index
            opacity: scene.artwork && scene.displayedIndex === index ? 0.78 : 0
            Behavior on opacity { enabled: !Backend.reducedMotion; OpacityAnimator { duration: 190; easing.type: Easing.OutCubic } }
            Image {
                id: still
                onStatusChanged: if (status === Image.Ready && parent.chosen) scene.displayedIndex = index
                anchors.fill: parent; source: parent.animated ? "" : parent.art
                sourceSize: Qt.size(1920,1080); cache: true; asynchronous: true; smooth: true; mipmap: true
                fillMode: Image.PreserveAspectCrop; horizontalAlignment: Image.AlignRight
            }
            AnimatedImage {
                id: moving
                onStatusChanged: if (status === Image.Ready && parent.chosen) scene.displayedIndex = index
                anchors.fill: parent; source: parent.animated ? parent.art : ""
                visible: parent.animated; playing: scene.visible && parent.chosen && !Backend.reducedMotion
                fillMode: Image.PreserveAspectCrop; smooth: true
            }
        }
    }
    Rectangle {
        anchors.fill: parent
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: 0; color: "#ee00275f" }
            GradientStop { position: 0.48; color: "#bd003273" }
            GradientStop { position: 1; color: "#3a002b62" }
        }
    }
    Rectangle {
        anchors.fill: parent
        gradient: Gradient {
            GradientStop { position: 0; color: "#45002056" }
            GradientStop { position: 0.50; color: "#00001e54" }
            GradientStop { position: 1; color: "#e800153e" }
        }
    }
    Canvas {
        id: wave; x: -36; width: parent.width + 72; height: parent.height; opacity: scene.artwork ? 0.45 : 0.8
        onWidthChanged: requestPaint(); onHeightChanged: requestPaint()
        SequentialAnimation {
            running: scene.visible && !Backend.reducedMotion; loops: Animation.Infinite
            XAnimator { target: wave; from: -36; to: 0; duration: 14000; easing.type: Easing.InOutSine }
            XAnimator { target: wave; from: 0; to: -36; duration: 14000; easing.type: Easing.InOutSine }
        }
        onPaint: {
            var ctx = getContext("2d"); ctx.reset()
            for (var i = 0; i < 4; ++i) {
                ctx.beginPath()
                for (var x = -8; x <= width + 8; x += 8) {
                    var y = height * (0.72 + i * 0.035) + Math.sin(x / width * 5.2 + scene.phase + i * 0.24) * height * 0.09
                    if (x === -8) ctx.moveTo(x, y); else ctx.lineTo(x, y)
                }
                ctx.strokeStyle = Qt.rgba(0.38, 0.74, 1, 0.13 - i * 0.02); ctx.lineWidth = i === 0 ? 2 : 1; ctx.stroke()
            }
        }
    }
}

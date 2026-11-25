import QtQuick
import Launcher

Canvas {
    id: scene
    property color accent: Theme.smoke
    property real strength: 1.0
    onStrengthChanged: requestPaint()
    onAccentChanged: requestPaint()
    onWidthChanged: requestPaint()
    onHeightChanged: requestPaint()
    onPaint: {
        var ctx = getContext("2d"); ctx.reset()
        ctx.fillStyle = "#101216"; ctx.fillRect(0, 0, width, height)
        function glow(x, y, r, c, a) {
            var g = ctx.createRadialGradient(x, y, 0, x, y, r)
            g.addColorStop(0, Qt.rgba(c.r, c.g, c.b, a * scene.strength))
            g.addColorStop(1, Qt.rgba(c.r, c.g, c.b, 0))
            ctx.fillStyle = g; ctx.fillRect(0, 0, width, height)
        }
        glow(width * 0.08, height * 0.12, height * 0.65, accent, 0.43)
        glow(width * 0.15, height * 0.90, height * 0.65, accent, 0.18)
        glow(width * 0.85, 0, width * 0.55, accent, 0.26)
    }
}

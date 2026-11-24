import QtQuick
import Launcher

// A small PS4 controller button drawn in code (2026-11-23), shown next to each keyboard binding on the
// Controls tab so the player sees which button a key presses. glyph: cross circle square triangle
// l1 r1 l2 r2 l3 r3 ls rs options up down left right touchpad_left touchpad_right tilt_left tilt_right shake.
Canvas {
    id: g
    property string glyph: ""
    implicitWidth: 40; implicitHeight: 32
    width: implicitWidth; height: implicitHeight
    antialiasing: true
    onGlyphChanged: requestPaint()
    onWidthChanged: requestPaint(); onHeightChanged: requestPaint()

    readonly property color body: "#262a32"
    readonly property color rim: "#4a505c"
    readonly property color ink: "#e9e6df"
    readonly property color lit: "#ff3d9a"     // the Controls accent: the part of the pad that is meant

    function roundRect(ctx, x, y, w, h, r) {
        ctx.beginPath()
        ctx.moveTo(x + r, y); ctx.lineTo(x + w - r, y); ctx.arcTo(x + w, y, x + w, y + r, r)
        ctx.lineTo(x + w, y + h - r); ctx.arcTo(x + w, y + h, x + w - r, y + h, r)
        ctx.lineTo(x + r, y + h); ctx.arcTo(x, y + h, x, y + h - r, r)
        ctx.lineTo(x, y + r); ctx.arcTo(x, y, x + r, y, r); ctx.closePath()
    }
    function disc(ctx, cx, cy, r) {
        ctx.beginPath(); ctx.arc(cx, cy, r, 0, 2 * Math.PI); ctx.fillStyle = body; ctx.fill()
        ctx.lineWidth = 1.5; ctx.strokeStyle = rim; ctx.stroke()
    }
    function label(ctx, text, cx, cy, size) {
        ctx.fillStyle = ink; ctx.font = "bold " + size + "px sans-serif"
        ctx.textAlign = "center"; ctx.textBaseline = "middle"; ctx.fillText(text, cx, cy + 0.5)
    }
    function pad(ctx, cx, cy, s, angle) {   // a small controller outline
        ctx.save(); ctx.translate(cx, cy); ctx.rotate(angle)
        ctx.beginPath()
        ctx.moveTo(-9 * s, -5 * s); ctx.lineTo(9 * s, -5 * s)
        ctx.quadraticCurveTo(13 * s, -5 * s, 13 * s, 2 * s); ctx.lineTo(12 * s, 7 * s)
        ctx.quadraticCurveTo(10 * s, 10 * s, 7 * s, 6 * s); ctx.lineTo(-7 * s, 6 * s)
        ctx.quadraticCurveTo(-10 * s, 10 * s, -12 * s, 7 * s); ctx.lineTo(-13 * s, 2 * s)
        ctx.quadraticCurveTo(-13 * s, -5 * s, -9 * s, -5 * s); ctx.closePath()
        ctx.fillStyle = body; ctx.fill(); ctx.lineWidth = 1.5; ctx.strokeStyle = lit; ctx.stroke()
        ctx.restore()
    }

    onPaint: {
        var ctx = getContext("2d"); ctx.reset()
        var w = width, h = height, cx = w / 2, cy = h / 2, r = Math.min(w, h) / 2 - 1.5
        ctx.lineCap = "round"; ctx.lineJoin = "round"
        switch (glyph) {
        case "cross":
            disc(ctx, cx, cy, r); ctx.strokeStyle = "#8fb3ff"; ctx.lineWidth = 2.2; ctx.beginPath()
            ctx.moveTo(cx - 6, cy - 6); ctx.lineTo(cx + 6, cy + 6); ctx.moveTo(cx + 6, cy - 6); ctx.lineTo(cx - 6, cy + 6); ctx.stroke(); break
        case "circle":
            disc(ctx, cx, cy, r); ctx.strokeStyle = "#ff6f7d"; ctx.lineWidth = 2.2; ctx.beginPath(); ctx.arc(cx, cy, 6.5, 0, 2 * Math.PI); ctx.stroke(); break
        case "square":
            disc(ctx, cx, cy, r); ctx.strokeStyle = "#f294d6"; ctx.lineWidth = 2.2; ctx.strokeRect(cx - 6, cy - 6, 12, 12); break
        case "triangle":
            disc(ctx, cx, cy, r); ctx.strokeStyle = "#45dbb4"; ctx.lineWidth = 2.2; ctx.beginPath()
            ctx.moveTo(cx, cy - 7); ctx.lineTo(cx + 7, cy + 5); ctx.lineTo(cx - 7, cy + 5); ctx.closePath(); ctx.stroke(); break
        case "l1": case "r1":
            roundRect(ctx, 2, cy - 7, w - 4, 14, 5); ctx.fillStyle = body; ctx.fill(); ctx.lineWidth = 1.5; ctx.strokeStyle = rim; ctx.stroke()
            label(ctx, glyph.toUpperCase(), cx, cy, 12); break
        case "l2": case "r2":
            ctx.beginPath(); ctx.moveTo(5, h - 3); ctx.lineTo(5, 9); ctx.quadraticCurveTo(5, 2, 12, 2); ctx.lineTo(w - 12, 2)
            ctx.quadraticCurveTo(w - 5, 2, w - 5, 9); ctx.lineTo(w - 5, h - 3); ctx.closePath()
            ctx.fillStyle = body; ctx.fill(); ctx.lineWidth = 1.5; ctx.strokeStyle = rim; ctx.stroke()
            label(ctx, glyph.toUpperCase(), cx, cy + 1, 12); break
        case "l3": case "r3": case "ls": case "rs":
            disc(ctx, cx, cy, r); ctx.beginPath(); ctx.arc(cx, cy, r - 4, 0, 2 * Math.PI)
            ctx.lineWidth = 1.2; ctx.strokeStyle = (glyph === "l3" || glyph === "r3") ? lit : rim; ctx.stroke()
            label(ctx, glyph === "ls" ? "L" : glyph === "rs" ? "R" : glyph.toUpperCase(), cx, cy, glyph.length > 2 || glyph[1] === "3" ? 11 : 13); break
        case "options":
            roundRect(ctx, cx - 8, cy - 11, 16, 22, 7); ctx.fillStyle = body; ctx.fill(); ctx.lineWidth = 1.5; ctx.strokeStyle = rim; ctx.stroke()
            ctx.strokeStyle = ink; ctx.lineWidth = 1.6; ctx.beginPath()
            for (var i = -1; i <= 1; i++) { ctx.moveTo(cx - 3.5, cy + i * 3.5); ctx.lineTo(cx + 3.5, cy + i * 3.5) }
            ctx.stroke(); break
        case "up": case "down": case "left": case "right":
            var a = 5, L = 13
            ctx.fillStyle = body; ctx.strokeStyle = rim; ctx.lineWidth = 1.5
            ctx.beginPath()
            ctx.moveTo(cx - a, cy - L); ctx.lineTo(cx + a, cy - L); ctx.lineTo(cx + a, cy - a); ctx.lineTo(cx + L, cy - a)
            ctx.lineTo(cx + L, cy + a); ctx.lineTo(cx + a, cy + a); ctx.lineTo(cx + a, cy + L); ctx.lineTo(cx - a, cy + L)
            ctx.lineTo(cx - a, cy + a); ctx.lineTo(cx - L, cy + a); ctx.lineTo(cx - L, cy - a); ctx.lineTo(cx - a, cy - a); ctx.closePath()
            ctx.fill(); ctx.stroke()
            ctx.fillStyle = lit
            if (glyph === "up") ctx.fillRect(cx - a + 1, cy - L + 1, 2 * a - 2, L - a - 1)
            else if (glyph === "down") ctx.fillRect(cx - a + 1, cy + a, 2 * a - 2, L - a - 1)
            else if (glyph === "left") ctx.fillRect(cx - L + 1, cy - a + 1, L - a - 1, 2 * a - 2)
            else ctx.fillRect(cx + a, cy - a + 1, L - a - 1, 2 * a - 2)
            break
        case "touchpad_left": case "touchpad_right":
            roundRect(ctx, 2, cy - 8, w - 4, 16, 4); ctx.fillStyle = body; ctx.fill(); ctx.lineWidth = 1.5; ctx.strokeStyle = rim; ctx.stroke()
            ctx.fillStyle = lit
            if (glyph === "touchpad_left") ctx.fillRect(4, cy - 6, (w - 8) / 2 - 1, 12)
            else ctx.fillRect(cx + 1, cy - 6, (w - 8) / 2 - 1, 12)
            break
        case "tilt_left": pad(ctx, cx, cy, 1.2, -0.38); break
        case "tilt_right": pad(ctx, cx, cy, 1.2, 0.38); break
        case "shake":
            pad(ctx, cx, cy, 1.0, 0)
            ctx.strokeStyle = lit; ctx.lineWidth = 1.5; ctx.beginPath()
            ctx.moveTo(1, cy - 4); ctx.lineTo(1, cy + 4); ctx.moveTo(w - 1, cy - 4); ctx.lineTo(w - 1, cy + 4); ctx.stroke()
            break
        }
    }
}

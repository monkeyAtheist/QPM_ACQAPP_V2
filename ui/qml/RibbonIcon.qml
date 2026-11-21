import QtQuick

// Icon of the tool header and of the module bar: strokes drawn on a 24 x 24 grid in the colour given.
// No image file, so it follows the theme and stays sharp whatever the scale of the screen.
// (The drawing helpers and several icons come from QTsignalApp V2.)
Canvas {
    id: icon
    property string name: ""
    property color tint: Theme.text

    width: 22
    height: 22
    onNameChanged: requestPaint()
    onTintChanged: requestPaint()
    onWidthChanged: requestPaint()
    onVisibleChanged: if (visible) requestPaint()

    onPaint: {
        const c = getContext("2d")
        c.reset()
        c.scale(width / 24, height / 24)
        c.lineWidth = 1.7
        c.lineCap = "round"
        c.lineJoin = "round"
        c.strokeStyle = String(icon.tint)
        c.fillStyle = String(icon.tint)
        // A broken line through the points (x0, y0, x1, y1...), closed or not; a circle; a rectangle.
        const line = (points, closed) => {
            c.beginPath()
            c.moveTo(points[0], points[1])
            for (let i = 2; i < points.length; i += 2)
                c.lineTo(points[i], points[i + 1])
            if (closed)
                c.closePath()
            c.stroke()
        }
        const circle = (x, y, r, filled) => {
            c.beginPath()
            c.arc(x, y, r, 0, 2 * Math.PI, false)
            if (filled)
                c.fill()
            else
                c.stroke()
        }
        const box = (x, y, w, h) => {
            c.beginPath()
            c.rect(x, y, w, h)
            c.stroke()
        }
        // A sine wave from x0 to x1 around y, of amplitude a, with n half periods.
        const wave = (x0, x1, y, a, n) => {
            c.beginPath()
            for (let i = 0; i <= 40; ++i) {
                const t = i / 40
                const x = x0 + (x1 - x0) * t
                const v = y - a * Math.sin(Math.PI * n * t)
                if (i === 0)
                    c.moveTo(x, v)
                else
                    c.lineTo(x, v)
            }
            c.stroke()
        }
        switch (icon.name) {
        // ---- Modules ----
        case "signals":
            wave(3, 21, 7, 3, 2)
            wave(3, 21, 17, 2.5, 4)
            break
        case "acquire":
            // A plug going into a board.
            box(3, 14, 18, 7)
            line([9, 14, 9, 10], false)
            line([15, 14, 15, 10], false)
            line([7, 10, 17, 10, 17, 6, 7, 6], true)
            line([12, 6, 12, 2], false)
            break
        case "analysis":
            line([3, 21, 21, 21], false)
            line([5, 21, 5, 15], false)
            line([9, 21, 9, 9], false)
            line([13, 21, 13, 12], false)
            line([17, 21, 17, 17], false)
            circle(16, 7, 3.5, false)
            line([18.5, 9.5, 21, 12], false)
            break
        case "generator":
            wave(3, 15, 12, 5, 2)
            line([16, 12, 21, 12], false)
            line([18.5, 9.5, 21, 12, 18.5, 14.5], false)
            break
        case "filter":
            line([4, 5, 20, 5, 14, 12, 14, 19, 10, 17, 10, 12], true)
            break
        case "modulation":
            for (let i = 0; i < 4; ++i)
                for (let j = 0; j < 4; ++j)
                    circle(5 + i * 4.7, 5 + j * 4.7, 1.2, true)
            break
        // ---- Files and edits ----
        case "open":
            line([3, 19, 3, 5, 9, 5, 11, 7, 19, 7, 19, 10], false)
            line([3, 19, 6, 10, 22, 10, 19, 19], true)
            break
        case "save":
            line([4, 4, 17, 4, 20, 7, 20, 20, 4, 20], true)
            box(8, 4, 8, 5)
            box(7, 13, 10, 7)
            break
        case "import":
            line([12, 3, 12, 14], false)
            line([8, 10, 12, 14, 16, 10], false)
            line([4, 14, 4, 20, 20, 20, 20, 14], false)
            break
        case "export":
            line([12, 14, 12, 3], false)
            line([8, 7, 12, 3, 16, 7], false)
            line([4, 14, 4, 20, 20, 20, 20, 14], false)
            break
        case "rename":
            line([4, 20, 5, 15, 16, 4, 20, 8, 9, 19], true)
            line([14, 6, 18, 10], false)
            break
        case "delete":
            line([4, 7, 20, 7], false)
            line([9, 7, 9, 4, 15, 4, 15, 7], false)
            line([6, 7, 7, 20, 17, 20, 18, 7], false)
            line([10, 11, 10, 16], false)
            line([14, 11, 14, 16], false)
            break
        case "preset":
            line([6, 3, 18, 3, 18, 21, 12, 16, 6, 21], true)
            break
        // ---- Acquisition ----
        case "play":
            line([8, 5, 19, 12, 8, 19], true)
            break
        case "stop":
            box(7, 7, 10, 10)
            break
        case "pause":
            box(7, 6, 3.5, 12)
            box(13.5, 6, 3.5, 12)
            break
        case "discard":
            line([6, 6, 18, 18], false)
            line([18, 6, 6, 18], false)
            break
        case "clear":
            line([9, 20, 4, 15, 14, 5, 20, 11, 11, 20], true)
            line([8, 11, 14, 17], false)
            line([11, 20, 20, 20], false)
            break
        case "freeze":
            line([12, 3, 12, 21], false)
            line([4.2, 7.5, 19.8, 16.5], false)
            line([4.2, 16.5, 19.8, 7.5], false)
            line([10, 5, 12, 7, 14, 5], false)
            line([10, 19, 12, 17, 14, 19], false)
            break
        case "record":
            circle(12, 12, 8, false)
            circle(12, 12, 4.5, true)
            break
        case "trigger":
            line([3, 18, 10, 18, 14, 6, 21, 6], false)
            line([3, 12, 21, 12], false)
            break
        case "rising":
            line([3, 18, 10, 18, 14, 6, 21, 6], false)
            line([10.5, 13, 12, 10.5, 13.5, 13], false)
            break
        case "falling":
            line([3, 6, 10, 6, 14, 18, 21, 18], false)
            line([10.5, 11, 12, 13.5, 13.5, 11], false)
            break
        case "either":
            line([2, 18, 7, 18, 10, 6, 14, 6, 17, 18, 22, 18], false)
            break
        case "single":
            line([3, 18, 9, 18, 12, 6, 15, 18, 21, 18], false)
            circle(12, 3.5, 1.2, true)
            break
        case "rotate":
            c.beginPath()
            c.arc(12, 12, 7.5, 0.25 * Math.PI, 1.75 * Math.PI, false)
            c.stroke()
            line([13.2, 7.2, 17.3, 6.7, 17.6, 2.6], false)
            break
        case "chip":
            box(6, 6, 12, 12)
            for (let k = 0; k < 3; ++k) {
                line([9 + k * 3, 3, 9 + k * 3, 6], false)
                line([9 + k * 3, 18, 9 + k * 3, 21], false)
                line([3, 9 + k * 3, 6, 9 + k * 3], false)
                line([18, 9 + k * 3, 21, 9 + k * 3], false)
            }
            break
        // ---- Sources ----
        case "port":
            line([3, 7, 21, 7, 18, 17, 6, 17], true)
            circle(8, 10.5, 0.9, true)
            circle(12, 10.5, 0.9, true)
            circle(16, 10.5, 0.9, true)
            circle(10, 13.5, 0.9, true)
            circle(14, 13.5, 0.9, true)
            break
        case "network":
            circle(12, 5, 2.5, false)
            circle(5, 19, 2.5, false)
            circle(19, 19, 2.5, false)
            line([11, 7.3, 6.2, 16.8], false)
            line([13, 7.3, 17.8, 16.8], false)
            line([7.5, 19, 16.5, 19], false)
            break
        case "instrument":
            box(3, 5, 18, 14)
            box(5.5, 7.5, 9, 7)
            wave(6, 14, 11, 2, 2)
            circle(18, 9, 1.3, false)
            circle(18, 14, 1.3, false)
            break
        case "audio":
            line([9, 4, 15, 4, 15, 12, 9, 12], true)
            c.beginPath()
            c.moveTo(6, 10)
            c.quadraticCurveTo(6, 17, 12, 17)
            c.quadraticCurveTo(18, 17, 18, 10)
            c.stroke()
            line([12, 17, 12, 21], false)
            line([8, 21, 16, 21], false)
            break
        case "sim":
            box(3, 4, 18, 16)
            wave(5, 19, 12, 4, 2)
            break
        // ---- Views ----
        case "fit":
            line([4, 9, 4, 4, 9, 4], false)
            line([15, 4, 20, 4, 20, 9], false)
            line([20, 15, 20, 20, 15, 20], false)
            line([9, 20, 4, 20, 4, 15], false)
            box(9, 9, 6, 6)
            break
        case "zoomin":
        case "zoomout":
            circle(10, 10, 6.5, false)
            line([15, 15, 21, 21], false)
            line([7, 10, 13, 10], false)
            if (icon.name === "zoomin")
                line([10, 7, 10, 13], false)
            break
        case "stack":
            box(3, 3, 18, 7.5)
            box(3, 13.5, 18, 7.5)
            wave(5, 19, 6.75, 2, 2)
            wave(5, 19, 17.25, 2, 3)
            break
        case "overlay":
            box(3, 3, 18, 18)
            wave(5, 19, 12, 5, 2)
            wave(5, 19, 12, 3, 4)
            break
        case "window":
            line([3, 12, 21, 12], false)
            line([6, 8, 3, 12, 6, 16], false)
            line([18, 8, 21, 12, 18, 16], false)
            line([3, 4, 3, 20], false)
            line([21, 4, 21, 20], false)
            break
        // ---- Analysis ----
        case "spectrum":
            line([3, 21, 21, 21], false)
            line([6, 21, 6, 9], false)
            line([10, 21, 10, 16], false)
            line([14, 21, 14, 4], false)
            line([18, 21, 18, 17], false)
            break
        case "density":
            line([3, 21, 21, 21], false)
            line([3, 7, 5, 9, 7, 8, 9, 12, 11, 11, 13, 14, 15, 13, 17, 16, 19, 15, 21, 17], false)
            break
        case "decibel":
            c.font = "bold 11px sans-serif"
            c.fillText("dB", 3, 16)
            break
        case "linear":
            line([3, 21, 21, 21], false)
            line([3, 21, 3, 3], false)
            line([5, 19, 20, 5], false)
            break
        case "logscale":
            line([3, 16, 21, 16], false)
            for (const x of [4, 10.5, 14.3, 17, 19.1, 20.8])
                line([x, 13, x, 19], false)
            c.font = "8px sans-serif"
            c.fillText("log", 4, 9)
            break
        case "harmonics":
            line([3, 21, 21, 21], false)
            line([6, 21, 6, 8], false)
            line([12, 21, 12, 13], false)
            line([18, 21, 18, 17], false)
            line([10, 7, 12, 10, 14, 7], true)
            line([16, 11, 18, 14, 20, 11], true)
            break
        // ---- Measuring ----
        case "cursors":
            for (const x of [8, 16]) {
                for (let y = 3; y < 21; y += 4)
                    line([x, y, x, y + 2], false)
            }
            line([8, 12, 16, 12], false)
            line([10, 10, 8, 12, 10, 14], false)
            line([14, 10, 16, 12, 14, 14], false)
            break
        case "peak":
            line([3, 21, 21, 21], false)
            line([4, 20, 9, 18, 11, 9, 13, 18, 20, 20], false)
            line([11, 2, 14, 5, 11, 8, 8, 5], true)
            break
        case "nextpeak":
            line([3, 21, 21, 21], false)
            line([3, 20, 6, 10, 9, 20, 13, 15, 16, 20], false)
            line([15, 5, 21, 5], false)
            line([18.5, 2.5, 21, 5, 18.5, 7.5], false)
            break
        case "back":
            line([9, 5, 4, 10, 9, 15], false)
            c.beginPath()
            c.moveTo(4, 10)
            c.lineTo(14, 10)
            c.arc(14, 15, 5, -Math.PI / 2, Math.PI / 2, false)
            c.lineTo(9, 20)
            c.stroke()
            break
        case "copy":
            box(8, 8, 12, 13)
            line([5, 16, 4, 16, 4, 3, 15, 3, 15, 5], false)
            break
        case "image":
            box(3, 4, 18, 16)
            line([3, 17, 9, 11, 13, 15, 16, 12, 21, 17], false)
            circle(15.5, 8.5, 1.6, false)
            break
        case "check":
            line([4, 12.5, 9.5, 18, 20, 6], false)
            break
        case "integral":
            c.beginPath()
            c.moveTo(12.5, 4.5)
            c.bezierCurveTo(11.5, 2, 8, 2.5, 8, 7)
            c.lineTo(8, 17)
            c.bezierCurveTo(8, 21.5, 4.5, 22, 3.5, 19.5)
            c.stroke()
            line([13, 20, 22, 20], false)
            line([13, 20, 13, 13, 15.5, 10, 18.5, 12, 22, 7, 22, 20], false)
            break
        case "addcurve":
            wave(2, 15, 13, 4, 2)
            line([18.5, 4, 18.5, 12], false)
            line([14.5, 8, 22.5, 8], false)
            line([2, 21, 22, 21], false)
            break
        case "keep":
            wave(3, 21, 6, 2.5, 2)
            line([12, 10, 12, 17], false)
            line([9, 14.5, 12, 17.5, 15, 14.5], false)
            line([4, 18, 4, 21, 20, 21, 20, 18], false)
            break
        case "zoneview":
            line([5, 5, 2.5, 5, 2.5, 19, 5, 19], false)
            line([19, 5, 21.5, 5, 21.5, 19, 19, 19], false)
            wave(6, 18, 12, 4, 3)
            break
        case "zonecursors":
            for (const x of [6, 18]) {
                for (let y = 3; y < 21; y += 4)
                    line([x, y, x, y + 2], false)
            }
            wave(6, 18, 12, 4, 2)
            break
        case "zoneall":
            wave(4, 20, 10, 4, 3)
            line([3, 19, 21, 19], false)
            line([6, 16.5, 3, 19, 6, 21.5], false)
            line([18, 16.5, 21, 19, 18, 21.5], false)
            break
        case "caret":
            c.beginPath()
            c.moveTo(4, 8)
            c.lineTo(20, 8)
            c.lineTo(12, 18)
            c.closePath()
            c.fill()
            break
        }
    }
}

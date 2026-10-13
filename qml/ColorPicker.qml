import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Shapes

// Hue/saturation wheel + brightness slider + hex field.
Popup {
    id: root
    property color color: "#8e9bff"
    signal accepted(color picked)

    // HSV state (kept separately so hue survives at zero saturation).
    property real hue: 0.65
    property real sat: 0.45
    property real val: 1.0
    readonly property color current: Qt.hsva(hue, sat, val, 1)

    function load(c) {
        hue = c.hsvHue >= 0 ? c.hsvHue : hue
        sat = c.hsvSaturation
        val = c.hsvValue
        hex.text = c.toString()
    }
    onOpened: load(color)

    width: 280
    padding: 16
    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    background: Rectangle {
        radius: 16
        color: "#1b1c21"
        border.color: Qt.rgba(1, 1, 1, 0.08)
    }

    contentItem: Column {
        spacing: 14

        // ---- wheel ----
        Item {
            id: wheel
            width: 248
            height: 248
            readonly property real r: width / 2

            Shape {
                anchors.fill: parent
                preferredRendererType: Shape.CurveRenderer
                // Hue around the circle.
                ShapePath {
                    strokeWidth: -1
                    fillGradient: ConicalGradient {
                        centerX: wheel.r; centerY: wheel.r; angle: 0
                        GradientStop { position: 0.000; color: Qt.hsva(0.000, 1, 1, 1) }
                        // Conical gradients run counter-clockwise, like the hue angle below.
                        GradientStop { position: 0.167; color: Qt.hsva(0.167, 1, 1, 1) }
                        GradientStop { position: 0.333; color: Qt.hsva(0.333, 1, 1, 1) }
                        GradientStop { position: 0.500; color: Qt.hsva(0.500, 1, 1, 1) }
                        GradientStop { position: 0.667; color: Qt.hsva(0.667, 1, 1, 1) }
                        GradientStop { position: 0.833; color: Qt.hsva(0.833, 1, 1, 1) }
                        GradientStop { position: 1.000; color: Qt.hsva(0.999, 1, 1, 1) }
                    }
                    PathAngleArc { centerX: wheel.r; centerY: wheel.r; radiusX: wheel.r; radiusY: wheel.r; startAngle: 0; sweepAngle: 360 }
                }
                // Saturation: white in the middle fading out.
                ShapePath {
                    strokeWidth: -1
                    fillGradient: RadialGradient {
                        centerX: wheel.r; centerY: wheel.r; centerRadius: wheel.r
                        focalX: wheel.r; focalY: wheel.r
                        GradientStop { position: 0; color: "white" }
                        GradientStop { position: 1; color: "transparent" }
                    }
                    PathAngleArc { centerX: wheel.r; centerY: wheel.r; radiusX: wheel.r; radiusY: wheel.r; startAngle: 0; sweepAngle: 360 }
                }
            }
            // Brightness darkens the whole wheel.
            Rectangle {
                anchors.fill: parent
                radius: width / 2
                color: "black"
                opacity: 1 - root.val
            }
            // Handle
            Rectangle {
                readonly property real a: root.hue * 2 * Math.PI
                x: wheel.r + Math.cos(a) * root.sat * wheel.r - width / 2
                y: wheel.r - Math.sin(a) * root.sat * wheel.r - height / 2
                width: 18; height: 18; radius: 9
                color: root.current
                border.color: "white"
                border.width: 2
            }
            MouseArea {
                anchors.fill: parent
                function pick(mx, my) {
                    const dx = mx - wheel.r, dy = wheel.r - my
                    let a = Math.atan2(dy, dx)
                    if (a < 0) a += 2 * Math.PI
                    root.hue = a / (2 * Math.PI)
                    root.sat = Math.min(1, Math.sqrt(dx * dx + dy * dy) / wheel.r)
                    hex.text = root.current.toString()
                }
                onPressed: (m) => pick(m.x, m.y)
                onPositionChanged: (m) => { if (pressed) pick(m.x, m.y) }
            }
        }

        // ---- brightness ----
        Rectangle {
            width: 248
            height: 14
            radius: 7
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0; color: "black" }
                GradientStop { position: 1; color: Qt.hsva(root.hue, root.sat, 1, 1) }
            }
            Rectangle {
                x: root.val * (parent.width - width)
                anchors.verticalCenter: parent.verticalCenter
                width: 18; height: 18; radius: 9
                color: root.current
                border.color: "white"
                border.width: 2
            }
            MouseArea {
                anchors.fill: parent
                anchors.margins: -6
                function pick(mx) { root.val = Math.max(0, Math.min(1, (mx - 6) / (width - 12))); hex.text = root.current.toString() }
                onPressed: (m) => pick(m.x)
                onPositionChanged: (m) => { if (pressed) pick(m.x) }
            }
        }

        Row {
            spacing: 10
            Rectangle {
                width: 36; height: 36; radius: 10
                color: root.current
                border.color: Qt.rgba(1, 1, 1, 0.15)
            }
            TextField {
                id: hex
                width: 110
                font.family: "monospace"
                onEditingFinished: { const c = Qt.color(text); if (c.valid) root.load(c) }
            }
            Button {
                width: 82
                text: qsTr("Use")
                highlighted: true
                onClicked: { root.accepted(root.current); root.close() }
            }
        }
    }
}

import QtQuick
import QtQuick.Controls.Basic

// Round toolbar button with an optional label and a delayed tooltip.
AbstractButton {
    id: root
    property string iconName
    property string tip
    property bool primary: false
    property bool busy: false
    property color accent: "#8e9bff"
    property color fg: primary ? "#14151a" : "#eceef4"

    implicitHeight: 36
    implicitWidth: text.length > 0 ? row.implicitWidth + 28 : 36
    hoverEnabled: true
    focusPolicy: Qt.NoFocus

    background: Rectangle {
        radius: height / 2
        color: root.primary ? (root.down ? Qt.darker(root.accent, 1.15) : root.hovered ? Qt.lighter(root.accent, 1.08) : root.accent)
                            : (root.down ? Qt.rgba(1, 1, 1, 0.16) : root.hovered ? Qt.rgba(1, 1, 1, 0.09) : "transparent")
        Behavior on color { ColorAnimation { duration: 120 } }
    }

    contentItem: Item {
        Row {
            id: row
            anchors.centerIn: parent
            spacing: 7
            Icon {
                anchors.verticalCenter: parent.verticalCenter
                name: root.busy ? "loader-circle" : root.iconName
                color: root.fg
                size: 18
                opacity: root.enabled ? 1 : 0.4
                RotationAnimator on rotation {
                    running: root.busy
                    from: 0; to: 360; duration: 900
                    loops: Animation.Infinite
                }
            }
            Text {
                visible: root.text.length > 0
                anchors.verticalCenter: parent.verticalCenter
                text: root.text
                color: root.fg
                font.pixelSize: 14
                font.weight: Font.Medium
            }
        }
    }

    ToolTip.visible: hovered && tip.length > 0 && text.length === 0
    ToolTip.delay: 450
    ToolTip.text: tip
}

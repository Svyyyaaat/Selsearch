import QtQuick

// Short usage hint at the top of the focused screen; fades once the user acts.
Rectangle {
    id: root
    property bool shown: true
    property bool ocrBusy: false
    property bool textMode: false

    width: row.implicitWidth + 32
    height: 38
    radius: 19
    color: Qt.rgba(0.09, 0.09, 0.11, 0.86)
    border.color: Qt.rgba(1, 1, 1, 0.08)
    opacity: shown ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: 220 } }

    Row {
        id: row
        anchors.centerIn: parent
        spacing: 10
        Icon {
            anchors.verticalCenter: parent.verticalCenter
            name: root.ocrBusy ? "loader-circle" : (root.textMode ? "text-cursor-input" : "scan")
            size: 16
            color: "#eceef4"
            RotationAnimator on rotation { running: root.ocrBusy; from: 0; to: 360; duration: 900; loops: Animation.Infinite }
            onVisibleChanged: if (!root.ocrBusy) rotation = 0
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            color: "#eceef4"
            font.pixelSize: 13
            text: root.textMode ? qsTr("Drag across text to select it  ·  Ctrl+A selects all  ·  Esc to close")
                                : qsTr("Drag or circle to select  ·  Click a window  ·  Drag over text to select it  ·  Esc to close")
        }
    }
}

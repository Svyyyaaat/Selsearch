import QtQuick

// Small transient notice at the bottom of the screen.
Rectangle {
    id: root
    property alias text: label.text
    function show(message) {
        label.text = message
        hideTimer.restart()
        state = "shown"
    }
    signal hidden()

    width: label.implicitWidth + 36
    height: 40
    radius: 20
    color: Qt.rgba(0.09, 0.09, 0.11, 0.94)
    border.color: Qt.rgba(1, 1, 1, 0.08)
    opacity: 0
    scale: 0.96
    visible: opacity > 0

    Text {
        id: label
        anchors.centerIn: parent
        color: "#eceef4"
        font.pixelSize: 14
    }

    Timer {
        id: hideTimer
        interval: 2600
        onTriggered: { root.state = ""; root.hidden() }
    }

    states: State {
        name: "shown"
        PropertyChanges { root.opacity: 1; root.scale: 1 }
    }
    transitions: Transition {
        NumberAnimation { properties: "opacity,scale"; duration: 160; easing.type: Easing.OutCubic }
    }
}

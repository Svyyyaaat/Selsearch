import QtQuick
import QtQuick.Controls.Basic

// Target-language picker: recent languages first, then all.
Popup {
    id: root
    property var languages: []      // [{code, name}]
    property var recent: []         // [code]
    property string current: "en"
    signal picked(string code)

    width: 220
    height: Math.min(list.contentHeight + 12, 320)
    padding: 6
    modal: false
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    enter: Transition { NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 120 } }
    exit: Transition { NumberAnimation { property: "opacity"; from: 1; to: 0; duration: 100 } }

    background: Rectangle {
        radius: 14
        color: Qt.rgba(0.09, 0.09, 0.11, 0.97)
        border.color: Qt.rgba(1, 1, 1, 0.08)
    }

    function nameOf(code) {
        for (let l of languages) if (l.code === code) return l.name
        return code
    }

    contentItem: ListView {
        id: list
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        model: {
            let rows = []
            if (root.recent.length > 0) {
                rows.push({ header: qsTr("Recent") })
                for (let c of root.recent) rows.push({ code: c, name: root.nameOf(c) })
                rows.push({ header: qsTr("All languages") })
            }
            for (let l of root.languages) rows.push(l)
            return rows
        }
        delegate: Item {
            required property var modelData
            width: ListView.view.width
            height: modelData.header ? 28 : 34
            Text {
                visible: !!modelData.header
                text: modelData.header || ""
                color: Qt.rgba(1, 1, 1, 0.45)
                font.pixelSize: 11
                font.capitalization: Font.AllUppercase
                font.letterSpacing: 0.6
                anchors.left: parent.left; anchors.leftMargin: 10
                anchors.bottom: parent.bottom; anchors.bottomMargin: 4
            }
            Rectangle {
                visible: !modelData.header
                anchors.fill: parent
                radius: 8
                color: hover.hovered ? Qt.rgba(1, 1, 1, 0.08) : "transparent"
                HoverHandler { id: hover }
                TapHandler { onTapped: { root.picked(modelData.code); root.close() } }
                Text {
                    anchors.left: parent.left; anchors.leftMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    text: modelData.name || ""
                    color: "#eceef4"
                    font.pixelSize: 14
                }
                Icon {
                    visible: modelData.code === root.current
                    anchors.right: parent.right; anchors.rightMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    name: "check"; size: 16; color: "#eceef4"
                }
            }
        }
    }
}

import QtQuick
import QtQuick.Effects

// The pill of actions shown next to a finished selection.
Item {
    id: root
    property bool textMode: false       // a text selection (vs. an image region)
    property bool hasText: false        // region contains recognised words
    property bool ocrBusy: false
    property bool translating: false
    property bool translated: false
    property string targetLang: "en"
    property color accent: "#8e9bff"
    property bool aiEnabled: true

    signal search()
    signal copyImage()
    signal copyText()
    signal translate()
    signal pickLanguage(Item anchor)
    signal ask()
    signal save()

    implicitWidth: pill.width
    implicitHeight: pill.height

    Rectangle {
        id: pill
        width: row.implicitWidth + 8
        height: 44
        radius: 22
        color: Qt.rgba(0.09, 0.09, 0.11, 0.94)
        border.color: Qt.rgba(1, 1, 1, 0.08)
        layer.enabled: true
        layer.effect: MultiEffect {
            shadowEnabled: true
            shadowBlur: 0.7
            shadowVerticalOffset: 6
            shadowColor: Qt.rgba(0, 0, 0, 0.5)
        }

        Row {
            id: row
            anchors.centerIn: parent
            spacing: 2

            IconButton {
                text: qsTr("Search")
                iconName: "search"
                primary: true
                accent: root.accent
                onClicked: root.search()
            }
            Item { width: 4; height: 1 }
            IconButton {
                visible: root.textMode
                iconName: "copy"
                tip: qsTr("Copy text  (Ctrl+C)")
                onClicked: root.copyText()
            }
            IconButton {
                visible: !root.textMode
                iconName: "copy"
                tip: qsTr("Copy image  (Ctrl+C)")
                onClicked: root.copyImage()
            }
            IconButton {
                visible: !root.textMode && (root.hasText || root.ocrBusy)
                iconName: "scan-text"
                busy: root.ocrBusy
                tip: qsTr("Copy text  (Ctrl+Shift+C)")
                onClicked: root.copyText()
            }
            // Translate: main part toggles, the chevron picks the language.
            Rectangle {
                visible: root.hasText || root.textMode || root.ocrBusy
                height: 36
                width: trRow.width
                radius: 18
                color: root.translated ? Qt.rgba(1, 1, 1, 0.14) : "transparent"
                Row {
                    id: trRow
                    IconButton {
                        iconName: "languages"
                        busy: root.translating
                        tip: root.translated ? qsTr("Show original  (T)") : qsTr("Translate  (T)")
                        onClicked: root.translate()
                    }
                    AbstractLangButton {
                        id: langButton
                        lang: root.targetLang
                        onClicked: root.pickLanguage(langButton)
                    }
                }
            }
            IconButton {
                visible: root.aiEnabled
                iconName: "sparkles"
                tip: qsTr("Ask AI about this  (A)")
                onClicked: root.ask()
            }
            IconButton {
                visible: !root.textMode
                iconName: "download"
                tip: qsTr("Save image  (Ctrl+S)")
                onClicked: root.save()
            }
        }
    }

    component AbstractLangButton: MouseArea {
        property string lang
        width: langText.implicitWidth + 26
        height: 36
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        Rectangle {
            anchors.fill: parent
            radius: 18
            color: parent.pressed ? Qt.rgba(1, 1, 1, 0.16) : parent.containsMouse ? Qt.rgba(1, 1, 1, 0.09) : "transparent"
        }
        Row {
            anchors.centerIn: parent
            spacing: 2
            Text {
                id: langText
                anchors.verticalCenter: parent.verticalCenter
                text: parent.parent.lang.toUpperCase()
                color: "#eceef4"
                font.pixelSize: 12
                font.weight: Font.DemiBold
                font.letterSpacing: 0.5
            }
            Icon { anchors.verticalCenter: parent.verticalCenter; name: "chevron-down"; size: 14; color: "#eceef4" }
        }
    }
}

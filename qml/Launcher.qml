import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Effects

// The launcher: a floating card on a transparent full-screen layer.
// Search mode lists apps, files, calculator answers and web search; AI mode
// is a small chat about the query or an image from the lens.
Window {
    id: win
    required property var launcher
    color: "transparent"
    flags: Qt.FramelessWindowHint
    title: "Selsearch"

    readonly property bool aiMode: launcher.mode === "ai"
    readonly property color accent: launcher.accent
    readonly property color fg: "#eceef4"
    readonly property color fgDim: "#9a9eab"
    property int selected: 0

    // Click outside the card closes.
    MouseArea {
        anchors.fill: parent
        onPressed: win.launcher.hide()
    }

    Connections {
        target: win.launcher
        function onFocusInput() { input.forceActiveFocus(); input.selectAll() }
        function onResultsChanged() { win.selected = 0 }
        function onShownChanged() { if (win.launcher.shown) openAnim.restart() }
    }

    Item {
        id: card
        width: Math.min(720, win.width - 32)
        height: column.implicitHeight
        x: (win.width - width) / 2
        y: Math.round(win.height * 0.16)
        opacity: win.launcher.shown ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: win.launcher.shown ? 140 : 100 } }
        transform: Translate { id: shift; y: 0 }

        SequentialAnimation {
            id: openAnim
            ParallelAnimation {
                NumberAnimation { target: shift; property: "y"; from: -10; to: 0; duration: 220; easing.type: Easing.OutCubic }
                NumberAnimation { target: card; property: "scale"; from: 0.985; to: 1; duration: 220; easing.type: Easing.OutCubic }
                NumberAnimation { target: sheen; property: "progress"; from: 0; to: 1; duration: 700; easing.type: Easing.InOutSine }
            }
        }

        // Swallow clicks on the card itself.
        MouseArea { anchors.fill: parent }

        Rectangle {
            id: bg
            anchors.fill: parent
            radius: 24
            color: "#16171b"
            border.color: Qt.rgba(1, 1, 1, 0.08)
            layer.enabled: true
            layer.effect: MultiEffect {
                shadowEnabled: true
                shadowBlur: 1.0
                shadowVerticalOffset: 10
                shadowColor: Qt.rgba(0, 0, 0, 0.55)
            }
        }

        // A thin light sweep across the top edge when the card opens.
        Item {
            id: sheen
            property real progress: 1
            anchors { left: parent.left; right: parent.right; top: parent.top; leftMargin: 24; rightMargin: 24 }
            height: 1
            clip: true
            opacity: progress < 1 ? 1 : 0
            Behavior on opacity { NumberAnimation { duration: 200 } }
            Rectangle {
                width: parent.width * 0.35
                height: 1
                x: -width + (parent.width + width) * sheen.progress
                gradient: Gradient {
                    orientation: Gradient.Horizontal
                    GradientStop { position: 0; color: "transparent" }
                    GradientStop { position: 0.5; color: Qt.rgba(1, 1, 1, 0.85) }
                    GradientStop { position: 1; color: "transparent" }
                }
            }
        }

        Column {
            id: column
            width: parent.width

            // ---- input row -------------------------------------------------
            Item {
                width: parent.width
                height: 64

                Icon {
                    id: modeIcon
                    anchors { left: parent.left; leftMargin: 22; verticalCenter: parent.verticalCenter }
                    name: win.aiMode ? "sparkles" : "search"
                    size: 20
                    color: win.aiMode ? win.accent : win.fgDim
                }

                // Attached lens image
                Rectangle {
                    id: chip
                    visible: win.launcher.attachment !== ""
                    anchors { left: modeIcon.right; leftMargin: 12; verticalCenter: parent.verticalCenter }
                    width: visible ? 44 : 0
                    height: 36
                    radius: 8
                    color: Qt.rgba(1, 1, 1, 0.06)
                    border.color: Qt.rgba(1, 1, 1, 0.12)
                    clip: true
                    Image {
                        anchors.fill: parent
                        anchors.margins: 1
                        source: win.launcher.attachment
                        fillMode: Image.PreserveAspectCrop
                        sourceSize: Qt.size(88, 72)
                    }
                    Rectangle {
                        anchors { right: parent.right; top: parent.top; margins: 2 }
                        width: 14; height: 14; radius: 7
                        color: Qt.rgba(0, 0, 0, 0.7)
                        Icon { anchors.centerIn: parent; name: "x"; size: 10; color: "white" }
                        MouseArea { anchors.fill: parent; onClicked: win.launcher.removeAttachment() }
                    }
                }

                TextField {
                    id: input
                    anchors { left: chip.visible ? chip.right : modeIcon.right; leftMargin: 12; right: buttons.left; rightMargin: 8; verticalCenter: parent.verticalCenter }
                    text: win.launcher.query
                    placeholderText: win.aiMode ? (win.launcher.attachment !== "" ? qsTr("Ask about this selection…") : qsTr("Ask anything…"))
                                                : qsTr("Search apps, files and the web")
                    placeholderTextColor: "#6f7383"
                    color: win.fg
                    font.pixelSize: 19
                    background: Item {}
                    selectByMouse: true
                    selectionColor: Qt.rgba(win.accent.r, win.accent.g, win.accent.b, 0.45)
                    onTextEdited: win.launcher.setQuery(text)
                    Keys.onPressed: (event) => win.handleKey(event)
                }

                Row {
                    id: buttons
                    anchors { right: parent.right; rightMargin: 14; verticalCenter: parent.verticalCenter }
                    spacing: 2
                    IconButton {
                        iconName: "scan"
                        tip: qsTr("Search your screen")
                        onClicked: win.launcher.startLens()
                    }
                    IconButton {
                        visible: win.launcher.aiEnabled
                        iconName: "sparkles"
                        tip: win.aiMode ? qsTr("Back to search  (Tab)") : qsTr("Ask AI  (Tab)")
                        fg: win.aiMode ? win.accent : "#eceef4"
                        onClicked: win.launcher.mode = win.aiMode ? "search" : "ai"
                    }
                    IconButton {
                        iconName: "settings"
                        tip: qsTr("Settings")
                        onClicked: win.launcher.openSettings()
                    }
                }
            }

            Rectangle {
                width: parent.width
                height: 1
                color: Qt.rgba(1, 1, 1, 0.06)
                visible: list.visible || chat.visible
            }

            // ---- search results ---------------------------------------------
            ListView {
                id: list
                visible: !win.aiMode && count > 0
                width: parent.width
                height: visible ? Math.min(contentHeight + 16, Math.max(320, win.height * 0.6)) : 0
                topMargin: 8
                bottomMargin: 8
                clip: true
                model: win.launcher.results
                currentIndex: win.selected
                boundsBehavior: Flickable.StopAtBounds
                highlightMoveDuration: 0
                section.property: "group"
                section.delegate: Text {
                    required property string section
                    width: ListView.view.width
                    leftPadding: 24
                    topPadding: 8
                    bottomPadding: 4
                    text: section
                    color: "#7c8091"
                    font.pixelSize: 11
                    font.weight: Font.DemiBold
                    font.letterSpacing: 0.6
                    font.capitalization: Font.AllUppercase
                }
                delegate: Item {
                    id: row
                    required property var modelData
                    required property int index
                    readonly property bool current: index === win.selected
                    width: ListView.view.width
                    height: 50

                    Rectangle {
                        anchors { fill: parent; leftMargin: 10; rightMargin: 10 }
                        radius: 12
                        color: row.current ? Qt.rgba(1, 1, 1, 0.075) : rowHover.hovered ? Qt.rgba(1, 1, 1, 0.04) : "transparent"
                    }
                    HoverHandler { id: rowHover }
                    TapHandler { onTapped: win.launcher.activate(row.index) }

                    Image {
                        id: rowIcon
                        anchors { left: parent.left; leftMargin: 24; verticalCenter: parent.verticalCenter }
                        width: 28; height: 28
                        sourceSize: Qt.size(56, 56)
                        source: row.modelData.icon || ""
                        asynchronous: true
                        smooth: true
                    }
                    Column {
                        anchors { left: rowIcon.right; leftMargin: 14; right: hint.left; rightMargin: 12; verticalCenter: parent.verticalCenter }
                        spacing: 1
                        Text {
                            width: parent.width
                            text: row.modelData.title
                            color: win.fg
                            font.pixelSize: 15
                            elide: Text.ElideRight
                        }
                        Text {
                            width: parent.width
                            visible: text.length > 0
                            text: row.modelData.subtitle || ""
                            color: win.fgDim
                            font.pixelSize: 12
                            elide: Text.ElideMiddle
                        }
                    }
                    Text {
                        id: hint
                        anchors { right: parent.right; rightMargin: 24; verticalCenter: parent.verticalCenter }
                        visible: row.current
                        text: "↵"
                        color: win.fgDim
                        font.pixelSize: 14
                    }
                }
            }

            // Web suggestions (only when enabled in settings).
            Flow {
                visible: !win.aiMode && win.launcher.suggestions.length > 0
                width: parent.width - 40
                x: 20
                spacing: 6
                bottomPadding: 10
                Repeater {
                    model: win.launcher.suggestions
                    delegate: Rectangle {
                        required property string modelData
                        height: 28
                        width: sText.implicitWidth + 24
                        radius: 14
                        color: sHover.hovered ? Qt.rgba(1, 1, 1, 0.1) : Qt.rgba(1, 1, 1, 0.05)
                        HoverHandler { id: sHover }
                        TapHandler { onTapped: win.launcher.searchWeb(parent.modelData) }
                        Text { id: sText; anchors.centerIn: parent; text: parent.modelData; color: win.fg; font.pixelSize: 13 }
                    }
                }
            }

            // ---- AI chat -------------------------------------------------------
            ListView {
                id: chat
                visible: win.aiMode && count > 0
                width: parent.width
                height: visible ? Math.min(contentHeight + 24, win.height * 0.55) : 0
                topMargin: 12
                bottomMargin: 12
                spacing: 10
                clip: true
                model: win.launcher.messages
                onCountChanged: Qt.callLater(positionViewAtEnd)
                onContentHeightChanged: if (win.launcher.aiBusy) Qt.callLater(positionViewAtEnd)
                delegate: Item {
                    id: msg
                    required property var modelData
                    readonly property bool mine: modelData.role === "user"
                    readonly property bool error: modelData.role === "error"
                    width: ListView.view.width
                    height: bubble.height

                    // Natural width of the user's text, to size their bubble.
                    Text {
                        id: measure
                        visible: false
                        text: msg.modelData.text
                        font.pixelSize: 14
                    }
                    Rectangle {
                        id: bubble
                        x: msg.mine ? msg.width - width - 20 : 20
                        width: msg.mine ? Math.min(msg.width * 0.75, Math.max(measure.implicitWidth, msg.modelData.image ? 220 : 0) + 30) : msg.width - 40
                        height: bodyCol.implicitHeight + (msg.mine ? 20 : 8)
                        radius: 16
                        color: msg.mine ? Qt.rgba(1, 1, 1, 0.07) : msg.error ? Qt.rgba(1, 0.35, 0.35, 0.10) : "transparent"
                        Column {
                            id: bodyCol
                            x: msg.mine ? 14 : 4
                            y: msg.mine ? 10 : 4
                            width: bubble.width - (msg.mine ? 28 : 8)
                            spacing: 8
                            Image {
                                visible: !!msg.modelData.image
                                source: msg.modelData.image || ""
                                width: Math.min(220, parent.width)
                                height: visible ? Math.min(140, width * (implicitHeight / Math.max(1, implicitWidth))) : 0
                                fillMode: Image.PreserveAspectFit
                                horizontalAlignment: Image.AlignLeft
                            }
                            TextEdit {
                                width: parent.width
                                readOnly: true
                                selectByMouse: true
                                wrapMode: Text.Wrap
                                textFormat: msg.mine ? TextEdit.PlainText : TextEdit.MarkdownText
                                text: msg.modelData.text + (msg.modelData.streaming ? " ▍" : "")
                                color: msg.error ? "#ffb4b4" : win.fg
                                selectionColor: Qt.rgba(win.accent.r, win.accent.g, win.accent.b, 0.45)
                                font.pixelSize: 14
                                onLinkActivated: (link) => Qt.openUrlExternally(link)
                            }
                            Row {
                                visible: !msg.mine && !msg.error && !msg.modelData.streaming && msg.modelData.text.length > 0
                                spacing: 2
                                IconButton { iconName: "copy"; tip: qsTr("Copy answer"); implicitHeight: 28; implicitWidth: 28; onClicked: win.launcher.copy(msg.modelData.text) }
                            }
                        }
                    }
                }
            }

            // Empty AI state / history
            Column {
                visible: win.aiMode && chat.count === 0
                width: parent.width
                topPadding: 14
                bottomPadding: 12
                spacing: 4
                Text {
                    x: 24
                    visible: win.launcher.threads.length > 0
                    text: qsTr("Recent")
                    color: "#7c8091"
                    font.pixelSize: 11
                    font.weight: Font.DemiBold
                    font.letterSpacing: 0.6
                    font.capitalization: Font.AllUppercase
                }
                Repeater {
                    model: win.launcher.threads.slice(0, 5)
                    delegate: Item {
                        required property var modelData
                        width: parent.width
                        height: 40
                        Rectangle {
                            anchors { fill: parent; leftMargin: 10; rightMargin: 10 }
                            radius: 12
                            color: tHover.hovered ? Qt.rgba(1, 1, 1, 0.05) : "transparent"
                        }
                        HoverHandler { id: tHover }
                        TapHandler { onTapped: win.launcher.openThread(parent.modelData.id) }
                        Icon { id: hIcon; x: 24; anchors.verticalCenter: parent.verticalCenter; name: "history"; size: 16; color: win.fgDim }
                        Text {
                            anchors { left: hIcon.right; leftMargin: 12; right: del.left; rightMargin: 8; verticalCenter: parent.verticalCenter }
                            text: parent.modelData.title
                            color: win.fg
                            font.pixelSize: 14
                            elide: Text.ElideRight
                        }
                        IconButton {
                            id: del
                            visible: tHover.hovered
                            anchors { right: parent.right; rightMargin: 18; verticalCenter: parent.verticalCenter }
                            implicitWidth: 28; implicitHeight: 28
                            iconName: "trash-2"
                            tip: qsTr("Delete")
                            onClicked: win.launcher.deleteThread(parent.modelData.id)
                        }
                    }
                }
                Text {
                    visible: win.launcher.threads.length === 0
                    x: 24
                    width: parent.width - 48
                    wrapMode: Text.Wrap
                    text: qsTr("Ask a question, or use the lens (the scan button) to ask about something on your screen.")
                    color: win.fgDim
                    font.pixelSize: 13
                }
            }

            // ---- footer --------------------------------------------------------
            Rectangle { width: parent.width; height: 1; color: Qt.rgba(1, 1, 1, 0.06) }
            Item {
                width: parent.width
                height: 34
                Text {
                    anchors { left: parent.left; leftMargin: 22; verticalCenter: parent.verticalCenter }
                    color: "#6f7383"
                    font.pixelSize: 11
                    text: win.aiMode ? qsTr("↵ send  ·  Esc close  ·  Tab search  ·  Ctrl+N new chat")
                                     : qsTr("↵ open  ·  Ctrl+↵ ask AI  ·  Tab AI mode  ·  Esc close")
                }
                // Where data goes: always visible, never surprising.
                Row {
                    anchors { right: parent.right; rightMargin: 20; verticalCenter: parent.verticalCenter }
                    spacing: 6
                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        width: 6; height: 6; radius: 3
                        color: win.aiMode && win.launcher.aiRemoteHost !== "" ? "#f2b766" : "#7fd1a8"
                    }
                    Text {
                        color: "#7c8091"
                        font.pixelSize: 11
                        text: win.aiMode
                              ? (win.launcher.aiRemoteHost !== "" ? qsTr("Sends to %1").arg(win.launcher.aiRemoteHost)
                                                                   : qsTr("Local · %1").arg(win.launcher.aiModel))
                              : qsTr("On this device · web via %1").arg(win.launcher.engineName)
                    }
                    IconButton {
                        visible: win.aiMode && win.launcher.aiBusy
                        implicitWidth: 24; implicitHeight: 24
                        iconName: "x"
                        tip: qsTr("Stop")
                        onClicked: win.launcher.stopAi()
                    }
                }
            }
        }
    }

    function handleKey(event) {
        const n = list.count
        if (event.key === Qt.Key_Escape) {
            if (win.launcher.aiBusy) win.launcher.stopAi()
            else win.launcher.hide()
        } else if (event.key === Qt.Key_Tab || event.key === Qt.Key_Backtab) {
            if (win.launcher.aiEnabled) win.launcher.mode = win.aiMode ? "search" : "ai"
        } else if (event.key === Qt.Key_Down && !win.aiMode) {
            win.selected = Math.min(n - 1, win.selected + 1)
            list.positionViewAtIndex(win.selected, ListView.Contain)
        } else if (event.key === Qt.Key_Up && !win.aiMode) {
            win.selected = Math.max(0, win.selected - 1)
            list.positionViewAtIndex(win.selected, ListView.Contain)
        } else if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter)) {
            if (win.aiMode || (event.modifiers & Qt.ControlModifier)) win.launcher.askAi(input.text)
            else if (n > 0) win.launcher.activate(win.selected)
            else win.launcher.searchWeb(input.text)
        } else if (event.key === Qt.Key_N && (event.modifiers & Qt.ControlModifier)) {
            win.launcher.newChat()
        } else if (event.key === Qt.Key_Backspace && input.text.length === 0 && win.launcher.attachment !== "") {
            win.launcher.removeAttachment()
        } else {
            return
        }
        event.accepted = true
    }
}

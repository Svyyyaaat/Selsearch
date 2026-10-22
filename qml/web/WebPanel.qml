import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Effects
import QtWebEngine

// The results panel: a rounded sheet under the top edge with a slim header.
Window {
    id: win
    width: Math.min(Screen.width - 48, 980)
    height: Math.min(Screen.height - 120, 860)
    color: "transparent"
    flags: Qt.FramelessWindowHint
    title: "Selsearch results"

    property string pageTitle: ""

    Connections {
        target: bridge
        function onOpenRequested(url, title) {
            win.pageTitle = title
            view.url = url
            closeAnim.stop()
            openAnim.restart()
        }
        function onHideRequested() { win.dismiss() }
    }

    // Close: animate out, then unmap.
    function dismiss() {
        if (!win.visible || closeAnim.running)
            return
        openAnim.stop()
        closeAnim.restart()
    }
    function finishDismiss() {
        win.hide()
        view.url = "about:blank"
        bridge.emitEvent("hidden")
    }

    // Open: drop in from slightly above with a soft overshoot.
    ParallelAnimation {
        id: openAnim
        NumberAnimation { target: sheet; property: "opacity"; from: 0; to: 1; duration: 180; easing.type: Easing.OutCubic }
        NumberAnimation { target: sheetShift; property: "y"; from: -24; to: 0; duration: 340; easing.type: Easing.OutBack; easing.overshoot: 1.2 }
        NumberAnimation { target: sheet; property: "scale"; from: 0.97; to: 1; duration: 340; easing.type: Easing.OutBack; easing.overshoot: 1.2 }
    }
    SequentialAnimation {
        id: closeAnim
        ParallelAnimation {
            NumberAnimation { target: sheet; property: "opacity"; to: 0; duration: 140; easing.type: Easing.InCubic }
            NumberAnimation { target: sheetShift; property: "y"; to: -14; duration: 160; easing.type: Easing.InCubic }
            NumberAnimation { target: sheet; property: "scale"; to: 0.98; duration: 160; easing.type: Easing.InCubic }
        }
        ScriptAction { script: win.finishDismiss() }
    }

    onActiveChanged: if (!active && visible && !menuOpen) dismiss()
    property bool menuOpen: false

    WebEngineProfile {
        id: profile
        // Off the record unless the user asked to keep logins (search.persist_cookies).
        offTheRecord: !bridge.persistent
        storageName: bridge.persistent ? "selsearch" : ""
        persistentStoragePath: bridge.persistent ? bridge.storagePath : ""
        httpCacheType: bridge.persistent ? WebEngineProfile.DiskHttpCache : WebEngineProfile.MemoryHttpCache
        persistentCookiesPolicy: bridge.persistent ? WebEngineProfile.AllowPersistentCookies : WebEngineProfile.NoPersistentCookies
        // The default user agent is Chrome's plus "QtWebEngine/x.y.z"; sites
        // such as Google treat that token as a bot hint (more CAPTCHAs, sign-in
        // refused). Present the plain Chrome string of the same engine.
        Component.onCompleted: httpUserAgent = httpUserAgent.replace(/ QtWebEngine\/[0-9.]+/, "")
    }

    Item {
        anchors.fill: parent
        anchors.margins: 16   // room for the shadow

        Rectangle {
            id: sheet
            anchors.fill: parent
            radius: 22
            color: "#16171b"
            border.color: Qt.rgba(1, 1, 1, 0.08)
            opacity: 0
            scale: 0.97
            transform: Translate { id: sheetShift }
            layer.enabled: true
            layer.effect: MultiEffect {
                shadowEnabled: true
                shadowBlur: 1.0
                shadowVerticalOffset: 8
                shadowColor: Qt.rgba(0, 0, 0, 0.55)
            }

            // Header
            Item {
                id: header
                anchors { left: parent.left; right: parent.right; top: parent.top }
                height: 48
                Row {
                    anchors { left: parent.left; leftMargin: 10; verticalCenter: parent.verticalCenter }
                    spacing: 4
                    HeaderButton { glyph: "←"; tip: qsTr("Back"); enabled: view.canGoBack; onClicked: view.goBack() }
                    HeaderButton { glyph: "↻"; tip: qsTr("Reload"); onClicked: view.reload() }
                }
                Text {
                    anchors.centerIn: parent
                    width: parent.width - 420
                    horizontalAlignment: Text.AlignHCenter
                    elide: Text.ElideMiddle
                    color: "#c9ccd6"
                    font.pixelSize: 13
                    text: view.title || win.pageTitle || view.url
                }
                Row {
                    anchors { right: parent.right; rightMargin: 10; verticalCenter: parent.verticalCenter }
                    spacing: 4
                    // Explicit, labelled: the main way out of the panel.
                    AbstractButton {
                        id: openExternal
                        height: 32
                        width: openRow.implicitWidth + 24
                        hoverEnabled: true
                        onClicked: { bridge.emitEvent("openExternal", view.url.toString()); win.dismiss() }
                        background: Rectangle {
                            radius: 16
                            color: openExternal.down ? Qt.rgba(1, 1, 1, 0.16) : openExternal.hovered ? Qt.rgba(1, 1, 1, 0.10) : Qt.rgba(1, 1, 1, 0.05)
                            border.color: Qt.rgba(1, 1, 1, 0.10)
                        }
                        contentItem: Item {
                            Row {
                                id: openRow
                                anchors.centerIn: parent
                                spacing: 6
                                Text { anchors.verticalCenter: parent.verticalCenter; text: "↗"; color: "#e6e8ef"; font.pixelSize: 14 }
                                Text { anchors.verticalCenter: parent.verticalCenter; text: qsTr("Open in browser"); color: "#e6e8ef"; font.pixelSize: 13 }
                            }
                        }
                        ToolTip.visible: hovered
                        ToolTip.delay: 600
                        ToolTip.text: qsTr("Ctrl+L")
                    }
                    HeaderButton { glyph: "✕"; tip: qsTr("Close  (Esc)"); onClicked: win.dismiss() }
                }
                Rectangle {
                    anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
                    height: 1
                    color: Qt.rgba(1, 1, 1, 0.06)
                }
                ProgressBar {
                    anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
                    height: 2
                    visible: view.loading
                    value: view.loadProgress / 100
                    background: Item {}
                    contentItem: Rectangle {
                        width: parent.width * view.loadProgress / 100
                        height: 2
                        color: "#8e9bff"
                    }
                }
            }

            // Rounded bottom corners for the page: mask the view's layer.
            Rectangle {
                id: viewMask
                anchors.fill: view
                radius: sheet.radius - 1
                visible: false
                layer.enabled: true
                // Square top corners: extend a plain rectangle over the top half.
                Rectangle { width: parent.width; height: parent.height / 2 }
            }

            WebEngineView {
                id: view
                anchors { left: parent.left; right: parent.right; top: header.bottom; bottom: parent.bottom; margins: 1 }
                layer.enabled: true
                layer.effect: MultiEffect {
                    maskEnabled: true
                    maskSource: viewMask
                    maskThresholdMin: 0.5
                    maskSpreadAtMin: 1.0
                }
                profile: profile
                settings.pluginsEnabled: false
                settings.localContentCanAccessRemoteUrls: true   // local image-search upload pages post to the engine
                onNewWindowRequested: (request) => { view.url = request.requestedUrl }
                onContextMenuRequested: (request) => { win.menuOpen = true }
            }
        }
    }

    Shortcut { sequence: "Escape"; onActivated: win.dismiss() }
    Shortcut { sequence: "Alt+Left"; onActivated: view.goBack() }
    Shortcut { sequence: "Ctrl+L"; onActivated: { bridge.emitEvent("openExternal", view.url.toString()); win.dismiss() } }

    component HeaderButton: AbstractButton {
        id: hb
        property string glyph
        property string tip
        width: 32; height: 32
        hoverEnabled: true
        background: Rectangle {
            radius: 16
            color: hb.down ? Qt.rgba(1, 1, 1, 0.14) : hb.hovered ? Qt.rgba(1, 1, 1, 0.08) : "transparent"
        }
        contentItem: Text {
            text: hb.glyph
            color: hb.enabled ? "#e6e8ef" : "#5d6070"
            font.pixelSize: 16
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
        ToolTip.visible: hovered && tip.length > 0
        ToolTip.delay: 500
        ToolTip.text: tip
    }
}

import QtQuick
import QtQuick.Controls.Basic

// Language picker with a search field: type to filter (by English or native
// name, or code; accents ignored), arrows to move, Enter to pick. Recent
// languages come first while the search is empty.
Popup {
    id: root
    property var languages: []      // [{code, name}]
    property var recent: []         // [code]
    property var pinned: []         // [{code, name}] always listed first, e.g. "System language"
    property string current: "en"
    property color accent: "#8e9bff"
    signal picked(string code)

    width: 260
    height: Math.min(search.height + list.contentHeight + 22, 380)
    padding: 6
    modal: false
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    enter: Transition { NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 120 } }
    exit: Transition { NumberAnimation { property: "opacity"; from: 1; to: 0; duration: 100 } }

    onAboutToShow: { search.text = ""; list.currentIndex = -1 }
    onOpened: search.forceActiveFocus()

    background: Rectangle {
        radius: 14
        color: Qt.rgba(0.09, 0.09, 0.11, 0.97)
        border.color: Qt.rgba(1, 1, 1, 0.08)
    }

    function nameOf(code) {
        for (let l of languages) if (l.code === code) return l.name
        return code
    }
    // Lower case without accents, so "francais" finds "Français".
    function fold(s) { return (s || "").normalize("NFD").replace(/[̀-ͯ]/g, "").toLowerCase() }

    readonly property var rows: {
        const q = fold(search.text.trim())
        let out = []
        if (q === "") {
            for (let p of pinned) out.push(p)
            if (recent.length > 0) {
                out.push({ header: qsTr("Recent") })
                for (let c of recent) out.push({ code: c, name: nameOf(c) })
                out.push({ header: qsTr("All languages") })
            }
            for (let l of languages) out.push(l)
            return out
        }
        // Names starting with the query first, then the rest that contain it.
        let starts = [], contains = []
        for (let l of pinned.concat(languages)) {
            const n = fold(l.name), c = fold(l.code)
            if (c === q || n.startsWith(q) || n.split(/[\s·()]+/).some(w => w.startsWith(q))) starts.push(l)
            else if (n.includes(q)) contains.push(l)
        }
        return starts.concat(contains)
    }
    function firstPickable(from, step) {
        for (let i = from; i >= 0 && i < rows.length; i += step)
            if (!rows[i].header) return i
        return -1
    }
    function pick(i) {
        if (i < 0 || i >= rows.length || rows[i].header) return
        root.picked(rows[i].code)
        root.close()
    }

    contentItem: Column {
        spacing: 6
        TextField {
            id: search
            width: parent.width
            height: 34
            placeholderText: qsTr("Search languages")
            placeholderTextColor: Qt.rgba(1, 1, 1, 0.4)
            color: "#eceef4"
            font.pixelSize: 14
            leftPadding: 32
            background: Rectangle {
                radius: 9
                color: Qt.rgba(1, 1, 1, 0.06)
                border.color: search.activeFocus ? Qt.rgba(root.accent.r, root.accent.g, root.accent.b, 0.7) : Qt.rgba(1, 1, 1, 0.08)
                Icon { x: 9; anchors.verticalCenter: parent.verticalCenter; name: "search"; size: 15; color: "#8b8f9c" }
            }
            onTextChanged: { list.currentIndex = text.trim() === "" ? -1 : root.firstPickable(0, 1); list.positionViewAtBeginning() }
            Keys.onDownPressed: { const i = root.firstPickable(list.currentIndex + 1, 1); if (i >= 0) list.currentIndex = i }
            Keys.onUpPressed: { const i = root.firstPickable(list.currentIndex - 1, -1); if (i >= 0) list.currentIndex = i }
            Keys.onReturnPressed: root.pick(list.currentIndex >= 0 ? list.currentIndex : root.firstPickable(0, 1))
            Keys.onEnterPressed: root.pick(list.currentIndex >= 0 ? list.currentIndex : root.firstPickable(0, 1))
        }
        ListView {
            id: list
            width: parent.width
            height: Math.min(contentHeight, 380 - search.height - 22)
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            currentIndex: -1
            highlightMoveDuration: 0
            model: root.rows
            ScrollBar.vertical: ScrollBar { policy: list.contentHeight > list.height ? ScrollBar.AsNeeded : ScrollBar.AlwaysOff }
            delegate: Item {
                id: row
                required property var modelData
                required property int index
                width: ListView.view.width
                height: modelData.header ? 28 : 34
                Text {
                    visible: !!row.modelData.header
                    text: row.modelData.header || ""
                    color: Qt.rgba(1, 1, 1, 0.45)
                    font.pixelSize: 11
                    font.capitalization: Font.AllUppercase
                    font.letterSpacing: 0.6
                    anchors.left: parent.left; anchors.leftMargin: 10
                    anchors.bottom: parent.bottom; anchors.bottomMargin: 4
                }
                Rectangle {
                    visible: !row.modelData.header
                    anchors.fill: parent
                    radius: 8
                    color: hover.hovered || list.currentIndex === row.index ? Qt.rgba(1, 1, 1, 0.08) : "transparent"
                    HoverHandler { id: hover }
                    TapHandler { onTapped: root.pick(row.index) }
                    Text {
                        anchors.left: parent.left; anchors.leftMargin: 10
                        anchors.right: check.left; anchors.rightMargin: 6
                        anchors.verticalCenter: parent.verticalCenter
                        text: row.modelData.name || ""
                        elide: Text.ElideRight
                        color: "#eceef4"
                        font.pixelSize: 14
                    }
                    Icon {
                        id: check
                        visible: row.modelData.code === root.current
                        anchors.right: parent.right; anchors.rightMargin: 10
                        anchors.verticalCenter: parent.verticalCenter
                        name: "check"; size: 16; color: "#eceef4"
                    }
                }
            }
        }
        Text {
            visible: root.rows.length === 0
            width: parent.width
            topPadding: 6; bottomPadding: 8
            horizontalAlignment: Text.AlignHCenter
            text: qsTr("No language matches")
            color: Qt.rgba(1, 1, 1, 0.45)
            font.pixelSize: 13
        }
    }
}

import QtQuick
import QtQuick.Shapes
import Selsearch

// One lens overlay per screen. Shows the frozen frame and handles selection:
// drag a rectangle, circle/scribble (lasso → bounding box), click a window,
// or drag across recognised text to select words.
Window {
    id: win
    required property var lens        // LensController
    required property var lensScreen  // LensScreen
    color: "black"
    flags: Qt.FramelessWindowHint
    title: "Selsearch"

    // ---- theme ----------------------------------------------------------------
    // "signal" (dot field, default), "mono" (plain darkening), "aurora".
    readonly property string themeName: lens.theme === "glass" ? "signal" : lens.theme
    readonly property color accent: lens.accent
    readonly property color rimA: themeName === "aurora" ? accent : "white"
    readonly property color rimB: themeName === "aurora" ? Qt.hsla((accent.hslHue + 0.42) % 1, accent.hslSaturation, accent.hslLightness, 1) : rimA

    // ---- selection state ------------------------------------------------------
    // phase: idle | drawing | adjusting | selected | texting | textSelected
    property string phase: "idle"
    property rect sel: Qt.rect(0, 0, 0, 0)
    property real selRadius: 18
    property real focusAmt: 0
    property real wave: -1
    property real time: 0
    property bool lasso: false
    property var lassoPoints: []
    property point pressPos
    property rect pressSel
    property string dragKind: ""   // new | move | resize-<edges> | text
    property int textAnchor: -1
    property int textFocus: -1
    property var hoverWindow: null
    property int hoverWord: -1
    property bool translated: false
    property bool interacted: false

    readonly property bool hasSel: sel.width > 2 && sel.height > 2
    readonly property bool isTextSel: phase === "texting" || phase === "textSelected"
    readonly property bool textMode: lens.mode === "text"
    readonly property var words: lensScreen.words
    readonly property int selFirst: Math.min(textAnchor, textFocus)
    readonly property int selLast: Math.max(textAnchor, textFocus)

    // Lines index for nearest-word lookups: [{top, bottom, first, last}]
    property var lines: []
    onWordsChanged: {
        let out = []
        for (let w of words) {
            let l = out[w.line]
            if (!l) out[w.line] = { top: w.y, bottom: w.y + w.h, first: w.index, last: w.index }
            else { l.top = Math.min(l.top, w.y); l.bottom = Math.max(l.bottom, w.y + w.h); l.last = w.index }
        }
        lines = out.filter(l => l !== undefined)
        if (lens.mode === "translate" && words.length > 0 && lensScreen.focused && !translated)
            translateAll()
    }

    function clearSelection() {
        phase = "idle"
        sel = Qt.rect(0, 0, 0, 0)
        lasso = false
        lassoPoints = []
        textAnchor = -1
        textFocus = -1
        focusAnim.stop(); focusAmt = 0
        waveAnim.stop(); wave = -1
        if (translated) { lens.clearTranslations(lensScreen.name); translated = false }
        langMenu.close()
    }

    Connections {
        target: win.lensScreen
        function onReset() { win.clearSelection(); win.interacted = false; win.hoverWindow = null; win.hoverWord = -1 }
    }
    Connections {
        target: win.lens
        function onActiveScreenChanged() {
            if (win.lens.activeScreen !== win.lensScreen.name && win.phase !== "idle") win.clearSelection()
        }
        function onNotify(message) {
            if (win.lens.activeScreen === win.lensScreen.name) snackbar.show(message)
        }
    }

    // ---- geometry helpers -----------------------------------------------------
    function clampRect(r) {
        let x = Math.max(0, Math.min(r.x, width - 1)), y = Math.max(0, Math.min(r.y, height - 1))
        let x2 = Math.max(x + 1, Math.min(r.x + r.width, width)), y2 = Math.max(y + 1, Math.min(r.y + r.height, height))
        return Qt.rect(x, y, x2 - x, y2 - y)
    }
    function rectFromPoints(a, b) {
        return Qt.rect(Math.min(a.x, b.x), Math.min(a.y, b.y), Math.abs(a.x - b.x), Math.abs(a.y - b.y))
    }
    // Works for Qt rects (width/height) and our word/window maps (w/h).
    function contains(r, x, y, pad) {
        pad = pad || 0
        const rw = r.width !== undefined ? r.width : r.w, rh = r.height !== undefined ? r.height : r.h
        return x >= r.x - pad && x <= r.x + rw + pad && y >= r.y - pad && y <= r.y + rh + pad
    }
    function wordAt(x, y) {
        for (let w of words) if (contains(w, x, y, 3)) return w.index
        return -1
    }
    // Nearest word in reading order, for extending a text selection.
    function nearestWord(x, y) {
        if (lines.length === 0) return -1
        let best = lines[0], bestD = 1e9
        for (let l of lines) {
            let d = y < l.top ? l.top - y : y > l.bottom ? y - l.bottom : 0
            if (d < bestD) { bestD = d; best = l }
        }
        let pick = best.first, pickD = 1e9
        for (let i = best.first; i <= best.last; ++i) {
            let w = words[i]
            let d = x < w.x ? w.x - x : x > w.x + w.w ? x - (w.x + w.w) : 0
            if (d < pickD) { pickD = d; pick = i }
        }
        return pick
    }
    function windowAt(x, y) {
        if (!lens.snapToWindows) return null
        for (let w of lensScreen.windows) if (contains(w, x, y)) return w
        return null
    }
    function edgesAt(x, y) {
        if (!hasSel || isTextSel || phase === "drawing") return ""
        const g = 14
        let l = Math.abs(x - sel.x) < g, r = Math.abs(x - (sel.x + sel.width)) < g
        let t = Math.abs(y - sel.y) < g, b = Math.abs(y - (sel.y + sel.height)) < g
        let inX = x > sel.x - g && x < sel.x + sel.width + g, inY = y > sel.y - g && y < sel.y + sel.height + g
        let e = ""
        if (t && inX) e += "t"
        if (b && inX) e += "b"
        if (l && inY) e += "l"
        if (r && inY) e += "r"
        return e
    }
    function cursorFor(edges) {
        switch (edges) {
        case "tl": case "br": return Qt.SizeFDiagCursor
        case "tr": case "bl": return Qt.SizeBDiagCursor
        case "t": case "b": return Qt.SizeVerCursor
        case "l": case "r": return Qt.SizeHorCursor
        }
        return Qt.CrossCursor
    }
    function wordsIn(r) {
        let out = []
        for (let w of words) if (contains(r, w.x + w.w / 2, w.y + w.h / 2)) out.push(w)
        return out
    }
    function joinWords(list) {
        let text = "", lastLine = -1
        for (let w of list) {
            if (lastLine >= 0) text += (w.line !== lastLine) ? "\n" : " "
            text += w.text
            lastLine = w.line
        }
        return text
    }
    function selectedText() {
        if (isTextSel && textAnchor >= 0) return joinWords(words.slice(selFirst, selLast + 1))
        if (hasSel) return joinWords(wordsIn(sel))
        return ""
    }
    function textSelectionRect() {
        if (textAnchor < 0) return Qt.rect(0, 0, 0, 0)
        let x1 = 1e9, y1 = 1e9, x2 = -1e9, y2 = -1e9
        for (let i = selFirst; i <= selLast; ++i) {
            let w = words[i]
            x1 = Math.min(x1, w.x); y1 = Math.min(y1, w.y); x2 = Math.max(x2, w.x + w.w); y2 = Math.max(y2, w.y + w.h)
        }
        return Qt.rect(x1, y1, x2 - x1, y2 - y1)
    }

    // ---- actions ----------------------------------------------------------------
    function finishRegion(animated) {
        phase = "selected"
        lens.activeScreen = lensScreen.name
        if (animated) {
            radiusAnim.restart()
            waveAnim.restart()
        }
        focusAnim.to = 1
        focusAnim.restart()
        switch (lens.onSelect) {
        case "search_image": doSearch(); break
        case "search_text": selectedText().length > 0 ? lens.searchText(selectedText()) : doSearch(); break
        case "ask": doAsk(); break
        }
    }
    function finishText() {
        phase = "textSelected"
        lens.activeScreen = lensScreen.name
        focusAnim.to = 0.35
        focusAnim.restart()
    }
    function actionRect() { return isTextSel ? textSelectionRect() : sel }
    function doSearch() {
        if (isTextSel) lens.searchText(selectedText())
        else lens.searchImage(lensScreen.name, sel)
    }
    function doCopy() {
        if (isTextSel) lens.copyText(selectedText())
        else if (hasSel) lens.copyImage(lensScreen.name, sel)
        else return
        closeSoon.restart()
    }
    function doCopyText() {
        const t = selectedText()
        if (t.length === 0) { snackbar.show(lensScreen.ocrBusy ? qsTr("Still reading the text…") : qsTr("No text found here")); return }
        lens.copyText(t)
        closeSoon.restart()
    }
    function doAsk() { lens.ask(lensScreen.name, isTextSel ? Qt.rect(0, 0, 0, 0) : sel, selectedText()) }
    function doTranslate() {
        if (translated) { lens.clearTranslations(lensScreen.name); translated = false; return }
        translated = true
        lens.translateRegion(lensScreen.name, actionRect())
    }
    function translateAll() {
        translated = true
        lens.activeScreen = lensScreen.name
        lens.translateRegion(lensScreen.name, Qt.rect(0, 0, 0, 0))
    }
    function selectAllText() {
        if (words.length === 0) return
        textAnchor = 0; textFocus = words.length - 1
        sel = Qt.rect(0, 0, 0, 0)
        finishText()
    }

    Timer { id: closeSoon; interval: 650; onTriggered: win.lens.close() }

    // ---- animations -------------------------------------------------------------
    NumberAnimation { id: focusAnim; target: win; property: "focusAmt"; to: 1; duration: 260; easing.type: Easing.OutCubic }
    NumberAnimation { id: radiusAnim; target: win; property: "selRadius"; from: 6; to: 18; duration: 420; easing.type: Easing.OutBack; easing.overshoot: 2.2 }
    NumberAnimation { id: waveAnim; target: win; property: "wave"; from: 0; to: 1; duration: 1100; easing.type: Easing.OutCubic
        onFinished: win.wave = -1 }
    // Clock for the dot shimmer / scan line; runs only while the lens is up.
    NumberAnimation on time {
        running: win.visible && win.themeName !== "mono"
        from: 0; to: 3600; duration: 3600 * 1000
        loops: Animation.Infinite
    }
    // Lasso → rectangle "snap"
    PropertyAnimation { id: snapAnim; target: win; property: "sel"; duration: 360; easing.type: Easing.OutBack; easing.overshoot: 1.4 }

    // ---- scene --------------------------------------------------------------------
    Item {
        id: scene
        anchors.fill: parent
        focus: true

        Image {
            id: frame
            visible: false
            source: win.lensScreen.frameUrl
            asynchronous: false
            cache: false
            smooth: true
        }

        ShaderEffect {
            id: backdrop
            anchors.fill: parent
            property variant source: frame
            property vector4d sel: Qt.vector4d(win.sel.x, win.sel.y, win.sel.width, win.sel.height)
            property vector2d size: Qt.vector2d(width, height)
            property real radius: win.isTextSel ? 0 : win.selRadius
            property real hasSel: win.hasSel && !win.isTextSel ? 1 : 0
            property real focusAmt: win.focusAmt
            property real idleDim: win.themeName === "mono" ? 0.28 : 0.22
            property real dim: win.themeName === "mono" ? 0.55 : 0.5
            property real rimAlpha: win.phase === "drawing" ? 0.75 : 0.95
            property real rimWidth: win.themeName === "aurora" ? 2.0 : 1.5
            property real wave: win.wave
            property real time: win.time
            property real dots: win.themeName === "mono" ? 0 : (win.isTextSel ? 0.25 : 1)
            property real spacing: 9
            property real tintMix: win.themeName === "aurora" ? 0.14 : 0
            property color rimColor: win.rimA
            property color rimColor2: win.rimB
            property color dotColor: win.themeName === "aurora" ? win.rimB : win.accent
            fragmentShader: "qrc:/shaders/glass.frag.qsb"
        }

        // Hovered window outline (click selects the window).
        Rectangle {
            visible: win.hoverWindow !== null && win.phase === "idle" && win.hoverWord < 0
            x: win.hoverWindow ? win.hoverWindow.x + 1 : 0
            y: win.hoverWindow ? win.hoverWindow.y + 1 : 0
            width: win.hoverWindow ? win.hoverWindow.w - 2 : 0
            height: win.hoverWindow ? win.hoverWindow.h - 2 : 0
            radius: 10
            color: Qt.rgba(1, 1, 1, 0.04)
            border.color: Qt.rgba(1, 1, 1, 0.55)
            border.width: 1.5
            Behavior on x { NumberAnimation { duration: 90; easing.type: Easing.OutCubic } }
            Behavior on y { NumberAnimation { duration: 90; easing.type: Easing.OutCubic } }
            Behavior on width { NumberAnimation { duration: 90; easing.type: Easing.OutCubic } }
            Behavior on height { NumberAnimation { duration: 90; easing.type: Easing.OutCubic } }
        }

        // Recognised words: pills inside the selection cascade in; hovered and
        // selected words are highlighted anywhere.
        Repeater {
            model: win.words
            delegate: Rectangle {
                id: pill
                required property var modelData
                readonly property bool selected: win.isTextSel && modelData.index >= win.selFirst && modelData.index <= win.selLast
                readonly property bool inRegion: win.phase === "selected" && win.contains(win.sel, modelData.x + modelData.w / 2, modelData.y + modelData.h / 2)
                readonly property bool hovered: win.hoverWord === modelData.index && !win.isTextSel
                x: modelData.x - 2
                y: modelData.y - 1
                width: modelData.w + 4
                height: modelData.h + 2
                radius: Math.min(6, height / 3)
                color: selected ? Qt.rgba(win.accent.r, win.accent.g, win.accent.b, 0.42)
                     : hovered ? Qt.rgba(1, 1, 1, 0.22)
                     : inRegion ? Qt.rgba(1, 1, 1, 0.13) : "transparent"
                border.color: selected ? Qt.rgba(win.accent.r, win.accent.g, win.accent.b, 0.8) : "transparent"
                border.width: selected ? 1 : 0
                opacity: inRegion && !selected && !hovered ? 0 : 1
                visible: color.a > 0
                states: State {
                    when: inRegion && !selected && !hovered
                    PropertyChanges { pill.opacity: 1 }
                }
                transitions: Transition {
                    SequentialAnimation {
                        PauseAnimation { duration: (modelData.index % 24) * 14 }
                        NumberAnimation { property: "opacity"; duration: 220; easing.type: Easing.OutCubic }
                    }
                }
            }
        }

        // Translations painted over the original lines.
        Repeater {
            model: win.lensScreen.translations
            delegate: Rectangle {
                required property var modelData
                required property int index
                x: modelData.x - 3
                y: modelData.y - 2
                width: modelData.w + 6
                height: modelData.h + 4
                radius: 4
                color: modelData.bg
                opacity: 0
                Component.onCompleted: opacity = 1
                Behavior on opacity { SequentialAnimation { PauseAnimation { duration: index * 18 } NumberAnimation { duration: 200 } } }
                Text {
                    anchors.fill: parent
                    anchors.leftMargin: 3
                    anchors.rightMargin: 3
                    text: modelData.text
                    color: modelData.fg
                    font.pixelSize: Math.max(9, modelData.h * 0.82)
                    fontSizeMode: Text.HorizontalFit
                    minimumPixelSize: 8
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideRight
                }
            }
        }

        // Lasso stroke while circling.
        Shape {
            id: lassoShape
            anchors.fill: parent
            visible: win.lasso && win.lassoPoints.length > 1
            opacity: win.phase === "drawing" ? 1 : 0
            Behavior on opacity { NumberAnimation { duration: 220 } }
            preferredRendererType: Shape.CurveRenderer
            ShapePath {
                strokeColor: Qt.rgba(win.rimA.r, win.rimA.g, win.rimA.b, 0.28)
                strokeWidth: 12
                fillColor: "transparent"
                capStyle: ShapePath.RoundCap
                joinStyle: ShapePath.RoundJoin
                PathPolyline { path: win.lassoPoints }
            }
            ShapePath {
                strokeColor: win.rimA
                strokeWidth: 3.5
                fillColor: "transparent"
                capStyle: ShapePath.RoundCap
                joinStyle: ShapePath.RoundJoin
                PathPolyline { path: win.lassoPoints }
            }
        }

        // Corner handles of a finished region: short arcs hugging the corners.
        Shape {
            anchors.fill: parent
            visible: win.phase === "selected" || win.phase === "adjusting"
            preferredRendererType: Shape.CurveRenderer
            readonly property real r: Math.max(10, Math.min(win.selRadius, Math.min(win.sel.width, win.sel.height) / 2))
            readonly property real o: 3   // offset outside the rim
            component Corner: ShapePath {
                property real cx
                property real cy
                property real start
                strokeColor: win.rimA
                strokeWidth: 4
                fillColor: "transparent"
                capStyle: ShapePath.RoundCap
                PathAngleArc {
                    centerX: cx; centerY: cy
                    radiusX: handles.r + handles.o; radiusY: handles.r + handles.o
                    startAngle: start; sweepAngle: 90
                    moveToStart: true
                }
            }
            id: handles
            Corner { cx: win.sel.x + handles.r; cy: win.sel.y + handles.r; start: 180 }
            Corner { cx: win.sel.x + win.sel.width - handles.r; cy: win.sel.y + handles.r; start: 270 }
            Corner { cx: win.sel.x + win.sel.width - handles.r; cy: win.sel.y + win.sel.height - handles.r; start: 0 }
            Corner { cx: win.sel.x + handles.r; cy: win.sel.y + win.sel.height - handles.r; start: 90 }
        }

        // ---- input ----------------------------------------------------------
        MouseArea {
            id: input
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            cursorShape: {
                if (win.dragKind === "move") return Qt.ClosedHandCursor
                if (win.dragKind.startsWith("resize-")) return win.cursorFor(win.dragKind.substring(7))
                if (win.dragKind === "text") return Qt.IBeamCursor
                return Qt.CrossCursor
            }

            function updateHover(x, y) {
                const e = win.edgesAt(x, y)
                if (e) { cursorShape = win.cursorFor(e); win.hoverWord = -1; return }
                win.hoverWord = win.lens.recognizeText ? win.wordAt(x, y) : -1
                if (win.hoverWord >= 0) { cursorShape = Qt.IBeamCursor; win.hoverWindow = null; return }
                if (win.phase === "selected" && win.contains(win.sel, x, y)) { cursorShape = Qt.OpenHandCursor; return }
                cursorShape = Qt.CrossCursor
                win.hoverWindow = win.phase === "idle" ? win.windowAt(x, y) : null
            }

            onPositionChanged: (mouse) => {
                if (!pressed) { updateHover(mouse.x, mouse.y); return }
                const p = Qt.point(mouse.x, mouse.y)
                const dx = p.x - win.pressPos.x, dy = p.y - win.pressPos.y
                switch (win.dragKind) {
                case "new": {
                    if (win.phase !== "drawing" && Math.abs(dx) < 4 && Math.abs(dy) < 4) return
                    if (win.phase !== "drawing") {
                        win.phase = "drawing"
                        win.interacted = true
                        win.lens.activeScreen = win.lensScreen.name
                        win.selRadius = 6
                        focusAnim.to = 0.45; focusAnim.restart()
                        if (win.translated) { win.lens.clearTranslations(win.lensScreen.name); win.translated = false }
                    }
                    let pts = win.lassoPoints
                    pts.push(p)
                    win.lassoPoints = pts
                    const direct = win.rectFromPoints(win.pressPos, p)
                    if (!win.lasso && pts.length > 8) {
                        // A path that leaves the drag rectangle a lot is a lasso.
                        let minX = 1e9, minY = 1e9, maxX = -1e9, maxY = -1e9
                        for (let q of pts) { minX = Math.min(minX, q.x); minY = Math.min(minY, q.y); maxX = Math.max(maxX, q.x); maxY = Math.max(maxY, q.y) }
                        const bboxArea = (maxX - minX) * (maxY - minY)
                        const directArea = direct.width * direct.height
                        if (bboxArea > directArea * 1.6 + 2500) win.lasso = true
                    }
                    if (win.lasso) {
                        let minX = 1e9, minY = 1e9, maxX = -1e9, maxY = -1e9
                        for (let q of pts) { minX = Math.min(minX, q.x); minY = Math.min(minY, q.y); maxX = Math.max(maxX, q.x); maxY = Math.max(maxY, q.y) }
                        win.sel = Qt.rect(minX, minY, maxX - minX, maxY - minY)
                    } else {
                        win.sel = direct
                    }
                    break
                }
                case "move":
                    win.sel = win.clampRect(Qt.rect(win.pressSel.x + dx, win.pressSel.y + dy, win.pressSel.width, win.pressSel.height))
                    win.phase = "adjusting"
                    break
                case "text":
                    win.textFocus = win.nearestWord(p.x, p.y)
                    win.phase = "texting"
                    break
                default:
                    if (win.dragKind.startsWith("resize-")) {
                        const e = win.dragKind.substring(7)
                        let x1 = win.pressSel.x, y1 = win.pressSel.y, x2 = x1 + win.pressSel.width, y2 = y1 + win.pressSel.height
                        if (e.includes("l")) x1 = Math.min(p.x, x2 - 12)
                        if (e.includes("r")) x2 = Math.max(p.x, x1 + 12)
                        if (e.includes("t")) y1 = Math.min(p.y, y2 - 12)
                        if (e.includes("b")) y2 = Math.max(p.y, y1 + 12)
                        win.sel = win.clampRect(Qt.rect(x1, y1, x2 - x1, y2 - y1))
                        win.phase = "adjusting"
                        win.selRadius = 4
                    }
                }
            }

            onPressed: (mouse) => {
                if (mouse.button === Qt.RightButton) { win.clearSelection(); return }
                langMenu.close()
                win.pressPos = Qt.point(mouse.x, mouse.y)
                win.pressSel = win.sel
                win.hoverWindow = null
                const edges = win.edgesAt(mouse.x, mouse.y)
                const word = win.lens.recognizeText ? (win.textMode ? win.nearestWord(mouse.x, mouse.y) : win.wordAt(mouse.x, mouse.y)) : -1
                if (edges) {
                    win.dragKind = "resize-" + edges
                } else if (word >= 0) {
                    // Pressing on text selects text, even inside a region.
                    win.dragKind = "text"
                    win.interacted = true
                    win.lens.activeScreen = win.lensScreen.name
                    if (mouse.modifiers & Qt.ShiftModifier && win.textAnchor >= 0) {
                        win.textFocus = word
                    } else {
                        if (win.phase !== "texting" && win.phase !== "textSelected") win.clearSelection()
                        win.textAnchor = word
                        win.textFocus = word
                    }
                    win.phase = "texting"
                } else if (win.phase === "selected" && win.contains(win.sel, mouse.x, mouse.y)) {
                    win.dragKind = "move"
                } else {
                    win.dragKind = "new"
                    if (win.phase !== "idle") win.clearSelection()
                    win.lassoPoints = [Qt.point(mouse.x, mouse.y)]
                    win.lasso = false
                }
            }

            onReleased: (mouse) => {
                const kind = win.dragKind
                win.dragKind = ""
                if (mouse.button === Qt.RightButton) return
                if (kind === "new") {
                    if (win.phase !== "drawing") {
                        // A click: select the window under the pointer, or close.
                        const w = win.windowAt(mouse.x, mouse.y)
                        if (w) {
                            win.interacted = true
                            win.sel = Qt.rect(w.x, w.y, w.w, w.h)
                            win.finishRegion(true)
                        } else {
                            win.lens.close()
                        }
                        return
                    }
                    if (win.sel.width < 8 || win.sel.height < 8) { win.clearSelection(); return }
                    if (win.lasso) {
                        // Snap the scribble's bounding box to a padded rectangle.
                        const pad = 6
                        snapAnim.from = win.sel
                        snapAnim.to = win.clampRect(Qt.rect(win.sel.x - pad, win.sel.y - pad, win.sel.width + 2 * pad, win.sel.height + 2 * pad))
                        snapAnim.restart()
                    }
                    win.finishRegion(true)
                } else if (kind === "move" || kind.startsWith("resize-")) {
                    win.finishRegion(false)
                    radiusAnim.restart()
                } else if (kind === "text") {
                    win.finishText()
                }
                updateHover(mouse.x, mouse.y)
            }

            onDoubleClicked: (mouse) => {
                const w = win.wordAt(mouse.x, mouse.y)
                if (w < 0) return
                // Double-click selects the whole line.
                const line = win.words[w].line
                let first = w, last = w
                while (first > 0 && win.words[first - 1].line === line) --first
                while (last < win.words.length - 1 && win.words[last + 1].line === line) ++last
                win.textAnchor = first
                win.textFocus = last
                win.finishText()
            }
        }

        // ---- keyboard ---------------------------------------------------------
        Keys.onPressed: (event) => {
            if (event.key === Qt.Key_Escape) {
                if (langMenu.opened) langMenu.close()
                else if (win.phase !== "idle") win.clearSelection()
                else win.lens.close()
            } else if (event.matches(StandardKey.Copy)) {
                win.doCopy()
            } else if (event.key === Qt.Key_C && (event.modifiers & Qt.ControlModifier) && (event.modifiers & Qt.ShiftModifier)) {
                win.doCopyText()
            } else if (event.matches(StandardKey.SelectAll)) {
                win.selectAllText()
            } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                if (win.hasSel || win.isTextSel) win.doSearch()
            } else if (event.key === Qt.Key_T && event.modifiers === Qt.NoModifier) {
                if (win.hasSel || win.isTextSel) win.doTranslate(); else win.translateAll()
            } else if (event.key === Qt.Key_A && event.modifiers === Qt.NoModifier) {
                if (win.hasSel || win.isTextSel) win.doAsk()
            } else if (event.matches(StandardKey.Save)) {
                if (win.hasSel) win.lens.saveImage(win.lensScreen.name, win.sel)
            } else {
                return
            }
            event.accepted = true
        }

        // ---- chrome -------------------------------------------------------------
        HintPill {
            anchors.horizontalCenter: parent.horizontalCenter
            y: 22
            visible: win.lens.showHints && win.lensScreen.focused
            shown: !win.interacted && win.lens.active
            ocrBusy: win.lensScreen.ocrBusy
            textMode: win.textMode
        }

        LensToolbar {
            id: toolbar
            readonly property rect target: win.actionRect()
            readonly property bool wanted: win.phase === "selected" || win.phase === "textSelected"
            readonly property bool below: target.y + target.height + height + 28 < win.height
            readonly property bool above: target.y - height - 28 > 0
            x: Math.max(12, Math.min(win.width - width - 12, target.x + target.width / 2 - width / 2))
            y: below ? target.y + target.height + 16 : above ? target.y - height - 16 : target.y + target.height - height - 16
            visible: opacity > 0
            opacity: wanted ? 1 : 0
            scale: wanted ? 1 : 0.94
            Behavior on opacity { NumberAnimation { duration: 160 } }
            Behavior on scale { NumberAnimation { duration: 220; easing.type: Easing.OutBack } }
            accent: win.accent
            textMode: win.isTextSel
            hasText: win.phase === "selected" && win.wordsIn(win.sel).length > 0
            ocrBusy: win.lensScreen.ocrBusy
            translating: win.lensScreen.translating
            translated: win.translated
            targetLang: win.lens.translateTarget
            onSearch: win.doSearch()
            onCopyImage: win.doCopy()
            onCopyText: win.doCopyText()
            onTranslate: win.doTranslate()
            onPickLanguage: (anchor) => {
                const p = anchor.mapToItem(scene, 0, anchor.height + 6)
                langMenu.languages = win.lens.translateLanguages()
                langMenu.recent = win.lens.recentTranslateLanguages()
                langMenu.current = win.lens.translateTarget
                langMenu.x = Math.min(p.x, win.width - langMenu.width - 8)
                langMenu.y = toolbar.below ? p.y : toolbar.y - langMenu.height - 8
                langMenu.open()
            }
            onAsk: win.doAsk()
            onSave: win.lens.saveImage(win.lensScreen.name, win.sel)
        }

        LanguageMenu {
            id: langMenu
            parent: scene
            onPicked: (code) => {
                win.lens.setTranslateTarget(code)
                if (win.translated) win.lens.clearTranslations(win.lensScreen.name)
                win.translated = true
                win.lens.translateRegion(win.lensScreen.name, win.actionRect())
            }
        }

        Snackbar {
            id: snackbar
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 48
        }
    }
}

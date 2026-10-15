import QtQuick
import QtQuick.Shapes
import Selsearch

// One lens overlay per screen. Shows the frozen frame and handles selection:
// drag a rectangle, click a window,
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
    property real pulse: -1
    property real time: 0
    property point pressPos
    property rect pressSel
    property string dragKind: ""   // new | move | resize-<edges> | text
    property int textAnchor: -1
    property int textFocus: -1
    property var picked: ({})      // Ctrl+click picks: { index: true }
    property int pickedCount: 0
    property var hoverWindow: null
    property int hoverWord: -1
    property bool translated: false
    property bool interacted: false

    readonly property bool hasSel: sel.width > 2 && sel.height > 2
    readonly property bool isTextSel: phase === "texting" || phase === "textSelected"
    function isSelectedWord(i) { return isTextSel && ((textAnchor >= 0 && i >= selFirst && i <= selLast) || !!picked[i]) }
    // Selected words in reading order (range ∪ picks).
    readonly property var selectedWordList: {
        if (!isTextSel) return []
        pickedCount  // dependency
        let out = []
        for (const w of words) if (isSelectedWord(w.index)) out.push(w)
        return out
    }
    readonly property var regionWords: phase === "selected" ? wordsIn(sel) : []
    readonly property bool textMode: lens.mode === "text"
    // Plain JS copies of the screen's words/windows. Reading lensScreen.words
    // converts the whole C++ list on every access, which made hover hit-tests
    // on every mouse move expensive on text-heavy screens.
    property var words: []
    property var windowList: []
    readonly property int selFirst: Math.min(textAnchor, textFocus)
    readonly property int selLast: Math.max(textAnchor, textFocus)

    // Lines index for nearest-word lookups: [{top, bottom, first, last}]
    property var lines: []
    function reloadWords() {
        words = lensScreen.words.map(w => ({ x: w.x, y: w.y, w: w.w, h: w.h, text: w.text, line: w.line, index: w.index, space: w.space }))
    }
    Component.onCompleted: { reloadWords(); windowList = lensScreen.windows.map(w => ({ x: w.x, y: w.y, w: w.w, h: w.h })) }
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
        if (words.length > 0)
            pulseAnim.restart()   // text is ready: the edge dots flash once
    }

    function clearSelection() {
        phase = "idle"
        sel = Qt.rect(0, 0, 0, 0)
        textAnchor = -1
        textFocus = -1
        picked = ({})
        pickedCount = 0
        focusAnim.stop(); focusAmt = 0
        waveAnim.stop(); wave = -1
        if (translated) { lens.clearTranslations(lensScreen.name); translated = false }
        langMenu.close()
    }

    Connections {
        target: win.lensScreen
        function onWordsChanged() { win.reloadWords() }
        function onWindowsChanged() { win.windowList = win.lensScreen.windows.map(w => ({ x: w.x, y: w.y, w: w.w, h: w.h })) }
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
        for (let w of windowList) if (contains(w, x, y)) return w
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
        let text = "", lastLine = -1, lastIndex = -2
        for (let w of list) {
            if (lastLine >= 0) {
                if (w.line !== lastLine) text += "\n"
                else if (w.index !== lastIndex + 1) text += " "   // gap between picked words
                else if (w.space !== false) text += " "            // no spaces inside CJK runs
            }
            text += w.text
            lastLine = w.line
            lastIndex = w.index
        }
        return text
    }
    function selectedText() {
        if (isTextSel) return joinWords(selectedWordList)
        if (hasSel) return joinWords(wordsIn(sel))
        return ""
    }
    function textSelectionRect() {
        const list = selectedWordList
        if (list.length === 0) return Qt.rect(0, 0, 0, 0)
        let x1 = 1e9, y1 = 1e9, x2 = -1e9, y2 = -1e9
        for (const w of list) {
            x1 = Math.min(x1, w.x); y1 = Math.min(y1, w.y); x2 = Math.max(x2, w.x + w.w); y2 = Math.max(y2, w.y + w.h)
        }
        return Qt.rect(x1, y1, x2 - x1, y2 - y1)
    }

    // ---- actions ----------------------------------------------------------------
    function finishRegion(animated) {
        phase = "selected"
        lens.activeScreen = lensScreen.name
        lens.recognizeRegion(lensScreen.name, sel)   // only with "read text in the selection only"
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
    function doAsk() { if (!lens.aiEnabled) return; lens.ask(lensScreen.name, isTextSel ? Qt.rect(0, 0, 0, 0) : sel, selectedText()) }
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
        picked = ({}); pickedCount = 0
        textAnchor = 0; textFocus = words.length - 1
        sel = Qt.rect(0, 0, 0, 0)
        finishText()
    }

    Timer { id: closeSoon; interval: 650; onTriggered: win.lens.close() }

    // ---- keys -----------------------------------------------------------------
    readonly property int ctrl: Qt.ControlModifier
    function handlesKey(key, mods) {
        switch (key) {
        case Qt.Key_Escape: case Qt.Key_Return: case Qt.Key_Enter: return true
        case Qt.Key_C: return mods === ctrl || mods === (ctrl | Qt.ShiftModifier)
        case Qt.Key_A: return mods === ctrl || mods === Qt.NoModifier
        case Qt.Key_S: return mods === ctrl
        case Qt.Key_T: return mods === Qt.NoModifier
        }
        return false
    }
    function handleKey(key, mods) {
        const any = hasSel || isTextSel
        switch (key) {
        case Qt.Key_Escape:
            if (langMenu.opened) langMenu.close()
            else if (phase !== "idle") clearSelection()
            else lens.close()
            break
        case Qt.Key_Return: case Qt.Key_Enter:
            if (any) doSearch()
            break
        case Qt.Key_C:
            if (mods & Qt.ShiftModifier) doCopyText(); else doCopy()
            break
        case Qt.Key_A:
            if (mods === ctrl) selectAllText()
            else if (any && lens.aiEnabled) doAsk()
            break
        case Qt.Key_S:
            if (hasSel) lens.saveImage(lensScreen.name, sel)
            break
        case Qt.Key_T:
            if (any) doTranslate(); else translateAll()
            break
        }
    }

    // ---- animations -------------------------------------------------------------
    NumberAnimation { id: focusAnim; target: win; property: "focusAmt"; to: 1; duration: 260; easing.type: Easing.OutCubic }
    NumberAnimation { id: radiusAnim; target: win; property: "selRadius"; from: 6; to: 18; duration: 420; easing.type: Easing.OutBack; easing.overshoot: 2.2 }
    NumberAnimation { id: waveAnim; target: win; property: "wave"; from: 0; to: 1; duration: 1100; easing.type: Easing.OutCubic
        onFinished: win.wave = -1 }
    NumberAnimation { id: pulseAnim; target: win; property: "pulse"; from: 0; to: 1; duration: 900; easing.type: Easing.OutQuad
        onFinished: win.pulse = -1 }
    // Clock for the dot shimmer / edge glow, stepped at ~30 fps: the glow is
    // ambient, and redrawing full-screen overlays at 144 Hz+ on every monitor
    // would keep a core busy. Interactions still render at full rate.
    Timer {
        interval: 33
        repeat: true
        running: win.visible && win.themeName !== "mono"
        property real started: 0
        onRunningChanged: if (running) started = Date.now()
        onTriggered: win.time = (Date.now() - started) / 1000
    }

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
            property real pulse: win.pulse
            property real time: win.time
            property real dots: win.themeName === "mono" ? 0 : (win.isTextSel ? 0.25 : 1)
            property real spacing: 9
            property real tintMix: win.themeName === "aurora" ? 0.14 : 0
            property color rimColor: win.rimA
            property color rimColor2: win.rimB
            property color dotColor: win.themeName === "aurora" ? win.rimB : win.accent
            property real boost: win.lens.dotBrightness
            fragmentShader: "qrc:/shaders/glass.frag.qsb"
        }

        // Hovered window outline (click selects the window).
        // It stays put while the pointer moves between text and empty space
        // inside the same window, and fades instead of popping.
        Rectangle {
            id: windowOutline
            property var target: null
            Connections {
                target: win
                function onHoverWindowChanged() { if (win.hoverWindow) windowOutline.target = win.hoverWindow }
            }
            opacity: win.hoverWindow !== null && win.phase === "idle" ? 1 : 0
            visible: opacity > 0 && target !== null
            Behavior on opacity { NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }
            x: target ? target.x + 1 : 0
            y: target ? target.y + 1 : 0
            width: target ? target.w - 2 : 0
            height: target ? target.h - 2 : 0
            radius: 10
            color: Qt.rgba(1, 1, 1, 0.04)
            border.color: Qt.rgba(1, 1, 1, 0.55)
            border.width: 1.5
            Behavior on x { NumberAnimation { duration: 90; easing.type: Easing.OutCubic } }
            Behavior on y { NumberAnimation { duration: 90; easing.type: Easing.OutCubic } }
            Behavior on width { NumberAnimation { duration: 90; easing.type: Easing.OutCubic } }
            Behavior on height { NumberAnimation { duration: 90; easing.type: Easing.OutCubic } }
        }

        // Recognised words. Only words that are highlighted get an item:
        // creating hundreds of items at once would stall the first frames.
        component WordPill: Rectangle {
            required property var word
            property bool strong: false
            x: word.x - 2
            y: word.y - 1
            width: word.w + 4
            height: word.h + 2
            radius: Math.min(6, height / 3)
            color: strong ? Qt.rgba(win.accent.r, win.accent.g, win.accent.b, 0.42) : Qt.rgba(1, 1, 1, 0.16)
            border.color: strong ? Qt.rgba(win.accent.r, win.accent.g, win.accent.b, 0.85) : "transparent"
            border.width: strong ? 1 : 0
            Behavior on color { ColorAnimation { duration: 90 } }
        }
        // Words inside a finished region cascade in.
        Repeater {
            model: win.phase === "selected" ? win.regionWords : []
            delegate: WordPill {
                required property var modelData
                required property int index
                word: modelData
                opacity: 0
                Component.onCompleted: fadeIn.start()
                SequentialAnimation on opacity {
                    id: fadeIn
                    running: false
                    PauseAnimation { duration: Math.min(index, 30) * 12 }
                    NumberAnimation { to: 1; duration: 200; easing.type: Easing.OutCubic }
                }
            }
        }
        // Selected words (drag range and Ctrl+click picks). Keyed by word
        // index so extending a selection only animates the new words.
        ListModel { id: selModel }
        Connections {
            target: win
            // Diff instead of resetting, so existing pills don't re-animate.
            function onSelectedWordListChanged() {
                const want = {}
                for (const w of win.selectedWordList) want[w.index] = true
                for (let i = selModel.count - 1; i >= 0; --i) {
                    const idx = selModel.get(i).idx
                    if (want[idx]) delete want[idx]
                    else selModel.remove(i)
                }
                for (const k in want) selModel.append({ idx: Number(k) })
            }
        }
        Repeater {
            model: selModel
            delegate: WordPill {
                id: selPill
                required property int idx
                word: idx < win.words.length ? win.words[idx] : ({ x: 0, y: 0, w: 0, h: 0 })
                strong: true
                opacity: 0
                scale: 0.92
                Component.onCompleted: appear.start()
                ParallelAnimation {
                    id: appear
                    NumberAnimation { target: selPill; property: "opacity"; to: 1; duration: 90; easing.type: Easing.OutCubic }
                    NumberAnimation { target: selPill; property: "scale"; to: 1; duration: 120; easing.type: Easing.OutBack; easing.overshoot: 2 }
                }
            }
        }
        // Hovered word: glides from word to word and fades in/out.
        WordPill {
            id: hoverPill
            readonly property bool active: win.hoverWord >= 0 && win.hoverWord < win.words.length && !win.isSelectedWord(win.hoverWord)
            property var last: ({ x: 0, y: 0, w: 0, h: 0 })
            word: active ? win.words[win.hoverWord] : last
            onWordChanged: if (active) last = word
            color: Qt.rgba(1, 1, 1, 0.22)
            opacity: active ? 1 : 0
            visible: opacity > 0
            Behavior on opacity { NumberAnimation { duration: 110 } }
            Behavior on x { enabled: hoverPill.opacity > 0.5; NumberAnimation { duration: 80; easing.type: Easing.OutCubic } }
            Behavior on y { enabled: hoverPill.opacity > 0.5; NumberAnimation { duration: 80; easing.type: Easing.OutCubic } }
            Behavior on width { enabled: hoverPill.opacity > 0.5; NumberAnimation { duration: 80; easing.type: Easing.OutCubic } }
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
                if (win.hoverWord >= 0) {
                    cursorShape = Qt.IBeamCursor
                    win.hoverWindow = win.phase === "idle" ? win.windowAt(x, y) : null
                    return
                }
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
                    win.sel = win.rectFromPoints(win.pressPos, p)
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
                } else if (word >= 0 && (mouse.modifiers & Qt.ControlModifier)) {
                    // Ctrl+click toggles single words in or out of the selection.
                    win.dragKind = "pick"
                    win.interacted = true
                    win.lens.activeScreen = win.lensScreen.name
                    if (!win.isTextSel) win.clearSelection()
                    // Fold a dragged range into individual picks, then toggle.
                    let p = Object.assign({}, win.picked)
                    if (win.textAnchor >= 0) {
                        for (let i = win.selFirst; i <= win.selLast; ++i) p[i] = true
                        win.textAnchor = -1
                        win.textFocus = -1
                    }
                    if (p[word]) delete p[word]
                    else p[word] = true
                    win.picked = p
                    win.pickedCount = Object.keys(p).length
                    if (win.pickedCount === 0) { win.clearSelection(); return }
                    win.phase = "texting"
                } else if (word >= 0) {
                    // Pressing on text selects text, even inside a region.
                    win.dragKind = "text"
                    win.interacted = true
                    win.lens.activeScreen = win.lensScreen.name
                    if (mouse.modifiers & Qt.ShiftModifier && win.textAnchor >= 0) {
                        win.textFocus = word
                    } else {
                        win.clearSelection()
                        win.textAnchor = word
                        win.textFocus = word
                    }
                    win.phase = "texting"
                } else if (win.phase === "selected" && win.contains(win.sel, mouse.x, mouse.y)) {
                    win.dragKind = "move"
                } else {
                    win.dragKind = "new"
                    if (win.phase !== "idle") win.clearSelection()
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
                    win.finishRegion(true)
                } else if (kind === "move" || kind.startsWith("resize-")) {
                    win.finishRegion(false)
                    radiusAnim.restart()
                } else if (kind === "text" || kind === "pick") {
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
        // Keyboard focus can sit on another screen's overlay than the one with
        // the selection (Hyprland gives it to whichever lens surface it
        // likes); keys pressed on an idle, inactive overlay go to the active one.
        Keys.onPressed: (event) => {
            const mods = event.modifiers & (Qt.ControlModifier | Qt.ShiftModifier | Qt.AltModifier | Qt.MetaModifier)
            if (!win.handlesKey(event.key, mods)) return
            event.accepted = true
            if (win.phase === "idle" && win.lens.activeScreen !== "" && win.lens.activeScreen !== win.lensScreen.name)
                win.lens.forwardKey(event.key, mods)
            else
                win.handleKey(event.key, mods)
        }
        Connections {
            target: win.lens
            function onKeyForwarded(key, mods) {
                if (win.lens.activeScreen === win.lensScreen.name) win.handleKey(key, mods)
            }
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
                // Pressing the button already closed an open menu (press
                // outside); this click must not reopen it.
                if (langMenu.visible || Date.now() - langMenu.closedAt < 300) { langMenu.close(); return }
                const p = anchor.mapToItem(scene, 0, anchor.height + 6)
                langMenu.languages = win.lens.translateLanguages()
                langMenu.recent = win.lens.recentTranslateLanguages()
                langMenu.current = win.lens.translateTarget
                langMenu.x = Math.min(p.x, win.width - langMenu.width - 8)
                langMenu.y = toolbar.below ? p.y : toolbar.y - langMenu.height - 8
                langMenu.open()
            }
            aiEnabled: win.lens.aiEnabled
            onAsk: win.doAsk()
            onSave: win.lens.saveImage(win.lensScreen.name, win.sel)
        }

        LanguageMenu {
            id: langMenu
            parent: scene
            property real closedAt: 0
            onClosed: closedAt = Date.now()
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

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// Settings: every option maps 1:1 to a key in ~/.config/selsearch/config.toml.
ApplicationWindow {
    id: win
    required property var settings
    readonly property var cfg: settings.config
    width: 860
    height: 640
    minimumWidth: 640
    minimumHeight: 420
    title: qsTr("Selsearch Settings")
    color: "#121317"
    palette {
        window: "#121317"
        windowText: "#eceef4"
        base: "#1b1c21"
        alternateBase: "#202127"
        text: "#eceef4"
        button: "#23252b"
        buttonText: "#eceef4"
        highlight: "#8e9bff"
        highlightedText: "#14151a"
        placeholderText: "#6f7383"
        mid: "#2c2e35"
        midlight: "#33353d"
        dark: "#0e0f12"
        light: "#3a3c44"
        shadow: "#000000"
        toolTipBase: "#23252b"
        toolTipText: "#eceef4"
    }

    readonly property color fg: "#eceef4"
    readonly property color fgDim: "#9a9eab"
    readonly property color line: Qt.rgba(1, 1, 1, 0.07)
    readonly property color accent: "#8e9bff"
    // Windows: hotkeys are registered in-app, so no compositor snippet.
    readonly property bool isWindows: Qt.platform.os === "windows"
    property int page: 0
    // Bumped on every config change so bindings re-read values.
    property int rev: 0
    Connections {
        target: win.cfg
        function onChanged(key) { win.rev++ }
        function onReloaded() { win.rev++ }
    }
    function val(key) { win.rev; return win.cfg.value(key) }
    function set(key, v) { win.cfg.setValue(key, v) }

    readonly property var pages: [
        { name: qsTr("Appearance"), icon: "sun" },
        { name: qsTr("Lens"), icon: "scan" },
        { name: qsTr("Search"), icon: "search" },
        { name: qsTr("Text recognition"), icon: "scan-text" },
        { name: qsTr("Translation"), icon: "languages" },
        { name: qsTr("AI"), icon: "sparkles" },
        { name: qsTr("Launcher"), icon: "app-window" },
        { name: qsTr("Shortcuts"), icon: "keyboard" },
        { name: qsTr("About"), icon: "settings" },
    ]

    RowLayout {
        anchors.fill: parent
        spacing: 0

        // ---- navigation ----
        Rectangle {
            Layout.fillHeight: true
            Layout.preferredWidth: 220
            color: "#0e0f12"
            Column {
                anchors.fill: parent
                anchors.topMargin: 18
                spacing: 2
                Text {
                    x: 22
                    text: "Selsearch"
                    color: win.fg
                    font.pixelSize: 18
                    font.weight: Font.DemiBold
                    bottomPadding: 14
                }
                Repeater {
                    model: win.pages
                    delegate: Rectangle {
                        required property var modelData
                        required property int index
                        x: 10
                        width: 200
                        height: 36
                        radius: 10
                        color: win.page === index ? Qt.rgba(1, 1, 1, 0.08) : navHover.hovered ? Qt.rgba(1, 1, 1, 0.04) : "transparent"
                        HoverHandler { id: navHover }
                        TapHandler { onTapped: win.page = parent.index }
                        Row {
                            anchors { left: parent.left; leftMargin: 12; verticalCenter: parent.verticalCenter }
                            spacing: 10
                            Icon { anchors.verticalCenter: parent.verticalCenter; name: modelData.icon; size: 16; color: win.page === index ? win.fg : win.fgDim }
                            Text { anchors.verticalCenter: parent.verticalCenter; text: modelData.name; color: win.page === index ? win.fg : win.fgDim; font.pixelSize: 14 }
                        }
                    }
                }
            }
        }

        // ---- page ----
        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentWidth: availableWidth
            clip: true
            Column {
                width: parent.width
                padding: 28
                spacing: 4
                Text {
                    text: win.settings.welcome ? qsTr("Welcome to Selsearch") : win.pages[win.page].name
                    color: win.fg
                    font.pixelSize: 22
                    font.weight: Font.DemiBold
                    bottomPadding: 14
                }
                Loader {
                    width: parent.width - 56
                    visible: win.settings.welcome
                    active: visible
                    sourceComponent: welcomePage
                }
                Loader {
                    width: parent.width - 56
                    visible: !win.settings.welcome
                    sourceComponent: [appearancePage, lensPage, searchPage, ocrPage, translatePage, aiPage, launcherPage, shortcutsPage, aboutPage][win.page]
                }
            }
        }
    }

    // ================= pages =================

    Component {
        id: welcomePage
        Column {
            spacing: 18
            Text {
                width: parent.width
                wrapMode: Text.Wrap
                color: win.fgDim
                font.pixelSize: 14
                text: qsTr("Freeze your screen, select anything — text, an image, a window — then copy, translate, search or ask about it. Recognition and translation run on this computer; nothing is sent anywhere unless you search the web or use a remote AI.")
            }
            WelcomeStep {
                number: 1
                title: qsTr("Bind the shortcuts")
                body: win.isWindows
                      ? qsTr("Selsearch listens for these keys itself: %1 opens the launcher, %2 searches the screen, %3 selects text. Change them under Shortcuts or in the config file.")
                            .arg(win.val("shortcuts.launcher")).arg(win.val("shortcuts.lens")).arg(win.val("shortcuts.lens_text"))
                      : qsTr("Add these lines to ~/.config/hypr/hyprland.lua (other compositors: see Shortcuts).")
                Column {
                    visible: !win.isWindows
                    width: parent.width
                    spacing: 8
                    Rectangle {
                        width: parent.width
                        height: welcomeSnippet.implicitHeight + 20
                        radius: 10
                        color: "#0b0c0f"
                        border.color: win.line
                        TextEdit {
                            id: welcomeSnippet
                            x: 12; y: 10
                            width: parent.width - 24
                            readOnly: true
                            selectByMouse: true
                            wrapMode: Text.WrapAnywhere
                            font.family: "monospace"
                            font.pixelSize: 12
                            color: "#c9ccd6"
                            text: { win.rev; return win.settings.hyprlandSnippet() }
                        }
                    }
                    Button { text: qsTr("Copy"); onClicked: win.settings.copy(welcomeSnippet.text) }
                }
            }
            WelcomeStep {
                number: 2
                title: qsTr("Better text recognition (optional, ~25 MB)")
                body: win.settings.ocrStatus
                Button {
                    text: win.settings.ocrDownloading ? qsTr("Downloading…") : qsTr("Download PP-OCRv5 models")
                    enabled: !win.settings.ocrDownloading
                    onClicked: win.settings.downloadOcrModels()
                }
            }
            WelcomeStep {
                number: 3
                title: qsTr("AI answers (optional)")
                body: qsTr("Point Selsearch at a local model server, e.g. install ollama and run: ollama pull qwen3-vl:4b. You can change this any time under AI.")
                Button { text: qsTr("Test connection"); onClicked: win.settings.testAi() }
            }
            Text { visible: win.settings.aiStatus.length > 0; text: win.settings.aiStatus; color: win.fgDim; font.pixelSize: 12 }
            Button {
                text: qsTr("Done")
                highlighted: true
                onClicked: win.settings.finishWelcome()
            }
        }
    }

    component WelcomeStep: Row {
        id: ws
        property int number
        property string title
        property string body
        default property alias content: stepBody.data
        width: parent ? parent.width : 400
        spacing: 14
        Rectangle {
            width: 28; height: 28; radius: 14
            color: Qt.rgba(win.accent.r, win.accent.g, win.accent.b, 0.18)
            Text { anchors.centerIn: parent; text: ws.number; color: win.accent; font.pixelSize: 13; font.weight: Font.DemiBold }
        }
        Column {
            width: ws.width - 42
            spacing: 8
            Text { width: parent.width; text: ws.title; color: win.fg; font.pixelSize: 15; font.weight: Font.Medium; wrapMode: Text.Wrap }
            Text { width: parent.width; text: ws.body; color: win.fgDim; font.pixelSize: 13; wrapMode: Text.Wrap }
            Item { id: stepBody; width: parent.width; height: childrenRect.height }
        }
    }

    Component {
        id: appearancePage
        Column {
            spacing: 0
            ChoiceRow {
                title: qsTr("Lens style"); key: "general.theme"
                subtitle: qsTr("Signal darkens the screen and lights a dot field around your selection. Mono only darkens. Aurora tints with two hues of your accent.")
                options: [{ id: "signal", name: qsTr("Signal") }, { id: "mono", name: qsTr("Mono") }, { id: "aurora", name: qsTr("Aurora") }]
            }
            SettingRow {
                id: accentRow
                title: qsTr("Accent colour")
                subtitle: qsTr("Used for the dot field, text selection and buttons.")
                readonly property string current: win.val("general.accent") || ""
                readonly property var presets: ["", "#7aa2f7", "#5eead4", "#a3e635", "#facc15", "#fb923c", "#f472b6", "#c084fc", "#e5e7eb"]
                Row {
                    spacing: 6
                    Repeater {
                        model: accentRow.presets
                        delegate: Rectangle {
                            required property string modelData
                            readonly property bool on: accentRow.current.toLowerCase() === modelData.toLowerCase()
                            width: 26; height: 26; radius: 13
                            color: modelData === "" ? "#8e9bff" : modelData
                            border.color: on ? "white" : Qt.rgba(1, 1, 1, 0.12)
                            border.width: on ? 2 : 1
                            ToolTip.visible: swHover.hovered
                            ToolTip.text: modelData === "" ? qsTr("Default") : modelData
                            HoverHandler { id: swHover }
                            TapHandler { onTapped: modelData === "" ? win.cfg.reset("general.accent") : win.set("general.accent", modelData) }
                        }
                    }
                    // Custom colour: opens the wheel.
                    Rectangle {
                        readonly property bool custom: accentRow.current !== "" && accentRow.presets.indexOf(accentRow.current.toLowerCase()) < 0
                        width: 26; height: 26; radius: 13
                        color: custom ? accentRow.current : "transparent"
                        border.color: custom ? "white" : Qt.rgba(1, 1, 1, 0.3)
                        border.width: custom ? 2 : 1
                        Icon { visible: !parent.custom; anchors.centerIn: parent; name: "plus"; size: 14; color: win.fg }
                        ToolTip.visible: wheelHover.hovered
                        ToolTip.text: qsTr("Custom colour…")
                        HoverHandler { id: wheelHover }
                        TapHandler {
                            onTapped: {
                                picker.color = accentRow.current !== "" ? accentRow.current : "#8e9bff"
                                picker.open()
                            }
                        }
                    }
                }
            }
            SwitchRow { title: qsTr("Follow the desktop accent colour"); key: "general.follow_portal_accent"
                subtitle: win.isWindows ? qsTr("Uses the accent colour from Windows settings.")
                                        : qsTr("Reads the accent from xdg-desktop-portal when your desktop provides one (GNOME, KDE). Falls back to the colour above.") }
            SwitchRow { title: qsTr("Start with Windows"); key: "general.autostart"; visible: win.isWindows
                subtitle: qsTr("Runs Selsearch in the background after you sign in, so the shortcuts work right away.") }
        }
    }

    Component {
        id: lensPage
        Column {
            ChoiceRow {
                title: qsTr("After selecting"); key: "lens.on_select"
                subtitle: qsTr("What happens as soon as you finish a selection.")
                options: [{ id: "toolbar", name: qsTr("Show actions") }, { id: "search_image", name: qsTr("Search the image") },
                          { id: "search_text", name: qsTr("Search the text") }, { id: "ask", name: qsTr("Ask AI") }]
            }
            SwitchRow { title: qsTr("Recognise text on screen"); key: "lens.recognize_text"
                subtitle: qsTr("Runs locally the moment the lens opens, so you can select words like text.") }
            SwitchRow { title: qsTr("Snap to windows"); key: "lens.snap_to_windows"
                subtitle: qsTr("Hover highlights a window; a click selects it. Uses your compositor's window list (Hyprland, sway).") }
            SwitchRow { title: qsTr("Show hints"); key: "lens.show_hints" }
        }
    }

    Component {
        id: searchPage
        Column {
            ChoiceRow { title: qsTr("Search engine"); key: "search.engine"; options: win.settings.searchEngines; combo: true }
            TextRow {
                visible: win.val("search.engine") === "searxng"
                title: qsTr("SearXNG instance"); key: "search.searxng_url"; placeholder: "https://searx.be"
            }
            TextRow {
                visible: win.val("search.engine") === "custom"
                title: qsTr("Custom search URL"); key: "search.custom_url"; placeholder: "https://example.com/search?q=%s"
                subtitle: qsTr("%s is replaced by the query.")
            }
            ChoiceRow { title: qsTr("Image search"); key: "search.image_engine"; options: win.settings.imageEngines; combo: true
                subtitle: qsTr("The image is only uploaded when you press Search on a selection (or enable it under Lens → After selecting).") }
            SwitchRow { title: qsTr("Suggestions while typing"); key: "search.suggestions"
                subtitle: qsTr("Sends every keystroke typed in the launcher to the search engine. Off by default.") }
            ChoiceRow { title: qsTr("Open results"); key: "search.open_in"
                options: [{ id: "panel", name: qsTr("In a panel") }, { id: "browser", name: qsTr("In your browser") }] }
            SwitchRow { title: qsTr("Keep cookies in the panel"); key: "search.persist_cookies"
                subtitle: qsTr("Stay signed in to your search engine (e.g. Kagi). Off = every panel session starts fresh.") }
        }
    }

    Component {
        id: ocrPage
        Column {
            ChoiceRow { title: qsTr("Engine"); key: "ocr.engine"
                options: [{ id: "auto", name: qsTr("Automatic") }, { id: "paddle", name: "PP-OCRv5" }, { id: "tesseract", name: "Tesseract" }] }
            LanguagesRow {
                title: qsTr("Languages")
                subtitle: qsTr("Languages you read on screen, most used first. None = your system languages plus English.")
                key: "ocr.languages"
            }
            SettingRow {
                title: qsTr("PP-OCRv5 models")
                subtitle: win.settings.ocrStatus
                Column {
                    spacing: 6
                    Button {
                        width: 150
                        text: win.settings.ocrDownloading ? qsTr("Downloading…") : win.settings.ocrInstalled ? qsTr("✓ Installed") : qsTr("Download")
                        enabled: !win.settings.ocrDownloading && !win.settings.ocrInstalled
                        onClicked: win.settings.downloadOcrModels()
                    }
                    Rectangle {
                        visible: win.settings.ocrDownloading
                        width: 150; height: 4; radius: 2
                        color: Qt.rgba(1, 1, 1, 0.1)
                        Rectangle { width: parent.width * win.settings.ocrFraction; height: parent.height; radius: 2; color: win.accent }
                    }
                }
            }
            NumberRow { title: qsTr("Unload models after (seconds)"); key: "ocr.unload_after_s"; from: 0; to: 3600
                subtitle: qsTr("Frees memory when idle. 0 keeps them loaded for the fastest lens.") }
        }
    }

    Component {
        id: translatePage
        Column {
            ChoiceRow { title: qsTr("Translator"); key: "translate.backend"
                options: [{ id: "auto", name: qsTr("Automatic") }, { id: "web", name: qsTr("Web page (Google Translate / DeepL)") },
                          { id: "bergamot", name: qsTr("Firefox Translations (translateLocally)") },
                          { id: "libretranslate", name: "LibreTranslate" }, { id: "llm", name: qsTr("AI model") }]
                combo: true
                subtitle: qsTr("Automatic translates in place with translateLocally or a LibreTranslate server when available, otherwise opens the web translator below. The AI model is only used if you pick it.") }
            ChoiceRow { title: qsTr("Web translator"); key: "translate.web_service"
                options: [{ id: "google", name: "Google Translate" }, { id: "deepl", name: "DeepL" }]
                subtitle: qsTr("Opens with the selected text. This sends the text to that site.") }
            ChoiceRow { title: qsTr("Translate into"); key: "translate.target"; combo: true
                options: [{ id: "", name: qsTr("System language") }].concat(win.settings.languages.map(l => ({ id: l.code, name: l.name }))) }
            TextRow { title: qsTr("LibreTranslate URL"); key: "translate.libretranslate_url"; placeholder: "http://localhost:5000" }
            TextRow { title: qsTr("LibreTranslate API key"); key: "translate.libretranslate_key"; secret: true }
        }
    }

    Component {
        id: aiPage
        Column {
            SwitchRow { title: qsTr("Enable AI"); key: "ai.enabled" }
            TextRow { title: qsTr("Endpoint"); key: "ai.endpoint"; placeholder: "http://localhost:11434/v1"
                subtitle: qsTr("Any OpenAI-compatible API: ollama, llama.cpp, LM Studio, vLLM… A non-local address means your questions and images leave this computer.") }
            SettingRow {
                title: qsTr("Model")
                subtitle: win.settings.aiStatus
                RowLayout {
                    spacing: 8
                    ComboBox {
                        id: modelBox
                        Layout.preferredWidth: 240
                        editable: true
                        model: win.settings.aiModels.length > 0 ? win.settings.aiModels : [win.val("ai.model")]
                        editText: win.val("ai.model")
                        onAccepted: win.set("ai.model", editText)
                        onActivated: win.set("ai.model", currentText)
                    }
                    Button { text: qsTr("Test"); onClicked: win.settings.testAi() }
                }
            }
            TextRow { title: qsTr("API key"); key: "ai.api_key"; secret: true
                subtitle: qsTr("Only for hosted APIs. Prefer the environment variable below so the key isn't stored in the config file.") }
            TextRow { title: qsTr("API key environment variable"); key: "ai.api_key_env"; placeholder: "OPENAI_API_KEY" }
            SwitchRow { title: qsTr("Keep chat history"); key: "ai.history"
                subtitle: qsTr("Stored only on this computer, in ~/.local/share/selsearch/history.") }
        }
    }

    Component {
        id: launcherPage
        Column {
            SwitchRow { title: qsTr("Apps"); key: "launcher.apps" }
            SwitchRow { title: qsTr("Files"); key: "launcher.files"; subtitle: qsTr("File names are indexed locally and never leave this computer.") }
            TextRow { title: qsTr("Folders to search"); key: "launcher.file_roots"; list: true; placeholder: "~" }
            SwitchRow { title: qsTr("Calculator and unit conversion"); key: "launcher.calculator" }
            NumberRow { title: qsTr("Maximum results"); key: "launcher.max_results"; from: 3; to: 30 }
        }
    }

    Component {
        id: shortcutsPage
        Column {
            spacing: 0
            readonly property string mode: win.settings.shortcutMode
            Text {
                width: parent.width
                bottomPadding: 12
                wrapMode: Text.Wrap
                color: win.fgDim
                font.pixelSize: 13
                text: parent.mode === "inapp" ? qsTr("Click a shortcut and press the new keys. It works immediately.")
                    : parent.mode === "portal" ? qsTr("Click a shortcut and press the new keys. Your desktop asks once to confirm them, and you can also change them in its keyboard settings.")
                    : qsTr("Click a shortcut and press the new keys. On this desktop the compositor owns global keys, so paste the snippet below into its config (it updates as you change keys).")
            }
            ShortcutRow { title: qsTr("Open the launcher"); key: "shortcuts.launcher" }
            ShortcutRow { title: qsTr("Search the screen"); key: "shortcuts.lens" }
            ShortcutRow { title: qsTr("Select text on screen"); key: "shortcuts.lens_text" }

            Item { width: 1; height: 18 }
            Column {
                visible: parent.mode !== "inapp" && parent.mode !== "portal"
                width: parent.width
                spacing: 10
                Text {
                    color: win.fg
                    font.pixelSize: 14
                    text: parent.parent.mode === "sway" ? qsTr("sway config") : qsTr("Hyprland config (~/.config/hypr/hyprland.lua)")
                }
                Rectangle {
                    width: parent.width
                    height: snippet.implicitHeight + 24
                    radius: 12
                    color: "#0b0c0f"
                    border.color: win.line
                    TextEdit {
                        id: snippet
                        x: 14; y: 12
                        width: parent.width - 28
                        readOnly: true
                        selectByMouse: true
                        wrapMode: Text.WrapAnywhere
                        font.family: "monospace"
                        font.pixelSize: 12
                        color: "#c9ccd6"
                        text: { win.rev; return parent.parent.parent.mode === "sway" ? win.settings.swaySnippet() : win.settings.hyprlandSnippet() }
                    }
                }
                Button { text: qsTr("Copy"); onClicked: win.settings.copy(snippet.text) }
                Text {
                    width: parent.width
                    wrapMode: Text.Wrap
                    color: win.fgDim
                    font.pixelSize: 12
                    text: qsTr("Other compositors (niri, river, …): bind the same keys to `selsearch launcher`, `selsearch lens` and `selsearch lens --text`.")
                }
            }
        }
    }

    // A shortcut with a key recorder: click, press keys; Esc cancels,
    // Backspace restores the default.
    component ShortcutRow: SettingRow {
        id: scr
        property string key
        property bool recording: false
        readonly property string error: win.settings.shortcutErrors[key.split(".")[1]] || ""
        subtitle: recording ? qsTr("Press the new keys… (Esc to cancel, Backspace for default)")
                  : error ? qsTr("Not active: %1").arg(error) : ""
        // Windows: keys come from a system keyboard hook, so Alt and Win
        // combinations reach us instead of opening menus.
        onRecordingChanged: if (win.settings.nativeKeyCapture) win.settings.setKeyCapture(recording)
        function finish(sequence) {
            if (sequence === "Esc") { recording = false; return }
            if (sequence === "Backspace") { win.cfg.reset(key); recording = false; return }
            win.set(key, sequence)
            recording = false
        }
        Connections {
            target: win.settings
            enabled: scr.recording && win.settings.nativeKeyCapture
            function onKeysCaptured(sequence) { scr.finish(sequence) }
        }
        Rectangle {
            id: keyBox
            width: 220
            height: 36
            radius: 10
            color: scr.recording ? Qt.rgba(win.accent.r, win.accent.g, win.accent.b, 0.16) : Qt.rgba(1, 1, 1, 0.05)
            border.color: scr.recording ? win.accent : win.line
            Behavior on color { ColorAnimation { duration: 120 } }
            Text {
                anchors.centerIn: parent
                text: scr.recording ? qsTr("Recording…") : (win.settings.displayKeys(win.val(scr.key)) || qsTr("Not set"))
                color: win.fg
                font.pixelSize: 13
                font.family: scr.recording ? "" : "monospace"
            }
            TapHandler { onTapped: { scr.recording = true; recorder.forceActiveFocus() } }
            Item {
                id: recorder
                focus: scr.recording
                onActiveFocusChanged: if (!activeFocus) scr.recording = false
                Keys.onPressed: (event) => {
                    if (!scr.recording) return
                    event.accepted = true
                    if (win.settings.nativeKeyCapture) return
                    const mods = [Qt.Key_Control, Qt.Key_Shift, Qt.Key_Alt, Qt.Key_Meta, Qt.Key_Super_L, Qt.Key_Super_R, Qt.Key_AltGr]
                    if (mods.indexOf(event.key) >= 0) return   // wait for the actual key
                    scr.finish(win.settings.sequenceFor(event.key, event.modifiers))
                }
            }
        }
    }

    Component {
        id: aboutPage
        Column {
            spacing: 10
            Text { text: "Selsearch " + win.settings.version; color: win.fg; font.pixelSize: 15 }
            Text {
                width: parent.width
                wrapMode: Text.Wrap
                color: win.fgDim
                font.pixelSize: 13
                text: qsTr("Screen search, text recognition, translation and a launcher that work on your computer. Nothing is sent anywhere unless you search the web or use a remote AI endpoint. No telemetry.")
            }
            Text { text: qsTr("Config file: %1").arg(win.settings.configPath); color: win.fgDim; font.pixelSize: 12 }
            Button { text: qsTr("Open config file"); onClicked: win.settings.openConfigFile() }
        }
    }

    // ================= row components =================

    component SettingRow: Item {
        id: sr
        property string title
        property string subtitle
        default property alias control: holder.data
        width: parent ? parent.width : 400
        height: Math.max(56, textCol.implicitHeight + 24)
        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: win.line }
        Column {
            id: textCol
            anchors { left: parent.left; right: holder.left; rightMargin: 20; verticalCenter: parent.verticalCenter }
            spacing: 3
            Text { width: parent.width; text: sr.title; color: win.fg; font.pixelSize: 14; wrapMode: Text.Wrap }
            Text { width: parent.width; visible: sr.subtitle.length > 0; text: sr.subtitle; color: win.fgDim; font.pixelSize: 12; wrapMode: Text.Wrap }
        }
        Item {
            id: holder
            anchors { right: parent.right; verticalCenter: parent.verticalCenter }
            width: childrenRect.width
            height: childrenRect.height
        }
    }

    component SwitchRow: SettingRow {
        id: sw
        property string key
        Switch {
            id: toggle
            checked: !!win.val(sw.key)
            onToggled: win.set(sw.key, checked)
            indicator: Rectangle {
                implicitWidth: 44
                implicitHeight: 24
                x: toggle.leftPadding
                y: (toggle.height - height) / 2
                radius: 12
                color: toggle.checked ? win.accent : "#2c2e35"
                Behavior on color { ColorAnimation { duration: 120 } }
                Rectangle {
                    x: toggle.checked ? parent.width - width - 3 : 3
                    y: 3
                    width: 18; height: 18; radius: 9
                    color: toggle.checked ? "#14151a" : "#9a9eab"
                    Behavior on x { NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }
                }
            }
        }
    }

    component TextRow: SettingRow {
        id: tr
        property string key
        property string placeholder
        property bool list: false
        property bool secret: false
        Text {
            id: savedMark
            anchors.right: field.left
            anchors.rightMargin: 8
            anchors.verticalCenter: field.verticalCenter
            text: qsTr("Saved")
            color: win.accent
            font.pixelSize: 12
            opacity: 0
            Behavior on opacity { NumberAnimation { duration: 200 } }
            Timer { id: savedTimer; interval: 1200; onTriggered: savedMark.opacity = 0 }
        }
        TextField {
            id: field
            width: 260
            placeholderText: tr.placeholder
            echoMode: tr.secret ? TextInput.Password : TextInput.Normal
            text: { const v = win.val(tr.key); return tr.list ? (v || []).join(", ") : (v || "") }
            onEditingFinished: {
                const v = tr.list ? text.split(",").map(s => s.trim()).filter(s => s.length > 0) : text.trim()
                const old = win.val(tr.key)
                if (JSON.stringify(old || (tr.list ? [] : "")) === JSON.stringify(v)) return
                win.set(tr.key, v)
                savedMark.opacity = 1
                savedTimer.restart()
            }
        }
    }

    // Chips for a list of language codes with an "Add" dropdown.
    component LanguagesRow: SettingRow {
        id: lr
        property string key
        readonly property var codes: win.val(lr.key) || []
        function nameOf(code) {
            for (const l of win.settings.languages) if (l.code === code) return l.name.split(" · ")[0]
            return code
        }
        Column {
            spacing: 8
            width: 300
            Flow {
                width: parent.width
                spacing: 6
                Repeater {
                    model: lr.codes
                    delegate: Rectangle {
                        required property string modelData
                        required property int index
                        height: 28
                        width: chipRow.implicitWidth + 16
                        radius: 14
                        color: Qt.rgba(1, 1, 1, 0.07)
                        border.color: Qt.rgba(1, 1, 1, 0.1)
                        Row {
                            id: chipRow
                            anchors.centerIn: parent
                            spacing: 6
                            Text { anchors.verticalCenter: parent.verticalCenter; text: lr.nameOf(modelData); color: win.fg; font.pixelSize: 13 }
                            Icon {
                                anchors.verticalCenter: parent.verticalCenter
                                name: "x"; size: 12; color: win.fgDim
                                TapHandler { onTapped: { const l = lr.codes.slice(); l.splice(index, 1); win.set(lr.key, l) } }
                            }
                        }
                    }
                }
                Text {
                    visible: lr.codes.length === 0
                    height: 28
                    verticalAlignment: Text.AlignVCenter
                    text: qsTr("System languages")
                    color: win.fgDim
                    font.pixelSize: 13
                    font.italic: true
                }
            }
            ComboBox {
                width: parent.width
                textRole: "name"
                valueRole: "code"
                displayText: qsTr("Add a language…")
                model: win.settings.languages.filter(l => lr.codes.indexOf(l.code) < 0)
                onActivated: {
                    const l = lr.codes.slice()
                    l.push(currentValue)
                    win.set(lr.key, l)
                }
            }
        }
    }

    ColorPicker {
        id: picker
        parent: Overlay.overlay
        anchors.centerIn: parent
        onAccepted: (c) => win.set("general.accent", c.toString())
    }

    component NumberRow: SettingRow {
        id: nr
        property string key
        property int from: 0
        property int to: 100
        SpinBox {
            from: nr.from
            to: nr.to
            editable: true
            value: win.val(nr.key)
            onValueModified: win.set(nr.key, value)
        }
    }

    component ChoiceRow: SettingRow {
        id: cr
        property string key
        property var options: []
        property bool combo: false
        ComboBox {
            visible: cr.combo
            width: 260
            textRole: "name"
            valueRole: "id"
            model: cr.options
            currentIndex: { const v = win.val(cr.key); for (let i = 0; i < cr.options.length; ++i) if (cr.options[i].id === v) return i; return 0 }
            onActivated: win.set(cr.key, currentValue)
        }
        Row {
            visible: !cr.combo
            spacing: 4
            Repeater {
                model: cr.combo ? [] : cr.options
                delegate: Rectangle {
                    required property var modelData
                    readonly property bool on: win.val(cr.key) === modelData.id
                    height: 32
                    width: chText.implicitWidth + 24
                    radius: 16
                    color: on ? win.accent : chHover.hovered ? Qt.rgba(1, 1, 1, 0.08) : Qt.rgba(1, 1, 1, 0.04)
                    border.color: on ? "transparent" : win.line
                    HoverHandler { id: chHover }
                    TapHandler { onTapped: win.set(cr.key, parent.modelData.id) }
                    Text { id: chText; anchors.centerIn: parent; text: parent.modelData.name; color: parent.on ? "#14151a" : win.fg; font.pixelSize: 13 }
                }
            }
        }
    }
}

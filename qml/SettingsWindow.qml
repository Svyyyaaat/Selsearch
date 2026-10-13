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

    readonly property color fg: "#eceef4"
    readonly property color fgDim: "#9a9eab"
    readonly property color line: Qt.rgba(1, 1, 1, 0.07)
    readonly property color accent: "#8e9bff"
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
                    text: win.pages[win.page].name
                    color: win.fg
                    font.pixelSize: 22
                    font.weight: Font.DemiBold
                    bottomPadding: 14
                }
                Loader {
                    width: parent.width - 56
                    sourceComponent: [appearancePage, lensPage, searchPage, ocrPage, translatePage, aiPage, launcherPage, shortcutsPage, aboutPage][win.page]
                }
            }
        }
    }

    // ================= pages =================

    Component {
        id: appearancePage
        Column {
            spacing: 0
            ChoiceRow {
                title: qsTr("Lens style"); key: "general.theme"
                subtitle: qsTr("Glass blurs and dims everything outside the selection. Mono only dims. Aurora tints with two hues of your accent.")
                options: [{ id: "glass", name: qsTr("Glass") }, { id: "mono", name: qsTr("Mono") }, { id: "aurora", name: qsTr("Aurora") }]
            }
            TextRow {
                title: qsTr("Accent colour"); key: "general.accent"
                subtitle: qsTr("A hex colour like #7aa2f7, used for text selection and buttons. Empty = built-in palette.")
                placeholder: "#8e9bff"
            }
            SwitchRow { title: qsTr("Follow the desktop accent colour"); key: "general.follow_portal_accent"
                subtitle: qsTr("Reads the accent from xdg-desktop-portal when your desktop provides one (GNOME, KDE). Falls back to the colour above.") }
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
            TextRow {
                title: qsTr("Languages"); key: "ocr.languages"; list: true
                subtitle: qsTr("Comma-separated codes like ru, en. Empty = your system languages plus English.")
                placeholder: "en"
            }
            SettingRow {
                title: qsTr("PP-OCRv5 models")
                subtitle: win.settings.ocrStatus
                Button {
                    text: win.settings.ocrDownloading ? qsTr("Downloading…") : qsTr("Download")
                    enabled: !win.settings.ocrDownloading
                    onClicked: win.settings.downloadOcrModels()
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
                options: [{ id: "auto", name: qsTr("Automatic") }, { id: "bergamot", name: qsTr("Firefox Translations (translateLocally)") },
                          { id: "libretranslate", name: "LibreTranslate" }, { id: "llm", name: qsTr("AI model") }]
                combo: true
                subtitle: qsTr("Automatic uses translateLocally if installed, then a LibreTranslate server, then your AI model.") }
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
            spacing: 12
            Text {
                width: parent.width
                wrapMode: Text.Wrap
                color: win.fgDim
                font.pixelSize: 13
                text: qsTr("Shortcuts are bound in your compositor, which then runs the tiny `selsearch` command. Add this to ~/.config/hypr/hyprland.lua:")
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
                    text: win.settings.hyprlandSnippet()
                }
            }
            Button { text: qsTr("Copy"); onClicked: win.settings.copy(snippet.text) }
            Text {
                width: parent.width
                wrapMode: Text.Wrap
                color: win.fgDim
                font.pixelSize: 13
                text: qsTr("sway / river / niri: bind the same commands (e.g. bindsym Mod1+space exec selsearch launcher). KDE and GNOME: add custom shortcuts that run them.")
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
            checked: !!win.val(sw.key)
            onToggled: win.set(sw.key, checked)
        }
    }

    component TextRow: SettingRow {
        id: tr
        property string key
        property string placeholder
        property bool list: false
        property bool secret: false
        TextField {
            width: 260
            placeholderText: tr.placeholder
            echoMode: tr.secret ? TextInput.Password : TextInput.Normal
            text: { const v = win.val(tr.key); return tr.list ? (v || []).join(", ") : (v || "") }
            onEditingFinished: {
                if (tr.list) win.set(tr.key, text.split(",").map(s => s.trim()).filter(s => s.length > 0))
                else win.set(tr.key, text.trim())
            }
        }
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

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs as NativeDialogs
import QtQuick.Layouts

DialogLayer {
    id: root
    objectName: "live-translation-dialog"
    key: "translation"
    title: qsTr("实时翻译")
    glyph: "type"
    dialogWidth: 580
    actions: root.styled
        ? [{ label: qsTr("字幕样式") }, { label: qsTr("完成"), primary: true }]
        : [{ label: qsTr("完成"), primary: true }]
    onActionTriggered: label => {
        if (label !== qsTr("字幕样式")) {
            dialogHost.close()
            return
        }
        bodyViewport.contentY = Math.min(styleSection.y,
            Math.max(0, bodyViewport.contentHeight - bodyViewport.height))
    }

    readonly property var translation: veyra.liveTranslation || null
    readonly property bool supported: translation !== null && translation.supported
    // The style only matters while the text is drawn on the picture.
    readonly property bool styled: supported && translation.showOnVideo !== false
    readonly property int provider: translation ? translation.provider : 0
    readonly property int source: translation ? translation.recognitionSource : 0
    readonly property bool screenSource: source === 2
    readonly property int region: translation ? translation.screenRegion : 0
    // Subtitles on the picture already in the language wanted: nothing is translated, they are
    // shown and read as they are. (Speech is told its language as it is heard: never assumed.)
    readonly property bool sameLanguage: screenSource && translation !== null && translation.sourceLanguage !== ""
        && translation.sourceLanguage === translation.targetLanguage
    readonly property rect customRegion: translation ? translation.customRegion : Qt.rect(0.08, 0.70, 0.84, 0.27)

    readonly property bool useVeyraAudio: translation !== null && translation.recognitionSource === 0
    readonly property bool englishOnly: useVeyraAudio && translation.recognitionModel === 0
    readonly property var recognitionSources: [
        {id: "0", label: qsTr("Veyra 音频")},
        {id: "1", label: qsTr("Windows 实时字幕（旧方式）"), disabled: !translation || !translation.windowsCaptionsSupported},
        {id: "2", label: qsTr("画面文字（采集卡 / PS5 字幕）"), disabled: !translation || !translation.screenTextSupported}
    ]
    readonly property var recognitionModels: [
        {id: "0", label: qsTr("Parakeet TDT · 英语")},
        {id: "1", label: qsTr("Whisper Turbo · 多语言")}
    ]
    readonly property var recognitionThreadOptions: [
        {id: "0", label: qsTr("自动")}, {id: "1", label: "1"}, {id: "2", label: "2"}, {id: "3", label: "3"},
        {id: "4", label: "4"}, {id: "5", label: "5"}, {id: "6", label: "6"}, {id: "7", label: "7"}, {id: "8", label: "8"}
    ]
    readonly property var subtitleLineOptions: [{id: "1", label: "1"}, {id: "2", label: "2"}, {id: "3", label: "3"}]
    readonly property int subtitleSize: veyra.preferences.liveSubtitleSize || veyra.preferences.subtitleSize || 22
    readonly property color subtitleColor: veyra.preferences.liveSubtitleColor || "#ffffff"
    readonly property real subtitleShadow: (veyra.preferences.liveSubtitleShadow ?? -1) >= 0
        ? veyra.preferences.liveSubtitleShadow : ((veyra.preferences.subtitleOutline ?? 2) > 0 ? 1.8 : 0)
    readonly property int subtitleLines: Math.min(3, Math.max(1,
        veyra.preferences.liveSubtitleLines || (veyra.preferences.subtitleLines ?? 2)))
    NativeDialogs.ColorDialog {
        id: captionColor
        title: qsTr("选择字幕颜色")
        onAccepted: veyra.setPreference("liveSubtitleColor",
            Qt.rgba(selectedColor.r, selectedColor.g, selectedColor.b, 1).toString())
    }
    readonly property var regions: [
        {id: "0", label: qsTr("底部字幕")},
        {id: "1", label: qsTr("底部整行")},
        {id: "2", label: qsTr("顶部字幕")},
        {id: "3", label: qsTr("整个画面")},
        {id: "4", label: qsTr("自定义区域")},
        {id: "5", label: "GTA VI"}
    ]
    function setRegion(x, y, w, h) {
        if (root.translation) root.translation.customRegion = Qt.rect(x, y, w, h)
    }
    readonly property var providers: [
        {id: "0", label: qsTr("本地模型")},
        {id: "1", label: qsTr("自定义 API（OpenAI 兼容）")},
        {id: "2", label: "MyMemory"}
    ]
    readonly property var languages: [
        {id: "zh-CN", label: qsTr("简体中文")}, {id: "zh-TW", label: qsTr("繁體中文")},
        {id: "en", label: "English"}, {id: "ja", label: "日本語"}, {id: "ko", label: "한국어"},
        {id: "fr", label: "Français"}, {id: "de", label: "Deutsch"}, {id: "es", label: "Español"},
        {id: "pt-BR", label: "Português (Brasil)"}, {id: "ru", label: "Русский"},
        {id: "it", label: "Italiano"}, {id: "ar", label: "العربية"},
        {id: "th", label: "ไทย"}, {id: "vi", label: "Tiếng Việt"}
    ]
    readonly property var sourceLanguages: provider === 2 ? languages :
        [{id: "", label: qsTr("自动识别原文") }].concat(languages)
    function languageLabel(options, value) {
        return options.find(option => option.id === value)?.label || value
    }

    // Live translation read aloud (TeraTTSv2): voices the translated subtitles.
    readonly property var voiceOver: translation ? translation.voice : null
    // The choices for one speaker: automatic (naming who that is now), muted, then the
    // narrators - men, women, and the one offered by hand only.
    function narratorOptions(row) {
        if (!voiceOver) return []
        const list = voiceOver.narrators
        const automatic = row.fallbackName ? qsTr("自动（%1）").arg(row.fallbackName) : qsTr("自动")
        return [{ id: "auto", label: automatic }, { id: "mute", label: qsTr("静音") }]
            .concat(list.filter(n => !n.female && !n.byHand).map(n => ({ id: n.id, label: "♂ " + n.name })))
            .concat(list.filter(n => n.female && !n.byHand).map(n => ({ id: n.id, label: "♀ " + n.name })))
            .concat(list.filter(n => n.byHand).map(n => ({ id: n.id, label: "♂ " + n.name })))
    }

    DialogSection { text: qsTr("读取字幕") }
    DialogGroup {
        VRow {
            visible: root.translation !== null
            label: qsTr("字幕来源")
            hintBelow: true
            hint: root.screenSource
                ? qsTr("用 Windows 文字识别（OCR）读取画面上的字幕：采集卡、PS5 远程游玩、Xbox/电脑串流或视频。")
                : root.useVeyraAudio ? qsTr("识别播放中的声音，翻译后的字幕显示在 Veyra 预览画面内。")
                                     : qsTr("识别系统播放的声音。")
            VSelect {
                objectName: "live-translation-source"
                implicitWidth: 248
                options: root.recognitionSources
                value: root.languageLabel(options, String(root.source))
                onPicked: id => { root.translation.recognitionSource = Number(id) }
            }
        }
        VRow {
            label: root.screenSource ? qsTr("读取画面上的字幕") : qsTr("实时翻译")
            hintBelow: true
            hint: root.useVeyraAudio
                ? qsTr("识别播放中的声音，翻译后的字幕显示在 Veyra 预览画面内。")
                : root.supported
                ? (root.screenSource
                    ? qsTr("开启后读取画面上的字幕。翻译、在画面上显示和朗读都以它为基础。")
                    : qsTr("使用 Windows 实时字幕识别系统播放的声音，译文显示在 Veyra 预览画面内。"))
                : (root.screenSource ? qsTr("画面文字识别需要 Windows 10 或更新版本")
                                     : qsTr("实时翻译需要 Windows 11 22H2 或更新版本"))
            VSwitch {
                objectName: "live-translation-enabled"
                enabled: root.supported
                checked: root.translation ? root.translation.enabled : false
                onToggled: value => { root.translation.enabled = value }
            }
        }
        VRow {
            visible: root.supported && root.source === 1
            label: qsTr("语音识别")
            hintBelow: true
            hint: qsTr("首次使用请在 Windows 下载语音包，并保持实时字幕窗口打开。")
            VButton {
                objectName: "live-translation-windows"
                text: qsTr("打开系统字幕")
                onClicked: root.translation.openWindowsCaptions()
            }
        }
        // How the picture is read: the neural reader (PP-OCRv5) or Windows' own.
        VRow {
            visible: root.supported && root.screenSource
            label: qsTr("读取方式")
            hintBelow: true
            hint: !root.translation ? ""
                : root.translation.screenReader === 1
                ? qsTr("Windows 文字识别：占用 CPU 少，但字幕没有清晰描边时会漏字。")
                : !root.translation.neuralReadsLanguage
                ? qsTr("神经网络不认识这种文字，改用 Windows 文字识别。")
                : root.translation.neuralInstalled
                ? qsTr("神经网络 PP-OCRv5：直接读画面，不依赖描边，并能分开场景里的文字。")
                : qsTr("神经网络 PP-OCRv5 · 首次下载 %1 MB；下载前用 Windows 文字识别。")
                    .arg(Math.ceil(root.translation.neuralBytes / 1000000))
            RowLayout {
                spacing: 4
                VSelect {
                    objectName: "live-translation-reader"
                    implicitWidth: 196
                    options: [
                        { id: "0", label: qsTr("神经网络（更准确）") },
                        { id: "1", label: qsTr("Windows 文字识别") }
                    ]
                    value: root.translation ? root.languageLabel(options, String(root.translation.screenReader)) : ""
                    onPicked: id => { root.translation.screenReader = Number(id) }
                }
                VButton {
                    visible: root.translation && root.translation.screenReader === 0
                        && (!root.translation.neuralInstalled || root.translation.neuralDownloading)
                    text: root.translation && root.translation.neuralDownloading ? qsTr("取消下载") : qsTr("下载模型")
                    onClicked: {
                        if (root.translation.neuralDownloading) root.translation.cancelNeuralDownload()
                        else root.translation.downloadNeuralReader()
                    }
                }
            }
        }
        ProgressBar {
            Layout.fillWidth: true
            Layout.topMargin: 4
            Layout.bottomMargin: 10
            visible: root.translation !== null && root.translation.neuralDownloading
            value: root.translation ? root.translation.neuralProgress : 0
        }
        VRow {
            visible: root.supported && root.screenSource
            label: qsTr("识别区域")
            hintBelow: true
            hint: root.region === 5
                ? (root.translation && root.translation.screenTextNeuralReady
                    ? qsTr("GTA VI 模板：只读以说话人名字开头的台词；任务、按键提示、菜单、地图和手机都不读。")
                    : qsTr("GTA VI 模板的规则需要神经网络读取方式（请下载）；现在只用它的区域。"))
                : root.region === 0 || root.region === 2
                ? qsTr("只读取这一部分画面，并只保留居中的字幕，忽略两侧的界面文字。")
                : qsTr("只读取这一部分画面。区域越小，识别越快、越准确。")
            VSelect {
                objectName: "live-translation-region"
                implicitWidth: 224
                options: root.regions
                value: root.regions[Math.max(0, Math.min(root.regions.length - 1, root.region))].label
                onPicked: id => { root.translation.screenRegion = Number(id) }
            }
        }
        Text {
            Layout.fillWidth: true
            Layout.bottomMargin: 8
            visible: root.supported && root.screenSource && root.translation !== null && root.translation.linesClipped
            text: qsTr("字幕行超出了区域宽度，两端被截掉。请把区域加宽。")
            color: Theme.warn
            font.family: Theme.fontUi
            font.pixelSize: 12
            wrapMode: Text.Wrap
        }
        // "Find subtitles": watch the picture while people talk and take the band.
        VRow {
            visible: root.supported && root.screenSource
            label: qsTr("自动查找字幕")
            hintBelow: true
            hint: !root.translation ? ""
                : root.translation.findState === 1
                ? qsTr("正在观察画面…还剩 %1 秒").arg(root.translation.findSecondsLeft)
                : root.translation.findState === 2 ? qsTr("已找到字幕位置，区域已设为“自定义”。")
                : root.translation.findState === 3 ? qsTr("没有看到字幕。请在角色说话时再试。")
                : root.translation.findState === 4 ? qsTr("无法查找：神经网络没有加载。")
                : !root.translation.neuralInstalled ? qsTr("需要神经网络读取方式的模型（先下载）。")
                : qsTr("在角色说话时按下：程序观察整个画面 12 秒，找出字幕出现的位置。")
            VButton {
                objectName: "live-translation-find"
                text: qsTr("开始查找")
                enabled: root.translation !== null && root.translation.findState !== 1
                onClicked: root.translation.findSubtitles()
            }
        }
        // What the last read found, so it is visible whether the area holds the subtitles.
        Text {
            objectName: "live-translation-reading"
            Layout.fillWidth: true
            visible: root.supported && root.screenSource && root.translation !== null && root.translation.enabled
            readonly property string reading: root.translation && root.translation.screenReading ? root.translation.screenReading : ""
            readonly property int words: root.translation && root.translation.screenWords ? root.translation.screenWords : 0
            text: reading.length ? qsTr("画面上读到：%1").arg(reading)
                : words > 0 ? qsTr("读到 %1 个词，但不像居中的字幕").arg(words)
                : qsTr("识别区域内暂时没有文字")
            color: reading.length ? Theme.t2 : Theme.t3
            font.family: Theme.fontUi
            font.pixelSize: 12
            wrapMode: Text.Wrap
            maximumLineCount: 3
            elide: Text.ElideRight
            Layout.bottomMargin: 8
        }
        VRow {
            visible: root.supported && root.screenSource && root.region === 4
            label: qsTr("在画面上框选")
            hintBelow: true
            hint: qsTr("窗口会关闭：在画面上按住鼠标拖出字幕所在的区域。")
            VButton {
                objectName: "live-translation-region-draw"
                text: qsTr("框选区域")
                onClicked: { if (veyra.beginTranslationRegionDraw()) root.dialogHost.close() }
            }
        }
        VRow {
            visible: root.supported && root.screenSource && root.region === 4
            label: qsTr("区域左边")
            VSlider {
                objectName: "live-translation-region-left"
                implicitWidth: 170
                from: 0; to: 0.95; inputScale: 100
                valueFromModel: true // the model clamps the region; the knob follows it
                value: root.customRegion.x
                onMoved: value => root.setRegion(value, root.customRegion.y, root.customRegion.width, root.customRegion.height)
            }
        }
        VRow {
            visible: root.supported && root.screenSource && root.region === 4
            label: qsTr("区域宽度")
            VSlider {
                implicitWidth: 170
                from: 0.05; to: 1; inputScale: 100
                valueFromModel: true
                value: root.customRegion.width
                onMoved: value => root.setRegion(root.customRegion.x, root.customRegion.y, value, root.customRegion.height)
            }
        }
        VRow {
            visible: root.supported && root.screenSource && root.region === 4
            label: qsTr("区域上边")
            VSlider {
                implicitWidth: 170
                from: 0; to: 0.95; inputScale: 100
                valueFromModel: true // the model clamps the region; the knob follows it
                value: root.customRegion.y
                onMoved: value => root.setRegion(root.customRegion.x, value, root.customRegion.width, root.customRegion.height)
            }
        }
        VRow {
            visible: root.supported && root.screenSource && root.region === 4
            label: qsTr("区域高度")
            VSlider {
                implicitWidth: 170
                from: 0.05; to: 1; inputScale: 100
                valueFromModel: true
                value: root.customRegion.height
                onMoved: value => root.setRegion(root.customRegion.x, root.customRegion.y, root.customRegion.width, value)
            }
        }
        VRow {
            visible: root.supported && root.screenSource
            label: qsTr("字幕里的名字")
            hintBelow: true
            hint: root.translation && root.translation.nameStyle === 2
                ? qsTr("字幕从不写说话人的名字：冒号前的字也照读。")
                : qsTr("“名字：台词”或另一种颜色的名字不读出，用来区分角色。")
            VSelect {
                objectName: "live-translation-name-style"
                implicitWidth: 248
                options: [
                    { id: "0", label: qsTr("名字：台词，或彩色名字") },
                    { id: "1", label: qsTr("名字单独一行在台词上方") },
                    { id: "2", label: qsTr("没有名字") }
                ]
                value: root.translation ? root.languageLabel(options, String(root.translation.nameStyle)) : ""
                onPicked: id => { root.translation.nameStyle = Number(id) }
            }
        }
        VRow {
            visible: root.supported
            label: root.screenSource ? qsTr("原文语言") : qsTr("语音的语言")
            hintBelow: root.screenSource
            hint: root.screenSource ? qsTr("画面文字按此语言识别，“自动识别原文”按英语识别；其他语言需在 Windows 中安装对应语言。") : ""
            VSelect {
                enabled: !root.englishOnly
                options: root.sourceLanguages
                value: root.translation ? root.languageLabel(options, root.translation.sourceLanguage) : ""
                onPicked: id => { root.translation.sourceLanguage = id }
            }
        }
    }

    DialogGroup {
        Layout.topMargin: 8
        visible: root.useVeyraAudio
        VRow {
            label: qsTr("语音识别模型")
            VSelect {
                objectName: "live-translation-recognition-model"
                implicitWidth: 248
                options: root.recognitionModels
                value: root.translation ? root.languageLabel(options, String(root.translation.recognitionModel)) : ""
                enabled: root.translation && !root.translation.recognitionDownloading
                onPicked: id => { root.translation.recognitionModel = Number(id) }
            }
        }
        VRow {
            label: root.translation && root.translation.recognitionInstalled ? qsTr("识别模型已就绪") : qsTr("识别模型按需下载")
            hintBelow: true
            hint: (root.englishOnly
                ? qsTr("NVIDIA Parakeet · 仅识别英语 · CC BY 4.0")
                : qsTr("OpenAI Whisper · MIT · 建议性能较强的 CPU"))
                + qsTr(" · 首次下载 %1 MB").arg(root.translation ? Math.ceil(root.translation.recognitionBytes / 1000000) : 0)
            RowLayout {
                VButton {
                    objectName: "live-translation-recognition-download"
                    text: root.translation && root.translation.recognitionDownloading ? qsTr("取消下载") : qsTr("下载模型")
                    enabled: root.translation && (!root.translation.recognitionInstalled || root.translation.recognitionDownloading)
                    onClicked: {
                        if (root.translation.recognitionDownloading) root.translation.cancelRecognitionDownload()
                        else root.translation.downloadRecognitionModel()
                    }
                }
                VButton {
                    objectName: "live-translation-recognition-delete"
                    visible: root.translation && root.translation.recognitionInstalled
                    enabled: root.translation && !root.translation.enabled && !root.translation.recognitionDownloading
                    text: qsTr("删除模型")
                    onClicked: root.translation.deleteRecognitionModel()
                }
            }
        }
        ProgressBar {
            objectName: "live-translation-recognition-progress"
            Layout.fillWidth: true
            Layout.topMargin: 4
            Layout.bottomMargin: 10
            visible: root.translation !== null && root.translation.recognitionDownloading
            value: root.translation ? root.translation.recognitionProgress : 0
        }
        VRow {
            objectName: "live-translation-recognition-threads"
            label: qsTr("识别线程")
            hintBelow: true
            hint: root.translation && root.translation.recognitionEffectiveThreads > 0
                && root.translation.recognitionEffectiveThreads !== root.translation.recognitionThreads
                ? qsTr("实际使用 %1 个线程").arg(root.translation.recognitionEffectiveThreads) : ""
            VSelect {
                options: root.recognitionThreadOptions
                value: root.translation ? root.languageLabel(options, String(root.translation.recognitionThreads)) : ""
                onPicked: id => { root.translation.recognitionThreads = Number(id) }
            }
        }
        Text {
            objectName: "live-translation-recognition-status"
            Layout.fillWidth: true
            text: root.translation ? root.translation.recognitionStatus : ""
            visible: text.length > 0
            color: Theme.t2
            font.family: Theme.fontUi
            font.pixelSize: Theme.fsBody
            wrapMode: Text.Wrap
        }
    }

    DialogSection { visible: root.supported; text: qsTr("翻译") }
    DialogGroup {
        visible: root.supported
        VRow {
            visible: root.supported
            label: qsTr("翻译成")
            VSelect {
                objectName: "live-translation-target"
                options: root.languages
                value: root.translation ? root.languageLabel(options, root.translation.targetLanguage) : ""
                onPicked: id => { root.translation.targetLanguage = id }
            }
        }
        Text {
            objectName: "live-translation-same-language"
            Layout.fillWidth: true
            visible: root.supported && root.sameLanguage
            text: qsTr("原文和译文是同一种语言：不翻译，字幕原样显示和朗读。")
            color: Theme.t2
            font.family: Theme.fontUi
            font.pixelSize: Theme.fsBody
            wrapMode: Text.Wrap
        }
        VRow {
            visible: root.supported && !root.sameLanguage
            label: qsTr("翻译引擎")
            hintBelow: true
            hint: root.provider === 0
                ? qsTr("在本机运行，字幕文本留在本机。")
                : qsTr("启用后会把识别出的字幕文本发送给所选服务。")
            VSelect {
                objectName: "live-translation-provider"
                implicitWidth: 224
                options: root.providers
                value: root.providers[root.provider].label
                onPicked: id => { root.translation.provider = Number(id) }
            }
        }
    }

    DialogGroup {
        Layout.topMargin: 8
        visible: root.supported && root.provider === 0 && !root.sameLanguage
        VRow {
            label: root.translation && root.translation.modelInstalled ? qsTr("本地模型已就绪") : qsTr("本地模型")
            hintBelow: true
            hint: qsTr("Qwen2.5 1.5B Q4 与显卡/CPU 运行库 · 按需下载 %1 GB")
                .arg(root.translation ? (root.translation.downloadBytes / 1000000000).toFixed(2) : "")
            RowLayout {
                VButton {
                    objectName: "live-translation-download"
                    text: root.translation && root.translation.downloading ? qsTr("取消下载")
                        : root.translation && root.translation.modelInstalled ? qsTr("验证运行库") : qsTr("下载模型")
                    enabled: root.translation !== null && (!root.translation.enabled || root.translation.downloading
                        || !root.translation.modelInstalled)
                    onClicked: {
                        if (root.translation.downloading) root.translation.cancelModelDownload()
                        else root.translation.downloadModel()
                    }
                }
            }
        }
        VRow {
            label: qsTr("用显卡运行本地模型")
            hintBelow: true
            hint: qsTr("用显卡（Vulkan）翻译，每句只需不到一秒；没有可用显卡时自动改用 CPU。")
            VSwitch {
                objectName: "live-translation-gpu"
                checked: root.translation && root.translation.useGpu !== undefined ? root.translation.useGpu : true
                onToggled: value => { root.translation.useGpu = value }
            }
        }
        ProgressBar {
            objectName: "live-translation-progress"
            Layout.fillWidth: true
            Layout.topMargin: 4
            Layout.bottomMargin: 10
            visible: root.translation !== null && root.translation.downloading
            value: root.translation ? root.translation.downloadProgress : 0
        }
    }

    DialogGroup {
        Layout.topMargin: 8
        visible: root.supported && root.provider === 1 && !root.sameLanguage
        VRow {
            label: qsTr("接口地址")
            DialogTextField {
                objectName: "live-translation-api-base"
                implicitWidth: 248
                placeholder: "https://example.com/v1"
                text: root.translation ? root.translation.apiBase : ""
                onTyped: value => { root.translation.apiBase = value }
            }
        }
        VRow {
            label: qsTr("接口模型名")
            DialogTextField {
                implicitWidth: 248
                text: root.translation ? root.translation.apiModel : ""
                onTyped: value => { root.translation.apiModel = value }
            }
        }
        VRow {
            label: "API Key"
            hint: qsTr("仅本次运行有效")
            DialogTextField {
                implicitWidth: 248
                echoMode: TextInput.Password
                onTyped: value => { if (root.translation) root.translation.setApiKey(value) }
            }
        }
    }
    Text {
        objectName: "live-translation-status"
        Layout.fillWidth: true
        Layout.topMargin: 8
        visible: text.length > 0
        text: root.translation ? root.translation.status : ""
        color: Theme.t2
        font.family: Theme.fontUi
        font.pixelSize: Theme.fsBody
        wrapMode: Text.Wrap
    }
    Text {
        Layout.fillWidth: true
        Layout.topMargin: 6
        visible: root.translation && root.translation.enabled &&
            (root.translation.recognitionRtf > 0 || root.translation.averageTranslationMs > 0 || root.translation.skippedSentences > 0)
        text: !root.translation ? "" : qsTr("识别实时倍率 %1 · 翻译平均 %2 ms · 跳过 %3 句")
            .arg(Number(root.translation.recognitionRtf ?? 0).toFixed(2))
            .arg(Math.round(root.translation.averageTranslationMs ?? 0))
            .arg(root.translation.skippedSentences ?? 0)
        color: Theme.t2
        font.family: Theme.fontUi
        font.pixelSize: Theme.fsBody
        wrapMode: Text.Wrap
    }

    DialogSection { visible: root.supported; text: qsTr("在画面上显示") }
    DialogGroup {
        visible: root.supported
        VRow {
            Layout.topMargin: 7
            visible: root.supported
            label: qsTr("在画面上显示实时翻译")
            hintBelow: true
            hint: qsTr("关闭后不在画面上显示文字，只朗读。")
            VSwitch {
                objectName: "live-translation-on-video"
                checked: root.translation ? root.translation.showOnVideo : true
                onToggled: value => { root.translation.showOnVideo = value }
            }
        }
        VRow {
            visible: root.styled && !root.sameLanguage
            label: qsTr("同时显示原文")
            VSwitch {
                checked: root.translation ? root.translation.showOriginal : true
                onToggled: value => { root.translation.showOriginal = value }
            }
        }
    }

    DialogSection { visible: root.supported && root.voiceOver !== null; text: qsTr("朗读") }
    DialogGroup {
        objectName: "live-voice"
        visible: root.supported && root.voiceOver !== null
        VRow {
            Layout.topMargin: 7
            label: qsTr("朗读译文")
            hintBelow: true
            hint: qsTr("用语音读出画面上的字幕或译文，朗读时自动降低原声音量。字幕已是目标语言时直接朗读，不经过翻译。")
            VSwitch {
                objectName: "live-voice-enabled"
                checked: root.voiceOver ? root.voiceOver.enabled : false
                onToggled: value => { root.voiceOver.enabled = value }
            }
        }
        VRow {
            label: root.voiceOver && root.voiceOver.installed ? qsTr("朗读语音已就绪") : qsTr("朗读语音按需下载")
            hintBelow: true
            hint: qsTr("TeraTTSv2 · TeraSpace · Open RAIL-M · 首次下载 %1 MB")
                .arg(root.voiceOver ? Math.ceil(root.voiceOver.downloadBytes / 1000000) : 0)
            RowLayout {
                VButton {
                    objectName: "live-voice-download"
                    visible: root.voiceOver && (!root.voiceOver.installed || root.voiceOver.downloading)
                    text: root.voiceOver && root.voiceOver.downloading ? qsTr("取消下载") : qsTr("下载模型")
                    onClicked: {
                        if (root.voiceOver.downloading) root.voiceOver.cancelDownload()
                        else root.voiceOver.download()
                    }
                }
                VButton {
                    objectName: "live-voice-preview"
                    visible: root.voiceOver && root.voiceOver.installed
                    text: qsTr("试听")
                    onClicked: root.voiceOver.preview()
                }
            }
        }
        ProgressBar {
            objectName: "live-voice-progress"
            Layout.fillWidth: true
            Layout.topMargin: 4
            Layout.bottomMargin: 10
            visible: root.voiceOver !== null && root.voiceOver.downloading
            value: root.voiceOver ? root.voiceOver.downloadProgress : 0
        }
        VRow {
            label: qsTr("字幕里有名字的角色")
            hintBelow: true
            hint: root.voiceOver && root.voiceOver.characterVoices === 1
                ? qsTr("名字不读出；角色按男女由男声和女声两位朗读者读。")
                : root.voiceOver && root.voiceOver.characterVoices === 2
                ? qsTr("名字不读出，也不区分角色：台词像没有名字一样朗读。")
                : qsTr("名字不读出，每个角色有自己的朗读者，下次启动仍记得。")
            VSelect {
                objectName: "live-voice-character-voices"
                implicitWidth: 248
                options: [
                    { id: "0", label: qsTr("每个角色自己的声音") },
                    { id: "1", label: qsTr("只用男声和女声") },
                    { id: "2", label: qsTr("不区分角色") }
                ]
                value: root.voiceOver ? root.languageLabel(options, String(root.voiceOver.characterVoices)) : ""
                onPicked: id => { root.voiceOver.characterVoices = Number(id) }
            }
        }
        VRow {
            label: qsTr("按游戏声音区分说话人")
            hintBelow: true
            hint: qsTr("听游戏里的声音：分出男声和女声，或认出每个说话人并一直用同一个声音读他的台词。适合字幕里没有名字的情况。")
            VSelect {
                objectName: "live-voice-listen"
                implicitWidth: 248
                options: [
                    { id: "0", label: qsTr("关闭", "off") },
                    { id: "1", label: qsTr("区分男女") },
                    { id: "2", label: qsTr("区分每个说话人") }
                ]
                value: root.voiceOver ? root.languageLabel(options, String(root.voiceOver.listenMode)) : ""
                onPicked: id => { root.voiceOver.listenMode = Number(id) }
            }
        }
        VRow {
            visible: root.voiceOver && root.voiceOver.listenMode > 0
            label: root.voiceOver && root.voiceOver.listenInstalled ? qsTr("声音识别模型已就绪") : qsTr("声音识别模型按需下载")
            hintBelow: true
            hint: qsTr("Silero VAD · 性别识别（MIT）· WeSpeaker ResNet221（Apache-2.0）· 首次下载 %1 MB")
                .arg(root.voiceOver ? Math.ceil(root.voiceOver.listenBytes / 1000000) : 0)
            VButton {
                objectName: "live-voice-listen-download"
                visible: root.voiceOver && (!root.voiceOver.listenInstalled || root.voiceOver.listenDownloading)
                text: root.voiceOver && root.voiceOver.listenDownloading ? qsTr("取消下载") : qsTr("下载模型")
                onClicked: {
                    if (root.voiceOver.listenDownloading) root.voiceOver.cancelListeningDownload()
                    else root.voiceOver.downloadListening()
                }
            }
        }
        ProgressBar {
            objectName: "live-voice-listen-progress"
            Layout.fillWidth: true
            Layout.topMargin: 4
            Layout.bottomMargin: 10
            visible: root.voiceOver !== null && root.voiceOver.listenDownloading
            value: root.voiceOver ? root.voiceOver.listenProgress : 0
        }
        VRow {
            label: qsTr("按原声语气朗读")
            hintBelow: true
            hint: qsTr("跟随游戏原声的语气：喊叫时更响、更高、更有活力，平静时更轻。按声音判断需要开启“按游戏声音区分说话人”，否则只看字幕里的“！”。")
            VSwitch {
                objectName: "live-voice-expressive"
                checked: root.voiceOver ? root.voiceOver.expressive : true
                onToggled: value => { root.voiceOver.expressive = value }
            }
        }
        VRow {
            label: qsTr("多人同时说话时重叠朗读")
            hintBelow: true
            hint: qsTr("另一个人在朗读尚未结束时开口，就同时读出他的台词，像原声里一样，不再排队等候。只用于能分辨的人：有名字的角色、认出的声音或男声与女声。")
            VSwitch {
                objectName: "live-voice-overlap"
                checked: root.voiceOver ? root.voiceOver.overlap : true
                onToggled: value => { root.voiceOver.overlap = value }
            }
        }
        // Who has spoken, each with the narrator reading for them.
        VRow {
            label: qsTr("角色的声音")
            hintBelow: true
            hint: root.voiceOver && root.voiceOver.listenMode === 2
                ? qsTr("说话的人会出现在这里：程序按声音认出他们，每人一位朗读者。可以手动更换或静音，铅笔可给朗读者改名。")
                : root.voiceOver && root.voiceOver.listenMode === 1
                ? qsTr("没有名字的台词按原声的男女由两位朗读者读；字幕里有名字的角色出现后各有自己的声音。可以手动更换或静音，铅笔可给朗读者改名。")
                : qsTr("台词由一位朗读者读；字幕里有名字的角色出现后各有自己的声音。可以手动更换或静音，铅笔可给朗读者改名。")
        }
        Repeater {
            objectName: "live-voice-speakers"
            model: root.voiceOver ? root.voiceOver.speakers : []
            delegate: VRow {
                id: speakerRow
                required property var modelData
                property bool renaming: false
                // (No hint: the choice beside it already says "Авто (Диана)", "Диана" or "Без звука".)
                label: (modelData.female === true ? "♀ " : modelData.female === false ? "♂ " : "") + modelData.title
                RowLayout {
                    spacing: 4
                    VSelect {
                        visible: !speakerRow.renaming
                        implicitWidth: 176
                        options: root.narratorOptions(speakerRow.modelData)
                        value: root.languageLabel(options, speakerRow.modelData.choice)
                        onPicked: id => root.voiceOver.setSpeakerVoice(speakerRow.modelData.key, id)
                    }
                    DialogTextField {
                        id: renameField
                        visible: speakerRow.renaming
                        implicitWidth: 176
                        onEdited: text => {
                            if (!speakerRow.renaming) return
                            speakerRow.renaming = false
                            root.voiceOver.renameNarrator(speakerRow.modelData.narrator, text)
                        }
                    }
                    VButton {
                        ghost: true
                        text: "▶"
                        tip: qsTr("试听")
                        visible: speakerRow.modelData.narrator !== "" && !speakerRow.renaming
                        onClicked: root.voiceOver.previewNarrator(speakerRow.modelData.narrator)
                    }
                    VButton {
                        ghost: true
                        text: "✎"
                        tip: qsTr("给这位朗读者改名")
                        visible: speakerRow.modelData.narrator !== ""
                        onClicked: {
                            if (speakerRow.renaming) {
                                speakerRow.renaming = false
                                root.voiceOver.renameNarrator(speakerRow.modelData.narrator, renameField.text)
                            } else {
                                renameField.text = speakerRow.modelData.narratorName
                                speakerRow.renaming = true
                                renameField.focusEditor()
                            }
                        }
                    }
                }
            }
        }
        VRow {
            visible: root.voiceOver && root.voiceOver.perCharacter
            label: qsTr("记住的角色")
            hintBelow: true
            hint: qsTr("角色和他们的声音在下次启动时仍保留。换了游戏可以清除。")
            VButton {
                text: qsTr("忘记所有角色")
                onClicked: root.voiceOver.forgetCharacters()
            }
        }
        // The user's own stress readings, to fix a word the networks get wrong.
        VRow {
            label: qsTr("自定义重音")
            hintBelow: true
            hint: qsTr("在重读元音前加“+”，例如 Тр+евор、зам+ок。优先于自动重音。")
            RowLayout {
                spacing: 4
                DialogTextField {
                    id: stressField
                    implicitWidth: 176
                    placeholder: "Тр+евор"
                    onEdited: text => { if (text.trim().length && root.voiceOver.addStressFix(text)) stressField.text = "" }
                }
                VButton {
                    text: qsTr("添加")
                    onClicked: { if (root.voiceOver.addStressFix(stressField.text)) stressField.text = "" }
                }
            }
        }
        Repeater {
            model: root.voiceOver ? root.voiceOver.stressFixes : []
            delegate: VRow {
                required property string modelData
                label: modelData
                VButton {
                    ghost: true
                    text: "✕"
                    tip: qsTr("删除")
                    onClicked: root.voiceOver.removeStressFix(modelData)
                }
            }
        }
        VRow {
            label: qsTr("朗读语速")
            value: root.voiceOver ? root.voiceOver.speed.toFixed(2) + "×" : ""
            VSlider {
                objectName: "live-voice-speed"
                from: 0.7; to: 1.6; stepSize: 0.05
                value: root.voiceOver ? root.voiceOver.speed : 1
                valueFromModel: true
                onMoved: value => { root.voiceOver.speed = value }
            }
        }
        VRow {
            label: qsTr("朗读音量")
            value: root.voiceOver ? Math.round(root.voiceOver.volume * 100) + "%" : ""
            VSlider {
                objectName: "live-voice-volume"
                inputScale: 100
                from: 0; to: 2; stepSize: 0.05
                value: root.voiceOver ? root.voiceOver.volume : 1
                valueFromModel: true
                onMoved: value => { root.voiceOver.volume = value }
            }
        }
        VRow {
            label: qsTr("朗读时的原声音量")
            hintBelow: true
            hint: qsTr("朗读期间 Veyra 播放的声音降到这个音量，读完后恢复。")
            value: root.voiceOver ? Math.round(root.voiceOver.duck * 100) + "%" : ""
            VSlider {
                objectName: "live-voice-duck"
                inputScale: 100
                from: 0; to: 1; stepSize: 0.05
                value: root.voiceOver ? root.voiceOver.duck : 0.35
                valueFromModel: true
                onMoved: value => { root.voiceOver.duck = value }
            }
        }
        Text {
            objectName: "live-voice-status"
            Layout.fillWidth: true
            text: root.voiceOver ? root.voiceOver.status : ""
            visible: text.length > 0
            color: Theme.t2
            font.family: Theme.fontUi
            font.pixelSize: Theme.fsBody
            wrapMode: Text.Wrap
        }
    }

    DialogSection { id: styleSection; visible: root.styled; text: qsTr("字幕样式") }
    DialogGroup {
        visible: root.styled
        Rectangle {
            objectName: "live-subtitle-preview"
            Layout.fillWidth: true
            implicitHeight: Math.max(92, sample.contentHeight + 22)
            radius: 10
            color: Theme.videoBlack
            border.width: 1
            border.color: Theme.stroke
            clip: true
            Text {
                anchors.centerIn: parent
                anchors.horizontalCenterOffset: root.subtitleShadow
                anchors.verticalCenterOffset: root.subtitleShadow
                visible: root.subtitleShadow > 0
                width: sample.width
                text: sample.text
                font: sample.font
                color: Qt.rgba(0, 0, 0, 0.59)
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
            }
            // A lighter sample background makes both the dark shadow and the
            // foreground colour visible while editing.
            gradient: Gradient {
                GradientStop { position: 0; color: "#6c7685" }
                GradientStop { position: 1; color: "#404b60" }
            }
            Text {
                id: sample
                objectName: "live-subtitle-sample"
                anchors.centerIn: parent
                width: parent.width - 32
                text: qsTr("字幕预览")
                color: root.subtitleColor
                font.family: "Microsoft YaHei UI"
                font.pixelSize: Math.round(root.subtitleSize * 24 / 22)
                style: (veyra.preferences.subtitleOutline ?? 2) > 0 ? Text.Outline : Text.Normal
                styleColor: "#c0000000"
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
            }
        }
        VRow {
            label: qsTr("字号")
            value: root.subtitleSize + " px"
            VSlider {
                objectName: "live-subtitle-size"
                from: 16; to: 56; stepSize: 1
                value: root.subtitleSize
                valueFromModel: true
                onMoved: value => veyra.setPreference("liveSubtitleSize", Math.round(value))
            }
        }
        VRow {
            label: qsTr("文字颜色")
            RowLayout {
                Rectangle {
                    width: 18; height: 18; radius: 4
                    color: root.subtitleColor
                    border.width: 1
                    border.color: Theme.stroke2
                }
                VButton {
                    objectName: "live-subtitle-color"
                    text: root.subtitleColor.toString().toUpperCase()
                    onClicked: {
                        captionColor.selectedColor = root.subtitleColor
                        captionColor.open()
                    }
                }
            }
        }
        VRow {
            label: qsTr("阴影距离")
            value: root.subtitleShadow + " px"
            VSlider {
                objectName: "live-subtitle-shadow"
                from: 0; to: 8; stepSize: 1
                value: root.subtitleShadow
                valueFromModel: true
                onMoved: value => veyra.setPreference("liveSubtitleShadow", Math.round(value))
            }
        }
        VRow {
            label: qsTr("译文行数上限")
            VSelect {
                options: root.subtitleLineOptions
                value: String(root.subtitleLines)
                onPicked: id => veyra.setPreference("liveSubtitleLines", Number(id))
            }
        }
        VButton {
            objectName: "live-subtitle-reset"
            Layout.alignment: Qt.AlignRight
            text: qsTr("恢复默认")
            ghost: true
            onClicked: {
                veyra.setPreference("liveSubtitleSize", 0)
                veyra.setPreference("liveSubtitleColor", "#ffffff")
                veyra.setPreference("liveSubtitleShadow", -1)
                veyra.setPreference("liveSubtitleLines", 0)
            }
        }
    }

}

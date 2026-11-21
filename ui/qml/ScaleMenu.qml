import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The menu of a scale, on a right click on it (time axis, scale of values, of frequencies, of levels, of
// colours), for every plot: its two ends typed (with their unit: "2 ms", "1,5 kHz", "−20 dBV"), back to
// automatic, and the options the page gives for that kind of scale (logarithmic frequencies, decibels).
// A value is applied as soon as it is typed (Entrée or leaving the field); the menu stays open until a
// click outside or Échap.
Popup {
    id: root
    // The plot the scale is on: scaleInfo(key), setScaleRange(key, low, high), setScaleAutomatic(key).
    property var view: null
    // The scale as the plot describes it: { key, kind, title, unit, low, high, automatic, lowLabel,
    // highLabel, autoText, locked, positive }.
    property var info: ({})
    // (scale) => [{ key, text, checked, enabled }]: the options of the page for this scale, or null.
    property var optionsProvider: null
    readonly property var options: optionsProvider && info.kind !== undefined ? optionsProvider(info) : []
    signal optionToggled(string key, bool on)

    // The ends typed, applied together when they make a range.
    property real typedLow: 0
    property real typedHigh: 1
    property string problem: ""

    function openFor(plot, x, y, scale) {
        view = plot
        showScale(scale)
        const p = plot.mapToItem(root.parent, x, y)
        root.x = p.x
        root.y = p.y
        open()
    }
    function showScale(scale) {
        info = scale
        // Not under the fingers: a field being typed in keeps what is typed.
        if (!lowField.editing)
            typedLow = scale.low ?? 0
        if (!highField.editing)
            typedHigh = scale.high ?? 1
        problem = ""
    }
    // What the plot shows now (after a change: once it has been drawn again).
    function refresh() {
        if (!view || info.key === undefined)
            return
        const scale = view.scaleInfo(info.key)
        if (scale.key === undefined) {
            close() // the scale is gone (channels hidden, plot emptied)
            return
        }
        showScale(scale)
    }
    function apply() {
        if (!(typedLow < typedHigh)) {
            problem = qsTr("Le minimum doit être plus petit que le maximum.")
            return
        }
        if (info.positive === true && !(typedLow > 0)) {
            problem = qsTr("Échelle logarithmique : le minimum doit être plus grand que zéro.")
            return
        }
        if (!view.setScaleRange(info.key, typedLow, typedHigh)) {
            problem = qsTr("Cette échelle ne se règle pas ainsi.")
            return
        }
        problem = ""
        refreshLater.restart()
    }

    parent: Overlay.overlay
    margins: 8
    padding: 12
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    background: Rectangle {
        color: Theme.panel
        border.color: Theme.border
        radius: 6
    }
    contentItem: ColumnLayout {
        spacing: 8
        // A change is drawn at the next frame: read the scale again just after (also after a gesture on
        // the plot while the menu is open).
        Timer {
            id: refreshLater
            interval: 120
            onTriggered: root.refresh()
        }
        Connections {
            target: root.view
            enabled: root.visible
            ignoreUnknownSignals: true
            function onViewChanged() { refreshLater.restart() }
            function onScalesChanged() { refreshLater.restart() }
            function onContentChanged() { refreshLater.restart() }
        }
        Label {
            text: root.info.title ?? ""
            color: Theme.text
            font.pixelSize: Theme.bodyText
            font.bold: true
            Layout.maximumWidth: 340
            elide: Text.ElideRight
        }
        Label {
            visible: (root.info.locked ?? "").length > 0
            text: root.info.locked ?? ""
            Layout.maximumWidth: 340
            wrapMode: Text.WordWrap
            color: Theme.textMuted
            font.pixelSize: Theme.smallText
        }
        RowLayout {
            spacing: 10
            enabled: (root.info.locked ?? "").length === 0
            QuantityField {
                id: lowField
                fieldWidth: 150
                label: root.info.lowLabel ?? qsTr("Minimum")
                unit: root.info.unit ?? ""
                value: root.typedLow
                onEdited: (v) => {
                    root.typedLow = v
                    root.apply()
                }
            }
            QuantityField {
                id: highField
                fieldWidth: 150
                label: root.info.highLabel ?? qsTr("Maximum")
                unit: root.info.unit ?? ""
                value: root.typedHigh
                onEdited: (v) => {
                    root.typedHigh = v
                    root.apply()
                }
            }
        }
        Label {
            visible: root.problem.length > 0
            text: root.problem
            Layout.maximumWidth: 340
            wrapMode: Text.WordWrap
            color: Theme.error
            font.pixelSize: Theme.smallText
        }
        Repeater {
            model: root.options
            delegate: CheckBox {
                required property var modelData
                text: modelData.text
                checked: modelData.checked
                enabled: modelData.enabled !== false
                onToggled: {
                    root.optionToggled(modelData.key, checked)
                    refreshLater.restart()
                }
            }
        }
        RowLayout {
            spacing: 8
            Button {
                text: root.info.autoText ?? qsTr("Automatique")
                enabled: root.info.automatic !== true && (root.info.locked ?? "").length === 0
                onClicked: {
                    root.view.setScaleAutomatic(root.info.key)
                    root.problem = ""
                    refreshLater.restart()
                }
            }
            Item { Layout.fillWidth: true }
            Button {
                text: qsTr("Fermer")
                flat: true
                onClicked: root.close()
            }
        }
        Label {
            text: qsTr("Avec l'unité ou un préfixe (2 ms, 1,5 kHz, 500 mV) ; Entrée applique.")
            Layout.maximumWidth: 340
            wrapMode: Text.WordWrap
            color: Theme.textMuted
            font.pixelSize: Theme.smallText - 1
        }
    }
}

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qacq.App

// Import of a file: a WAV file goes in at once; a text file (CSV...) first shows its first lines and what
// was recognised in it (decimal mark, header, time column), which can be corrected: without a time
// column, the sample rate is asked. The reading itself runs on the session's thread (startImport, then
// session.importFinished); `skipped` when nothing was started (dialog cancelled, or refused at once).
Dialog {
    id: root
    required property var session
    property url file
    property var info: ({})
    property bool timeColumn: false
    property bool decimalComma: false
    property real sampleRate: 1000
    property string unit: ""
    // Open for a file, from openFor to Importer or Annuler (not `visible`, still true while it closes).
    property bool asking: false
    signal skipped()

    function start(url, options) {
        if (!root.session.startImport(url, options))
            root.skipped()
    }
    function openFor(url) {
        info = session.inspectImport(url)
        file = url
        if (info.kind === "wav") {
            start(url, {})
            return
        }
        if (!info.ok) {
            // The session tells the error (popup of the window).
            start(url, {})
            return
        }
        timeColumn = info.timeColumn
        decimalComma = info.decimalComma
        unit = ""
        asking = true
        open()
    }

    title: qsTr("Importer « %1 »").arg(info.fileName ?? "")
    modal: true
    anchors.centerIn: Overlay.overlay
    width: Math.min(parent ? parent.width - 80 : 640, 640)
    onAccepted: {
        root.asking = false
        const options = { decimalComma: root.decimalComma, timeColumn: root.timeColumn,
                          timeUnitSeconds: root.info.timeUnitSeconds ?? 1, unit: root.unit }
        if (!root.timeColumn)
            options.sampleRateHz = root.sampleRate
        root.start(root.file, options)
    }
    onRejected: {
        root.asking = false
        root.skipped()
    }

    footer: DialogButtonBox {
        Button {
            text: qsTr("Importer")
            highlighted: true
            DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
        }
        Button {
            text: qsTr("Annuler")
            flat: true
            DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 10

        Label {
            text: qsTr("Début du fichier")
            color: Theme.textMuted
            font.pixelSize: Theme.smallText
        }
        TextArea {
            Layout.fillWidth: true
            readOnly: true
            wrapMode: TextEdit.NoWrap
            font.family: Theme.monospace
            font.pixelSize: Theme.smallText
            text: (root.info.preview ?? []).join("\n")
            background: Rectangle {
                color: Theme.canvas
                border.color: Theme.border
                radius: 4
            }
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: {
                const cols = root.info.columns ?? 0
                const data = root.timeColumn ? Math.max(0, cols - 1) : cols
                let t = data === 1 ? qsTr("1 voie") : qsTr("%1 voies").arg(data)
                t += root.info.header ? qsTr(", noms pris sur la première ligne") : qsTr(", sans ligne de titres")
                return t + "."
            }
            color: Theme.text
            font.pixelSize: Theme.smallText
        }
        CheckBox {
            text: qsTr("La première colonne est le temps")
            checked: root.timeColumn
            onToggled: root.timeColumn = checked
        }
        Label {
            visible: root.timeColumn && (root.info.timeStepSeconds ?? 0) > 0
            Layout.fillWidth: true
            Layout.leftMargin: 32
            wrapMode: Text.WordWrap
            text: qsTr("Pas de %1, soit %2 (en %3)")
                  .arg(Units.format(root.info.timeStepSeconds ?? 0, "s", 4))
                  .arg(Units.format(1 / (root.info.timeStepSeconds || 1), "Hz", 4))
                  .arg((root.info.timeUnitSeconds ?? 1) === 0.001 ? qsTr("millisecondes") : (root.info.timeUnitSeconds ?? 1) === 1e-6 ? qsTr("microsecondes") : qsTr("secondes"))
            color: Theme.textMuted
            font.pixelSize: Theme.smallText
        }
        QuantityField {
            visible: !root.timeColumn
            label: qsTr("Fréquence d'échantillonnage")
            unit: "Hz"
            minimum: 1e-9
            value: root.sampleRate
            onEdited: (v) => root.sampleRate = v
        }
        CheckBox {
            text: qsTr("Virgule décimale (3,14)")
            checked: root.decimalComma
            onToggled: root.decimalComma = checked
        }
        TextValueField {
            label: qsTr("Unité des voies dont le titre n'en donne pas (facultatif)")
            value: root.unit
            placeholder: "V"
            onEdited: (text) => root.unit = text.trim()
        }
    }
}

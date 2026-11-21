import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qacq.App

// Properties of the selected signal: name, timing, channels (name and unit), notes, and where it comes
// from. Every edit is one step of the undo history. The sample rate is the time scale of the signal: the
// samples stay, durations and frequencies follow it; a change by ten or more is confirmed first.
ScrollView {
    id: root
    required property var session
    property var signalId: 0
    property var info: ({})
    readonly property bool valid: info.signalId !== undefined
    readonly property var list: session.signalList

    contentWidth: availableWidth
    clip: true

    function refresh() {
        info = signalId > 0 && list.rowOf(signalId) >= 0 ? list.info(signalId) : ({})
    }
    function setRate(rate) {
        const before = info.sampleRate ?? 1
        if (rate / before >= 10 || rate / before <= 0.1) {
            rateDialog.newRate = rate
            rateDialog.open()
            return
        }
        applyRate(rate)
    }
    function applyRate(rate) {
        list.setTiming(signalId, rate, info.start ?? 0)
        list.endMerge()
    }
    onSignalIdChanged: refresh()
    Component.onCompleted: refresh()
    Connections {
        target: root.list
        function onDataChanged() { root.refresh() }
        function onCountChanged() { root.refresh() }
        function onModelReset() { root.refresh() }
    }

    // A line "name: value" of the description.
    component Fact: RowLayout {
        property string name
        property string value
        Layout.fillWidth: true
        spacing: 8
        Label {
            text: parent.name
            color: Theme.textMuted
            font.pixelSize: Theme.smallText
            Layout.preferredWidth: 120
        }
        Label {
            text: parent.value
            color: Theme.text
            font.pixelSize: Theme.smallText
            Layout.fillWidth: true
            wrapMode: Text.Wrap
        }
    }

    ColumnLayout {
        width: root.availableWidth
        spacing: 10

        Label {
            visible: !root.valid
            Layout.fillWidth: true
            Layout.topMargin: 20
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            text: qsTr("Choisissez un signal dans la liste de gauche pour voir et modifier ses propriétés.")
            color: Theme.textMuted
            font.pixelSize: Theme.smallText
        }

        ColumnLayout {
            visible: root.valid
            Layout.fillWidth: true
            spacing: 10

            TextValueField {
                label: qsTr("Nom")
                value: root.info.name ?? ""
                onEdited: (text) => { if (text.trim().length > 0) root.list.renameSignal(root.signalId, text.trim()) }
            }
            Fact {
                name: qsTr("Origine")
                value: (root.info.originName ?? "") + (root.info.created ? qsTr(", le %1").arg(Qt.formatDateTime(new Date(root.info.created), "dd/MM/yyyy à HH:mm:ss")) : "")
            }
            Fact { name: qsTr("Contenu"); value: root.info.summary ?? "" }
            Fact { name: qsTr("Taille en mémoire"); value: root.info.sizeText ?? "" }
            Fact {
                visible: (root.info.discontinuityCount ?? 0) > 0
                name: qsTr("Ruptures")
                value: qsTr("%1 (échantillons manquants ou reprise de l'enregistrement : pointillés sur le tracé)").arg(root.info.discontinuityCount ?? 0)
            }

            SectionTitle { text: qsTr("Échantillonnage") }
            QuantityField {
                label: qsTr("Fréquence d'échantillonnage (échelle des temps)")
                unit: "Hz"
                value: root.info.sampleRate ?? 1
                minimum: 1e-9
                tip: qsTr("À corriger pour une carte qui ne l'annonce pas (1 Hz par défaut), un CSV sans colonne de temps ou une horloge à recaler. Ce n'est pas un rééchantillonnage.")
                onEdited: (v) => root.setRate(v)
            }
            Label {
                Layout.fillWidth: true
                Layout.topMargin: -4
                wrapMode: Text.WordWrap
                text: qsTr("Ne change pas les échantillons : la durée, l'axe du temps et toutes les fréquences mesurées la suivent.")
                color: Theme.textMuted
                font.pixelSize: Theme.smallText
            }
            QuantityField {
                label: qsTr("Instant du premier échantillon")
                unit: "s"
                value: root.info.start ?? 0
                onEdited: (v) => { root.list.setTiming(root.signalId, root.info.sampleRate ?? 1, v); root.list.endMerge() }
            }

            SectionTitle { text: qsTr("Voies") }
            Repeater {
                model: root.info.channels ?? []
                delegate: RowLayout {
                    id: channelRow
                    required property var modelData
                    required property int index
                    Layout.fillWidth: true
                    spacing: 6
                    Rectangle {
                        Layout.alignment: Qt.AlignBottom
                        Layout.bottomMargin: 12
                        width: 10
                        height: 10
                        radius: 3
                        color: Theme.channelColors[channelRow.index % Theme.channelColors.length]
                    }
                    TextValueField {
                        label: channelRow.index === 0 ? qsTr("Nom") : ""
                        value: channelRow.modelData.name
                        onEdited: (text) => root.list.setChannel(root.signalId, channelRow.index, text, channelRow.modelData.unit)
                    }
                    TextValueField {
                        Layout.fillWidth: false
                        Layout.preferredWidth: 70
                        label: channelRow.index === 0 ? qsTr("Unité") : ""
                        value: channelRow.modelData.unit
                        onEdited: (text) => root.list.setChannel(root.signalId, channelRow.index, channelRow.modelData.name, text.trim())
                    }
                }
            }

            SectionTitle { text: qsTr("Notes") }
            TextArea {
                id: notes
                Layout.fillWidth: true
                Layout.preferredHeight: Math.max(70, implicitHeight)
                wrapMode: TextEdit.Wrap
                selectByMouse: true
                placeholderText: text.length === 0 && !activeFocus ? qsTr("Conditions de la mesure, montage, remarques…") : ""
                font.pixelSize: Theme.smallText
                text: root.info.notes ?? ""
                onActiveFocusChanged: {
                    if (!activeFocus && root.valid && text !== (root.info.notes ?? ""))
                        root.list.setNotes(root.signalId, text)
                }
            }

            SectionTitle {
                visible: provenance.text.length > 2
                text: qsTr("Provenance")
            }
            Label {
                visible: provenance.text.length > 2
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("Réglages de la source et statistiques au moment de l'acquisition (gardés dans le fichier de session).")
                color: Theme.textMuted
                font.pixelSize: Theme.smallText
            }
            TextArea {
                id: provenance
                visible: text.length > 2
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(260, implicitHeight)
                readOnly: true
                selectByMouse: true
                wrapMode: TextEdit.WrapAnywhere
                font.family: Theme.monospace
                font.pixelSize: 10
                color: Theme.textMuted
                text: root.info.provenance !== undefined && root.info.provenance !== null
                      ? JSON.stringify(root.info.provenance, null, 2) : ""
            }
        }

        // A new time scale ten times or more away from the old one: said before it is made.
        Dialog {
            id: rateDialog
            property real newRate: 1
            readonly property real oldRate: root.info.sampleRate ?? 1
            readonly property real factor: newRate / oldRate
            readonly property real frames: root.info.frameCount ?? 0
            // 12,5 ; 1 000 000
            function ratioText(r) {
                return r >= 1000 ? Units.count(Math.round(r)) : Units.compact(r, "", 3)
            }
            title: qsTr("Changer l'échelle des temps ?")
            modal: true
            // A width given: with its row of buttons, the dialog would otherwise loop on its implicit width.
            width: 470
            anchors.centerIn: Overlay.overlay
            onAccepted: root.applyRate(newRate)
            Label {
                width: parent ? parent.width : implicitWidth
                wrapMode: Text.WordWrap
                color: Theme.text
                text: qsTr("La fréquence d'échantillonnage passe de %1 à %2 (%3) : la durée passe de %4 à %5, et toutes les fréquences (axe des spectres, fréquence mesurée, 1/Δt des curseurs) changent d'autant.\n\nLes échantillons ne changent pas : ce n'est pas un rééchantillonnage. Ctrl+Z revient en arrière.")
                      .arg(Units.compact(rateDialog.oldRate, "Hz", 6))
                      .arg(Units.compact(rateDialog.newRate, "Hz", 6))
                      .arg((rateDialog.factor >= 1 ? "× " : "÷ ") + rateDialog.ratioText(rateDialog.factor >= 1 ? rateDialog.factor : 1 / rateDialog.factor))
                      .arg(Units.duration(rateDialog.frames / rateDialog.oldRate))
                      .arg(Units.duration(rateDialog.frames / rateDialog.newRate))
            }
            footer: DialogButtonBox {
                Button {
                    text: qsTr("Changer")
                    highlighted: true
                    DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
                }
                Button {
                    text: qsTr("Annuler")
                    flat: true
                    DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
                }
            }
        }
    }
}

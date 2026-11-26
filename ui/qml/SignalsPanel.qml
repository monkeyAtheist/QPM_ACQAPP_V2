import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Left panel: the signals of the session, the same in every module. A click selects a signal (the
// views and the properties follow it), a double click renames it (F2), the right click offers rename,
// export, its use in the generator (as a waveform, or as the message of a modulated carrier), taking some of
// its channels out, and delete (also the Suppr key). Every change can be undone (Ctrl+Z).
Rectangle {
    id: root
    required property var session
    property var selectedId: 0
    signal signalSelected(var id)
    signal collapseRequested()
    signal importRequested()
    signal exportRequested(var id)
    // Its use in the generator: "wave" (a waveform of a new channel) or "am" (the message of a modulated
    // carrier).
    signal generatorRequested(var id, string use)

    color: Theme.panel

    // Rename the selected signal (tool header, F2).
    function renameSelected() {
        if (root.selectedId > 0 && root.session.signalList.rowOf(root.selectedId) >= 0)
            renameDialog.openFor(root.selectedId, root.session.signalList.info(root.selectedId).name)
    }

    Rectangle {
        anchors.right: parent.right
        width: 1
        height: parent.height
        color: Theme.border
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.gap
        anchors.rightMargin: Theme.gap + 1
        spacing: 8

        RowLayout {
            Layout.fillWidth: true
            Label {
                text: qsTr("Signaux")
                font.pixelSize: Theme.titleText
                font.bold: true
                color: Theme.text
            }
            Label {
                text: root.session.signalList.count > 0 ? String(root.session.signalList.count) : ""
                color: Theme.textMuted
                font.pixelSize: Theme.smallText
                Layout.leftMargin: 4
            }
            Item { Layout.fillWidth: true }
            FoldButton {
                pointsLeft: true
                toolTip: qsTr("Masquer la liste (F9)")
                onClicked: root.collapseRequested()
            }
        }

        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 4
            model: root.session.signalList
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { }

            delegate: ItemDelegate {
                id: entry
                required property var signalId
                required property string name
                required property string origin
                required property string originName
                required property string summary
                required property int discontinuityCount
                required property int channelCount
                required property int index
                readonly property bool selected: signalId === root.selectedId

                width: ListView.view.width
                height: 62
                hoverEnabled: true
                padding: 8
                onClicked: root.signalSelected(signalId)
                onDoubleClicked: renameDialog.openFor(signalId, name)

                TapHandler {
                    acceptedButtons: Qt.RightButton
                    onTapped: {
                        root.signalSelected(entry.signalId)
                        contextMenu.targetId = entry.signalId
                        contextMenu.targetName = entry.name
                        contextMenu.targetChannels = entry.channelCount
                        contextMenu.popup()
                    }
                }

                background: Rectangle {
                    radius: Theme.radius
                    color: entry.selected ? Qt.alpha(Theme.accent, 0.16)
                                          : entry.hovered ? Qt.alpha(Theme.text, 0.06) : "transparent"
                    border.color: entry.selected ? Qt.alpha(Theme.accent, 0.5) : "transparent"
                }
                contentItem: ColumnLayout {
                    spacing: 2
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 6
                        Label {
                            Layout.fillWidth: true
                            text: entry.name
                            elide: Text.ElideRight
                            font.pixelSize: Theme.bodyText
                            font.bold: entry.selected
                            color: Theme.text
                        }
                        Rectangle {
                            radius: 4
                            implicitWidth: originLabel.implicitWidth + 10
                            implicitHeight: originLabel.implicitHeight + 2
                            color: Qt.alpha(Theme.originColor(entry.origin), 0.18)
                            Label {
                                id: originLabel
                                anchors.centerIn: parent
                                text: entry.originName
                                font.pixelSize: 10
                                color: Theme.originColor(entry.origin)
                            }
                        }
                    }
                    Label {
                        Layout.fillWidth: true
                        text: entry.summary
                        elide: Text.ElideRight
                        font.pixelSize: Theme.smallText
                        color: Theme.textMuted
                    }
                    Label {
                        visible: entry.discontinuityCount > 0
                        text: entry.discontinuityCount === 1 ? qsTr("1 rupture") : qsTr("%1 ruptures").arg(entry.discontinuityCount)
                        font.pixelSize: 10
                        color: Theme.warning
                    }
                }
            }

            Label {
                anchors.centerIn: parent
                width: list.width - 20
                visible: list.count === 0
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                text: qsTr("Aucun signal pour l'instant.\n\nImportez un fichier CSV ou WAV (ou déposez-le sur la fenêtre), lancez une acquisition (le simulateur ne demande aucun matériel), ou composez-en un dans le Générateur (Ajouter le met ici).")
                color: Theme.textMuted
                font.pixelSize: Theme.smallText
            }
        }

        // Only while the list is empty: then, Importer… of the top bar (Ctrl+I) or a file dropped on the window.
        Button {
            visible: list.count === 0
            Layout.fillWidth: true
            flat: true
            text: qsTr("Importer un fichier…")
            onClicked: root.importRequested()
        }
    }

    FitMenu {
        id: contextMenu
        property var targetId: 0
        property string targetName: ""
        property int targetChannels: 1
        MenuItem {
            text: qsTr("Renommer…")
            onTriggered: renameDialog.openFor(contextMenu.targetId, contextMenu.targetName)
        }
        MenuItem {
            text: qsTr("Exporter…")
            onTriggered: root.exportRequested(contextMenu.targetId)
        }
        MenuSeparator { }
        MenuItem {
            text: contextMenu.targetChannels > 1 ? qsTr("Utiliser sa première voie dans le Générateur") : qsTr("Utiliser dans le Générateur")
            onTriggered: root.generatorRequested(contextMenu.targetId, "wave")
            ToolTip.visible: hovered
            ToolTip.delay: 500
            ToolTip.text: qsTr("Une voie de plus dans la recette du Générateur, faite de ce signal : pour lui ajouter un bruit, le mêler à d'autres, le lire dans une opération ; sur une page vierge, toute la page, à sa fréquence et pour sa durée")
        }
        MenuItem {
            text: contextMenu.targetChannels > 1 ? qsTr("Moduler une porteuse avec sa première voie (AM, FM…)") : qsTr("Moduler une porteuse (AM, FM…)")
            onTriggered: root.generatorRequested(contextMenu.targetId, "am")
            ToolTip.visible: hovered
            ToolTip.delay: 500
            ToolTip.text: qsTr("Dans le Générateur : ce signal, ramené à 1 V crête, devient le message d'une porteuse modulée en amplitude, dans une voie de plus (sur une page vierge, toute la page) ; la carte de la porteuse propose aussi DBL, BLU, FM et PM. Ses autres voies : Générateur ▸ Moduler ▾")
        }
        MenuSeparator { }
        MenuItem {
            text: qsTr("Supprimer des voies…")
            enabled: contextMenu.targetChannels > 1
            onTriggered: channelsDialog.openFor(contextMenu.targetId)
        }
        MenuItem {
            text: qsTr("Supprimer")
            onTriggered: root.session.signalList.removeSignal(contextMenu.targetId)
        }
    }

    // Some channels of a signal taken out at once: those left ticked stay.
    Dialog {
        id: channelsDialog
        property var targetId: 0
        property string targetName: ""
        property var names: [] // of its channels, with their units
        property var kept: []  // one boolean a channel
        readonly property int keptCount: kept.filter((k) => k === true).length
        readonly property int goneCount: names.length - keptCount
        title: qsTr("Supprimer des voies de « %1 »").arg(targetName)
        modal: true
        width: 420
        anchors.centerIn: Overlay.overlay
        function openFor(id) {
            const info = root.session.signalList.info(id)
            targetId = id
            targetName = info.name ?? ""
            names = (info.channels ?? []).map((c) => (c.unit ?? "").length > 0 ? c.name + " (" + c.unit + ")" : c.name)
            kept = names.map(() => true)
            open()
        }
        function setKept(index, value) {
            const next = kept.slice()
            next[index] = value
            kept = next
        }
        onAccepted: {
            const indices = []
            for (let i = 0; i < kept.length; ++i)
                if (kept[i] === true)
                    indices.push(i)
            root.session.signalList.keepChannels(targetId, indices)
        }
        ColumnLayout {
            width: parent ? parent.width : 380
            spacing: 6
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("Décochez les voies à supprimer : les autres restent, dans leur ordre. Ctrl+Z les rend.")
                color: Theme.textMuted
                font.pixelSize: Theme.smallText
            }
            ScrollView {
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(320, channelColumn.implicitHeight)
                clip: true
                Column {
                    id: channelColumn
                    Repeater {
                        model: channelsDialog.names.length
                        delegate: CheckBox {
                            required property int index
                            text: channelsDialog.names[index] ?? ""
                            checked: channelsDialog.kept[index] === true
                            onToggled: channelsDialog.setKept(index, checked)
                        }
                    }
                }
            }
            Label {
                visible: channelsDialog.keptCount === 0
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("Un signal garde au moins une voie (pour tout enlever : Supprimer le signal).")
                color: Theme.warning
                font.pixelSize: Theme.smallText
            }
        }
        footer: DialogButtonBox {
            Button {
                text: channelsDialog.goneCount <= 0 ? qsTr("Supprimer")
                      : channelsDialog.goneCount === 1 ? qsTr("Supprimer 1 voie") : qsTr("Supprimer %1 voies").arg(channelsDialog.goneCount)
                highlighted: true
                enabled: channelsDialog.keptCount > 0 && channelsDialog.goneCount > 0
                DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
            }
            Button {
                text: qsTr("Annuler")
                flat: true
                DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            }
        }
    }

    Dialog {
        id: renameDialog
        property var targetId: 0
        title: qsTr("Renommer le signal")
        modal: true
        width: 400
        anchors.centerIn: Overlay.overlay
        footer: DialogButtonBox {
            Button {
                text: qsTr("Renommer")
                highlighted: true
                DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
            }
            Button {
                text: qsTr("Annuler")
                flat: true
                DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            }
        }
        function openFor(id, name) {
            targetId = id
            nameField.text = name
            open()
            nameField.selectAll()
            nameField.forceActiveFocus()
        }
        onAccepted: {
            const name = nameField.text.trim()
            if (name.length > 0)
                root.session.signalList.renameSignal(targetId, name)
        }
        TextField {
            id: nameField
            width: parent ? parent.width : 320
            selectByMouse: true
            onAccepted: renameDialog.accept()
        }
    }
}

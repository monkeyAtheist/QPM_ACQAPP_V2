import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Left panel: the signals of the session, the same in every module. A click selects a signal (the
// views and the properties follow it), a double click renames it (F2), the right click offers rename,
// export and delete (also the Suppr key). Every change can be undone (Ctrl+Z).
Rectangle {
    id: root
    required property var session
    property var selectedId: 0
    signal signalSelected(var id)
    signal collapseRequested()
    signal importRequested()
    signal exportRequested(var id)

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
                text: qsTr("Aucun signal pour l'instant.\n\nImportez un fichier CSV ou WAV (ou déposez-le sur la fenêtre), ou lancez une acquisition (le simulateur ne demande aucun matériel).")
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

    Menu {
        id: contextMenu
        property var targetId: 0
        property string targetName: ""
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
            text: qsTr("Supprimer")
            onTriggered: root.session.signalList.removeSignal(contextMenu.targetId)
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

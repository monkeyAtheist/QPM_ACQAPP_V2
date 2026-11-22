import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import QtCore

// The filter designed, as C for a microcontroller: floating point (float, double) or fixed point (Q15, Q31,
// scaled by the sum or by the peak), a name for its functions; what rounding does to it (response, poles,
// computing noise, saturation) and what is too little; the header and the source, to copy or save into a
// folder (name.h and name.c). The choices are remembered from one run to the next.
Dialog {
    id: root
    required property var filters
    required property var appSettings
    signal done(string message)
    signal failed(string message)

    property string format: "float"
    property string scaling: "sum"
    property string codeName: "filtre"
    readonly property var options: ({ format: root.format, scaling: root.scaling, name: root.codeName })
    // Asked for while open only; again when the filter or a choice changes.
    readonly property var code: visible ? filters.code(options) : ({})
    readonly property bool fixed: format === "q15" || format === "q31"
    readonly property bool ok: code.ok === true

    readonly property var formats: [
        { key: "float", name: qsTr("Flottant float"), tip: qsTr("Un microcontrôleur à unité de calcul flottant (Cortex-M4F, M7, ESP32) : le plus simple, 24 bits de précision") },
        { key: "double", name: qsTr("Flottant double"), tip: qsTr("53 bits de précision ; lent sans unité double précision (Cortex-M7 seulement)") },
        { key: "q15", name: qsTr("Virgule fixe Q15"), tip: qsTr("Échantillons et coefficients sur 16 bits, accumulateur de 32 bits (ou 64 s'il le faut) : Cortex-M0/M3, dsPIC, AVR, MSP430") },
        { key: "q31", name: qsTr("Virgule fixe Q31"), tip: qsTr("Échantillons et coefficients sur 32 bits, accumulateur de 64 bits : la précision d'un flottant sans unité flottante") }
    ]
    readonly property var scalings: [
        { key: "sum", name: qsTr("Somme : rien ne déborde"), tip: qsTr("La somme des |h| de l'entrée à chaque section au plus 0,99 : aucune entrée ne peut faire déborder une section (L1). Un débordement dans une section récursive, même saturé, peut la faire osciller tant que l'entrée reste forte.") },
        { key: "peak", name: qsTr("Crête : sinus pleine échelle"), tip: qsTr("Le gain de l'entrée à chaque section au plus 0,99 à toute fréquence (L∞) : un sinus établi pleine échelle ne déborde pas, quelques dB de bruit en moins ; un transitoire peut déborder.") }
    ]
    function indexOf(list, key) {
        for (let i = 0; i < list.length; ++i) {
            if (list[i].key === key)
                return i
        }
        return 0
    }

    title: qsTr("Code C pour microcontrôleur")
    modal: true
    anchors.centerIn: Overlay.overlay
    width: Math.min(parent ? parent.width - 80 : 980, 980)
    height: Math.min(parent ? parent.height - 80 : 760, 780)
    footer: DialogButtonBox {
        Button {
            text: qsTr("Fermer")
            flat: true
            DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
        }
    }

    Component.onCompleted: {
        root.format = appSettings.value("filters/codeFormat", "float")
        root.scaling = appSettings.value("filters/codeScaling", "sum")
        root.codeName = appSettings.value("filters/codeName", "filtre")
    }
    onFormatChanged: appSettings.setValue("filters/codeFormat", root.format)
    onScalingChanged: appSettings.setValue("filters/codeScaling", root.scaling)
    onCodeNameChanged: appSettings.setValue("filters/codeName", root.codeName)

    ColumnLayout {
        anchors.fill: parent
        spacing: 8

        RowLayout {
            Layout.fillWidth: true
            spacing: 10
            ColumnLayout {
                spacing: 2
                Label {
                    text: qsTr("Nombres")
                    color: Theme.textMuted
                    font.pixelSize: Theme.smallText
                }
                ComboBox {
                    id: formatBox
                    Layout.preferredWidth: 190
                    model: root.formats.map((f) => f.name)
                    currentIndex: root.indexOf(root.formats, root.format)
                    onActivated: (index) => root.format = root.formats[index].key
                    font.pixelSize: Theme.bodyText
                    ToolTip.visible: hovered
                    ToolTip.delay: 500
                    ToolTip.text: (root.formats.find((f) => f.key === root.format) ?? {}).tip ?? ""
                }
            }
            ColumnLayout {
                visible: root.fixed
                spacing: 2
                Label {
                    text: qsTr("Mise à l'échelle")
                    color: Theme.textMuted
                    font.pixelSize: Theme.smallText
                }
                ComboBox {
                    Layout.preferredWidth: 230
                    model: root.scalings.map((s) => s.name)
                    currentIndex: root.indexOf(root.scalings, root.scaling)
                    onActivated: (index) => root.scaling = root.scalings[index].key
                    font.pixelSize: Theme.bodyText
                    ToolTip.visible: hovered
                    ToolTip.delay: 500
                    ToolTip.text: (root.scalings.find((s) => s.key === root.scaling) ?? {}).tip ?? ""
                }
            }
            ColumnLayout {
                spacing: 2
                Label {
                    text: qsTr("Nom (préfixe des fonctions)")
                    color: Theme.textMuted
                    font.pixelSize: Theme.smallText
                }
                TextField {
                    Layout.preferredWidth: 160
                    text: root.codeName
                    selectByMouse: true
                    font.family: Theme.monospace
                    font.pixelSize: Theme.bodyText
                    onEditingFinished: if (text.trim().length > 0) root.codeName = text.trim()
                    ToolTip.visible: hovered
                    ToolTip.delay: 500
                    ToolTip.text: qsTr("Une lettre ou _, puis des lettres, des chiffres ou _ : %1_init, %1_echantillon, %1_bloc, %1_autotest").arg(root.codeName)
                }
            }
            Item { Layout.fillWidth: true }
            Label {
                id: copied
                visible: false
                text: qsTr("Copié")
                color: Theme.success
                font.pixelSize: Theme.smallText
            }
            Button {
                text: qsTr("Copier")
                enabled: root.ok
                ToolTip.visible: hovered
                ToolTip.delay: 500
                ToolTip.text: qsTr("Le fichier affiché (%1) dans le presse-papiers").arg(tabs.currentIndex === 0 ? (root.code.headerName ?? "") : (root.code.sourceName ?? ""))
                onClicked: {
                    if (ImageTools.copyText(tabs.currentIndex === 0 ? root.code.header : root.code.source)) {
                        copied.visible = true
                        copiedTimer.restart()
                    }
                }
            }
            Button {
                text: qsTr("Enregistrer…")
                enabled: root.ok
                highlighted: true
                ToolTip.visible: hovered
                ToolTip.delay: 500
                ToolTip.text: qsTr("%1 et %2 dans un dossier (remplacés s'ils y sont)").arg(root.code.headerName ?? "").arg(root.code.sourceName ?? "")
                onClicked: folderDialog.open()
            }
            Timer {
                id: copiedTimer
                interval: 2000
                onTriggered: copied.visible = false
            }
        }

        // What rounding does, what is too little; why there is no code.
        Label {
            visible: !root.ok
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: root.code.error ?? ""
            color: Theme.error
            font.pixelSize: Theme.bodyText
        }
        ScrollView {
            visible: root.ok
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(notesColumn.implicitHeight, 150)
            clip: true
            ColumnLayout {
                id: notesColumn
                width: root.availableWidth - 20
                spacing: 3
                Repeater {
                    model: root.code.warnings ?? []
                    delegate: Label {
                        required property string modelData
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: "⚠ " + modelData
                        color: Theme.warning
                        font.pixelSize: Theme.smallText
                    }
                }
                Repeater {
                    model: root.code.notes ?? []
                    delegate: Label {
                        required property string modelData
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: modelData
                        color: Theme.textMuted
                        font.pixelSize: Theme.smallText
                    }
                }
            }
        }
        // Each tab as wide as its name: shared out equally, "filtre.h" a pixel wider than "filtre.c" was cut.
        TabBar {
            id: tabs
            visible: root.ok
            TabButton {
                text: root.code.headerName ?? ""
                width: implicitWidth
            }
            TabButton {
                text: root.code.sourceName ?? ""
                width: implicitWidth
            }
        }
        ScrollView {
            visible: root.ok
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            TextArea {
                readOnly: true
                selectByMouse: true
                wrapMode: TextEdit.NoWrap
                font.family: Theme.monospace
                font.pixelSize: 12
                text: tabs.currentIndex === 0 ? (root.code.header ?? "") : (root.code.source ?? "")
                background: Rectangle {
                    color: Theme.canvas
                    border.color: Theme.border
                    radius: 4
                }
            }
        }
    }

    FolderDialog {
        id: folderDialog
        title: qsTr("Dossier où écrire %1 et %2").arg(root.code.headerName ?? "").arg(root.code.sourceName ?? "")
        currentFolder: StandardPaths.writableLocation(StandardPaths.DocumentsLocation)
        onAccepted: {
            const written = root.filters.saveCode(selectedFolder, root.options)
            if (written.length > 0)
                root.done(qsTr("Code C enregistré : %1 et %2").arg(root.code.headerName).arg(written))
            else
                root.failed(root.filters.lastError)
        }
    }
}

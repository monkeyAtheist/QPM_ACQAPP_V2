import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import Qacq.App

// Module "Acquisition": the data arriving, live, like an oscilloscope (rolling window, or trigger
// captures), the state and the figures of the link under it. The tool header starts and stops, sets
// the trigger, records to disk while acquiring and offers an example program for the board. The
// settings of the source are in the right panel. Once stopped, what was received is a signal of the
// session (undoable), shown here and selected in the list.
Item {
    id: root
    required property var session
    required property var acquisition
    required property var serialPorts
    property var signalId: 0
    property Item ribbonHost: null
    // The height its header needs (one row of groups, or more in a narrow window): the host takes it.
    readonly property real ribbonHeight: moduleHeader.implicitHeight
    property bool leftFolded: false
    property bool rightFolded: false
    signal unfoldLeft()
    signal unfoldRight()
    signal firmwareRequested()
    signal settingsRequested()
    signal infoMessage(string text)
    signal errorMessage(string text)

    readonly property bool running: acquisition.running
    readonly property var settings: acquisition.settings
    readonly property var trig: acquisition.trigger
    readonly property string source: settings.source ?? "simulated"
    readonly property var sourceEntry: Choices.sources[Choices.indexOf(Choices.sources, source)]
    readonly property bool boardSource: source === "serial" || source === "tcp" || source === "udp" || source === "simulated"
    readonly property int triggerChannel: trig.channel ?? 0
    readonly property string triggerUnit: acquisition.channelUnits[triggerChannel] ?? ""

    function toggle() {
        if (running)
            acquisition.stop(true)
        else if (!acquisition.start())
            root.settingsRequested()
    }

    Ribbon {
        id: moduleHeader
        parent: root.ribbonHost ? root.ribbonHost : root
        visible: root.visible
        tabs: [qsTr("Acquisition")]
        tabIcons: ["acquire"]

        RibbonGroup {
            title: qsTr("Source")
            RibbonButton {
                id: sourceButton
                glyph: root.sourceEntry.icon
                text: root.sourceEntry.name
                dropDown: true
                enabled: !root.running
                tip: qsTr("D'où viennent les données ; leurs réglages sont à droite")
                onClicked: sourceMenu.popup(sourceButton, 0, sourceButton.height)
                FitMenu {
                    id: sourceMenu
                    Instantiator {
                        model: Choices.sources
                        delegate: MenuItem {
                            required property var modelData
                            text: modelData.name
                            checkable: true
                            checked: modelData.key === root.source
                            onTriggered: {
                                root.acquisition.setSettings({ source: modelData.key })
                                root.settingsRequested()
                            }
                        }
                        onObjectAdded: (index, object) => sourceMenu.insertItem(index, object)
                        onObjectRemoved: (index, object) => sourceMenu.removeItem(object)
                    }
                }
            }
            RibbonButton {
                id: portButton
                visible: root.source === "serial"
                glyph: "port"
                text: (root.settings.serial?.port ?? "").length > 0 ? root.settings.serial.port : qsTr("Port")
                dropDown: true
                enabled: !root.running
                tip: qsTr("Le port série de la carte (la liste suit les branchements)")
                onClicked: portMenu.popup(portButton, 0, portButton.height)
                FitMenu {
                    id: portMenu
                    Instantiator {
                        model: root.serialPorts
                        delegate: MenuItem {
                            required property string label
                            required property string portName
                            text: label
                            checkable: true
                            checked: portName === (root.settings.serial?.port ?? "")
                            onTriggered: root.acquisition.setSettings({ serial: { port: portName } })
                        }
                        onObjectAdded: (index, object) => portMenu.insertItem(index, object)
                        onObjectRemoved: (index, object) => portMenu.removeItem(object)
                    }
                    MenuSeparator { }
                    MenuItem {
                        text: root.serialPorts.count === 0 ? qsTr("Aucun port : actualiser") : qsTr("Actualiser la liste")
                        onTriggered: root.serialPorts.refresh()
                    }
                }
            }
        }

        RibbonGroup {
            title: qsTr("Acquisition")
            RibbonButton {
                glyph: root.running ? "stop" : "play"
                primary: !root.running
                text: root.running ? qsTr("Arrêter") : qsTr("Démarrer")
                tip: root.running ? qsTr("Arrêter et garder ce qui a été reçu comme signal de la session (Espace)")
                                  : qsTr("Démarrer l'acquisition (Espace)")
                onClicked: root.toggle()
            }
            RibbonButton {
                glyph: "discard"
                text: qsTr("Abandonner")
                enabled: root.running
                tip: qsTr("Arrêter sans rien garder")
                onClicked: root.acquisition.stop(false)
            }
            RibbonButton {
                glyph: "clear"
                text: qsTr("Effacer")
                enabled: root.running
                tip: qsTr("Oublier ce qui a été reçu jusqu'ici ; l'acquisition continue")
                onClicked: root.acquisition.clear()
            }
            RibbonButton {
                glyph: "freeze"
                text: qsTr("Figer")
                checked: plot.frozen
                enabled: root.running
                tip: qsTr("Immobiliser le tracé pour le lire ; les données continuent d'arriver")
                onClicked: plot.frozen = !plot.frozen
            }
        }

        RibbonGroup {
            title: qsTr("Déclenchement")
            RibbonButton {
                glyph: "trigger"
                text: qsTr("Déclencher")
                checked: root.trig.enabled ?? false
                tip: qsTr("Comme un oscilloscope : des captures calées sur le passage d'un niveau")
                onClicked: root.acquisition.setTrigger({ enabled: !(root.trig.enabled ?? false) })
            }
            RibbonButton {
                id: edgeButton
                glyph: Choices.edges[Choices.indexOf(Choices.edges, root.trig.edge)].icon
                text: qsTr("Front")
                dropDown: true
                tip: Choices.nameOf(Choices.edges, root.trig.edge)
                onClicked: edgeMenu.popup(edgeButton, 0, edgeButton.height)
                FitMenu {
                    id: edgeMenu
                    Instantiator {
                        model: Choices.edges
                        delegate: MenuItem {
                            required property var modelData
                            text: modelData.name
                            checkable: true
                            checked: modelData.key === root.trig.edge
                            onTriggered: root.acquisition.setTrigger({ edge: modelData.key })
                        }
                        onObjectAdded: (index, object) => edgeMenu.insertItem(index, object)
                        onObjectRemoved: (index, object) => edgeMenu.removeItem(object)
                    }
                }
            }
            RibbonButton {
                id: modeButton
                glyph: "single"
                text: Choices.nameOf(Choices.triggerModes, root.trig.mode)
                dropDown: true
                tip: Choices.triggerModes[Choices.indexOf(Choices.triggerModes, root.trig.mode)].tip
                onClicked: modeMenu.popup(modeButton, 0, modeButton.height)
                FitMenu {
                    id: modeMenu
                    Instantiator {
                        model: Choices.triggerModes
                        delegate: MenuItem {
                            required property var modelData
                            text: modelData.name + " : " + modelData.tip
                            checkable: true
                            checked: modelData.key === root.trig.mode
                            onTriggered: root.acquisition.setTrigger({ mode: modelData.key })
                        }
                        onObjectAdded: (index, object) => modeMenu.insertItem(index, object)
                        onObjectRemoved: (index, object) => modeMenu.removeItem(object)
                    }
                }
            }
            RibbonButton {
                glyph: "rotate"
                text: qsTr("Réarmer")
                visible: root.trig.mode === "single"
                enabled: !(root.trig.armed ?? true)
                tip: qsTr("Attendre la capture suivante")
                onClicked: root.acquisition.armTrigger()
            }
            RibbonButton {
                id: channelButton
                glyph: "signals"
                text: root.acquisition.channelNames[root.triggerChannel] ?? qsTr("Voie 1")
                dropDown: true
                enabled: root.acquisition.channelNames.length > 1
                tip: qsTr("La voie qui déclenche")
                onClicked: channelMenu.popup(channelButton, 0, channelButton.height)
                FitMenu {
                    id: channelMenu
                    Instantiator {
                        model: root.acquisition.channelNames
                        delegate: MenuItem {
                            required property string modelData
                            required property int index
                            text: modelData
                            checkable: true
                            checked: index === root.triggerChannel
                            onTriggered: root.acquisition.setTrigger({ channel: index })
                        }
                        onObjectAdded: (index, object) => channelMenu.insertItem(index, object)
                        onObjectRemoved: (index, object) => channelMenu.removeItem(object)
                    }
                }
            }
            RibbonField {
                anchors.verticalCenter: parent.verticalCenter
                label: qsTr("Niveau")
                fieldWidth: 84
                unit: root.triggerUnit
                value: root.trig.level ?? 0
                tip: qsTr("Niveau de déclenchement, dans l'unité de la voie")
                onEdited: (v) => root.acquisition.setTrigger({ level: v })
            }
            RibbonField {
                anchors.verticalCenter: parent.verticalCenter
                label: root.acquisition.sampleRate > 0 ? qsTr("Durée") : qsTr("Trames")
                fieldWidth: 78
                unit: root.acquisition.sampleRate > 0 ? "s" : ""
                minimum: 0
                value: root.acquisition.sampleRate > 0 ? (root.trig.length ?? 1000) / root.acquisition.sampleRate : (root.trig.length ?? 1000)
                tip: qsTr("Durée d'une capture (en trames tant que la cadence n'est pas connue)")
                onEdited: (v) => root.acquisition.setTrigger({
                    length: Math.max(2, Math.round(root.acquisition.sampleRate > 0 ? v * root.acquisition.sampleRate : v)) })
            }
            RibbonField {
                anchors.verticalCenter: parent.verticalCenter
                label: qsTr("Avant")
                fieldWidth: 58
                unit: "%"
                minimum: 0
                maximum: 100
                value: 100 * (root.trig.preTrigger ?? 0.25)
                tip: qsTr("Part de la capture avant l'instant du déclenchement")
                onEdited: (v) => root.acquisition.setTrigger({ preTrigger: v / 100 })
            }
        }

        RibbonGroup {
            title: qsTr("Enregistrement")
            RibbonButton {
                glyph: "record"
                text: root.acquisition.recording ? qsTr("Arrêter") : qsTr("Sur disque")
                checked: root.acquisition.recording
                enabled: root.running
                tip: root.acquisition.recording ? qsTr("Arrêter l'enregistrement continu (le fichier est complet)")
                                                : qsTr("Écrire en continu dans un fichier CSV ou WAV, en plus de la mémoire")
                onClicked: root.acquisition.recording ? root.acquisition.stopRecording() : recordDialog.open()
            }
        }

        RibbonGroup {
            title: qsTr("Affichage")
            RibbonField {
                anchors.verticalCenter: parent.verticalCenter
                label: qsTr("Fenêtre")
                fieldWidth: 74
                unit: "s"
                minimum: 0.0001
                maximum: 3600
                value: plot.rollSeconds
                tip: root.running ? qsTr("Durée visible en défilement (aussi la molette sur le tracé)")
                                  : qsTr("Durée visible de l'enregistrement : glissez le tracé, la barre dessous ou les flèches ← → pour le parcourir")
                onEdited: (v) => plot.rollSeconds = v
            }
            RibbonButton {
                glyph: "overlay"
                text: qsTr("Superposées")
                checked: !plot.stacked
                tip: qsTr("Les voies sur un même tracé (une échelle par unité, à gauche et à droite), ou une bande chacune ; clic droit sur le tracé : choisir les voies")
                onClicked: plot.stacked = !plot.stacked
            }
        }
        // The same group as in "Signaux"; while the data roll in, the zoom sets the width of the window.
        ViewGroup {
            target: plot
            rolling: root.running && !plot.capture
            fitTip: qsTr("Tout l'enregistrement, échelles automatiques (en direct : échelles automatiques ; double clic sur le tracé)")
        }
        RibbonGroup {
            title: qsTr("Mesurer")
            RibbonButton {
                glyph: "cursors"
                text: qsTr("Curseurs")
                checked: plot.cursorsVisible
                enabled: plot.cursorsAvailable
                tip: plot.cursorsAvailable ? qsTr("Deux curseurs A et B : Δt, 1/Δt, valeurs et statistiques entre eux")
                                           : qsTr("Figez le tracé ou déclenchez pour poser des curseurs")
                onClicked: plot.cursorsVisible = !plot.cursorsVisible
            }
        }
        ImageActions {
            target: plotArea
            baseName: "acquisition"
            onDone: (m) => root.infoMessage(m)
            onFailed: (m) => root.errorMessage(m)
        }

        RibbonGroup {
            title: qsTr("Carte")
            visible: root.boardSource
            RibbonButton {
                glyph: "chip"
                text: qsTr("Programme")
                tip: qsTr("Programme d'exemple (Arduino ou C) qui envoie exactement le format réglé")
                onClicked: root.firmwareRequested()
            }
        }
    }

    FileDialog {
        id: recordDialog
        title: qsTr("Enregistrer en continu dans…")
        fileMode: FileDialog.SaveFile
        nameFilters: [qsTr("Texte CSV (*.csv)"), qsTr("Son WAV (*.wav)")]
        defaultSuffix: "csv"
        onAccepted: {
            const wav = selectedFile.toString().toLowerCase().endsWith(".wav")
            root.acquisition.startRecording(selectedFile, { format: wav ? "wav" : "csv" })
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            Rectangle {
                anchors.fill: parent
                color: Theme.canvas
            }
            // What the "Image" tools take: the plot and its readouts.
            Rectangle {
                id: plotArea
                anchors.fill: parent
                anchors.leftMargin: root.leftFolded ? 22 : 0
                anchors.rightMargin: root.rightFolded ? 22 : 0
                color: Theme.canvas
            }
            SignalPlot {
                id: plot
                parent: plotArea
                anchors.fill: parent
                anchors.bottomMargin: scrollBar.height
                session: root.session
                signalId: root.signalId
                acquisition: root.acquisition
                live: root.running
                backgroundColor: Theme.canvas
                gridColor: Theme.grid
                textColor: Theme.text
                mutedColor: Theme.textMuted
                accentColor: Theme.accent
                panelColor: Theme.panel
                channelColors: Theme.channelColors
                emptyText: qsTr("Choisissez la source dans le panneau de droite (le simulateur ne demande aucun matériel), puis Démarrer.\nUne fois arrêtée, l'acquisition devient un signal de la session.")
                onContextMenuRequested: (x, y) => plotMenu.openAt(plot, x, y)
                onScaleMenuRequested: (x, y, scale) => scaleMenu.openFor(plot, x, y, scale)
            }
            ViewScrollBar {
                id: scrollBar
                parent: plotArea
                view: plot
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.leftMargin: 62
                anchors.rightMargin: 14
                anchors.bottomMargin: 2
                height: implicitHeight
            }
            CursorReadout {
                parent: plotArea
                view: plot
            }
            PlotMenu {
                id: plotMenu
                view: plot
                mode: "channels"
            }
            // Right click on a scale or on the time axis: its ends, automatic.
            ScaleMenu {
                id: scaleMenu
            }
            PlotToolbar {
                view: plot
                anchors.top: parent.top
                anchors.right: parent.right
                anchors.topMargin: 10
                anchors.rightMargin: (root.rightFolded ? 22 : 0) + 18
            }
            Rectangle {
                visible: plot.frozen && root.running
                anchors.top: parent.top
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.topMargin: 10
                radius: 6
                implicitWidth: frozenLabel.implicitWidth + 16
                implicitHeight: 24
                color: Qt.alpha(Theme.accent, 0.2)
                border.color: Theme.accent
                Label {
                    id: frozenLabel
                    anchors.centerIn: parent
                    text: qsTr("Tracé figé : les données continuent d'arriver")
                    color: Theme.text
                    font.pixelSize: Theme.smallText
                }
            }
            EdgeTab {
                visible: root.leftFolded
                atLeft: true
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                toolTip: qsTr("Afficher la liste des signaux (F9)")
                onClicked: root.unfoldLeft()
            }
            EdgeTab {
                visible: root.rightFolded
                atLeft: false
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                toolTip: qsTr("Afficher les réglages (F10)")
                onClicked: root.unfoldRight()
            }
        }

        StatusStrip {
            Layout.fillWidth: true
            acquisition: root.acquisition
        }
    }
}

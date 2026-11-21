import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Module "Signaux": the selected signal of the session at any zoom, with its readout. The tool header
// gathers the files (import, export), the edits of the signal and the view.
Item {
    id: root
    required property var session
    property var signalId: 0
    property Item ribbonHost: null
    property bool leftFolded: false
    property bool rightFolded: false
    signal unfoldLeft()
    signal unfoldRight()
    signal importRequested()
    signal exportRequested()
    signal renameRequested()
    signal infoMessage(string text)
    signal errorMessage(string text)
    // The count is read so that this follows the list (a signal deleted, an undo).
    readonly property bool hasSignal: (session.signalList.count, signalId > 0 && session.signalList.rowOf(signalId) >= 0)

    Ribbon {
        parent: root.ribbonHost ? root.ribbonHost : root
        visible: root.visible
        tabs: [qsTr("Visualiser")]
        RibbonGroup {
            title: qsTr("Fichier")
            RibbonButton {
                glyph: "import"
                text: qsTr("Importer")
                tip: qsTr("Un fichier CSV, texte ou WAV devient un signal de la session (Ctrl+I)")
                onClicked: root.importRequested()
            }
            RibbonButton {
                glyph: "export"
                text: qsTr("Exporter")
                enabled: root.hasSignal
                tip: qsTr("Le signal choisi en CSV ou en WAV (Ctrl+E)")
                onClicked: root.exportRequested()
            }
        }
        RibbonGroup {
            title: qsTr("Signal")
            RibbonButton {
                glyph: "rename"
                text: qsTr("Renommer")
                enabled: root.hasSignal
                tip: qsTr("Aussi par un double clic dans la liste (F2)")
                onClicked: root.renameRequested()
            }
            RibbonButton {
                glyph: "delete"
                text: qsTr("Supprimer")
                enabled: root.hasSignal
                tip: qsTr("Retire le signal de la session (Ctrl+Z le rend)")
                onClicked: root.session.signalList.removeSignal(root.signalId)
            }
        }
        RibbonGroup {
            title: qsTr("Vue")
            RibbonButton {
                glyph: "fit"
                text: qsTr("Tout voir")
                enabled: plot.hasData
                tip: qsTr("Tout le signal, échelles automatiques (double clic sur le tracé)")
                onClicked: plot.showAll()
            }
            RibbonButton {
                glyph: "zoomin"
                text: qsTr("Zoom +")
                enabled: plot.hasData
                tip: qsTr("Deux fois plus court (molette sur le tracé : autour du pointeur)")
                onClicked: plot.zoomTime(0.5)
            }
            RibbonButton {
                glyph: "zoomout"
                text: qsTr("Zoom −")
                enabled: plot.hasData && plot.zoomed
                onClicked: plot.zoomTime(2)
            }
            RibbonButton {
                glyph: "back"
                text: qsTr("Précédente")
                enabled: plot.canGoBack
                tip: qsTr("Revenir à la vue d'avant (zoom, déplacement, tout voir)")
                onClicked: plot.goBack()
            }
        }
        RibbonGroup {
            title: qsTr("Mesurer")
            RibbonButton {
                glyph: "cursors"
                text: qsTr("Curseurs")
                checked: plot.cursorsVisible
                enabled: plot.hasData
                tip: qsTr("Deux curseurs A et B : Δt, 1/Δt, valeurs sous chacun, statistiques et intégrale entre eux")
                onClicked: plot.cursorsVisible = !plot.cursorsVisible
            }
        }
        ImageActions {
            target: plotArea
            onDone: (m) => root.infoMessage(m)
            onFailed: (m) => root.errorMessage(m)
        }
        RibbonGroup {
            title: qsTr("Voies")
            RibbonButton {
                glyph: "overlay"
                text: qsTr("Superposées")
                checked: !plot.stacked
                enabled: plot.hasData
                tip: qsTr("Les voies sur un même tracé (une échelle par unité, à gauche et à droite), ou une bande chacune ; clic droit sur le tracé : choisir les voies")
                onClicked: plot.stacked = !plot.stacked
            }
        }
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.canvas
    }
    // What the "Image" tools take: the plot and its readouts, on the colour of the canvas.
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
        backgroundColor: Theme.canvas
        gridColor: Theme.grid
        textColor: Theme.text
        mutedColor: Theme.textMuted
        accentColor: Theme.accent
        panelColor: Theme.panel
        channelColors: Theme.channelColors
        emptyText: root.session.signalList.count === 0
                   ? qsTr("La session est vide : importez un fichier CSV ou WAV (Ctrl+I), ou passez au module Acquisition.")
                   : qsTr("Choisissez un signal dans la liste de gauche.")
        onContextMenuRequested: (x, y) => plotMenu.openAt(plot, x, y)
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
    PlotToolbar {
        view: plot
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.topMargin: 10
        anchors.rightMargin: (root.rightFolded ? 22 : 0) + 18
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
        toolTip: qsTr("Afficher les propriétés (F10)")
        onClicked: root.unfoldRight()
    }
}

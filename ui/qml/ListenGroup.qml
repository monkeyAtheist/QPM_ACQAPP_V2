import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qacq.App

// Group "Écouter" of the tool header: a signal of a session on the sound card (AudioPlayer, one for the
// whole application: playing something else stops what played). One of its channels in mono, or the first
// two in stereo; normalized or as it is; once or in a loop; a volume; the output (menu Son ▾, remembered).
// Says what plays, or why it does not.
RibbonGroup {
    id: root
    required property var player
    property var session: null
    property var signalId: 0
    property var channels: [] // of the signal, [{name}]
    signal infoMessage(string text)
    signal errorMessage(string text)

    // The channel heard alone, or −1: the first two in stereo.
    property int heard: 0
    readonly property bool stereo: root.heard < 0 && root.channels.length >= 2
    readonly property int single: Math.min(Math.max(root.heard, 0), Math.max(0, root.channels.length - 1))
    // Something plays (from here or elsewhere: the button stops it, wherever one is), and it is this signal.
    readonly property bool sounding: root.player.playing || root.player.preparing
    readonly property bool mine: root.player.source !== null && root.player.source === root.session
                                 && root.player.sourceId === root.signalId
    readonly property bool busy: root.mine && root.sounding
    // This group asked the player (only it then says what plays, or why not); quietly: without saying it.
    property bool asked: false
    property bool quiet: false

    function listen() {
        root.asked = true
        root.quiet = false
        root.player.play(root.session, root.signalId, root.stereo ? [0, 1] : [root.single]) // (a failure is signalled)
    }
    // What plays plays again from the signal of now (a preview regenerated: what was changed is heard), without
    // saying it again.
    function relisten() {
        if (!root.busy || !root.player.playing)
            return
        root.listen()
        root.quiet = true
    }

    title: qsTr("Écouter")

    Connections {
        target: root.player
        function onStarted(text) {
            if (!root.asked)
                return
            root.asked = false
            if (!root.quiet)
                root.infoMessage(text)
        }
        function onFailed(error) {
            if (!root.asked)
                return
            root.asked = false
            root.errorMessage(error)
        }
    }

    RibbonButton {
        glyph: root.sounding ? "stop" : "play"
        text: root.sounding ? qsTr("Arrêter") : qsTr("Écouter")
        primary: root.sounding
        enabled: root.sounding || (root.session !== null && root.signalId > 0 && root.channels.length > 0)
        tip: !root.sounding ? qsTr("Écouter le signal sur la carte son (%1) ; voies, niveau, boucle, volume et sortie : Son ▾")
                              .arg(root.stereo ? qsTr("voies 1 et 2 en stéréo") : qsTr("« %1 » en mono").arg((root.channels[root.single] ?? {}).name ?? ""))
             : root.player.preparing ? qsTr("Préparation du son…")
             : qsTr("Arrêter l'écoute : %1 — %2 / %3").arg(root.player.description)
                   .arg(Units.compact(root.player.position, "s", 3)).arg(Units.compact(root.player.duration, "s", 3))
        onClicked: root.sounding ? root.player.stop() : root.listen()
    }
    RibbonButton {
        id: soundButton
        glyph: "speaker"
        text: qsTr("Son")
        dropDown: true
        tip: qsTr("Les voies écoutées, le niveau, la boucle, le volume et la sortie audio")
        onClicked: {
            root.player.refreshDevices()
            soundMenu.popup(soundButton, 0, soundButton.height)
        }
        FitMenu {
            id: soundMenu
            Instantiator {
                model: root.channels
                delegate: MenuItem {
                    required property var modelData
                    required property int index
                    text: qsTr("%1 en mono").arg(modelData.name ?? "")
                    checkable: true
                    checked: !root.stereo && root.single === index
                    onTriggered: {
                        root.heard = index
                        checked = Qt.binding(() => !root.stereo && root.single === index)
                    }
                }
                onObjectAdded: (index, object) => soundMenu.insertItem(index, object)
                onObjectRemoved: (index, object) => soundMenu.removeItem(object)
            }
            MenuItem {
                text: qsTr("Voies 1 et 2 en stéréo (gauche, droite)")
                enabled: root.channels.length >= 2
                checkable: true
                checked: root.stereo
                onTriggered: {
                    root.heard = -1
                    checked = Qt.binding(() => root.stereo)
                }
            }
            MenuSeparator {}
            MenuItem {
                text: qsTr("Normalisé : la crête à −1 dBFS")
                checkable: true
                checked: root.player.normalized
                onTriggered: {
                    root.player.normalized = true
                    checked = Qt.binding(() => root.player.normalized)
                }
            }
            MenuItem {
                text: qsTr("Tel quel : ±1 à pleine échelle")
                checkable: true
                checked: !root.player.normalized
                onTriggered: {
                    root.player.normalized = false
                    checked = Qt.binding(() => !root.player.normalized)
                }
            }
            MenuSeparator {}
            MenuItem {
                text: qsTr("En boucle")
                checkable: true
                checked: root.player.loop
                onTriggered: {
                    root.player.loop = checked
                    checked = Qt.binding(() => root.player.loop)
                }
            }
            MenuSeparator {}
            Item {
                implicitWidth: 270
                implicitHeight: 40
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 14
                    anchors.rightMargin: 14
                    spacing: 8
                    Label {
                        text: qsTr("Volume")
                        font.pixelSize: Theme.bodyText
                    }
                    Slider {
                        Layout.fillWidth: true
                        from: 0
                        to: 1
                        value: root.player.volume
                        onMoved: root.player.volume = value
                    }
                    Label {
                        Layout.preferredWidth: 38
                        horizontalAlignment: Text.AlignRight
                        text: Math.round(root.player.volume * 100) + " %"
                        color: Theme.textMuted
                        font.pixelSize: Theme.smallText
                    }
                }
            }
            MenuSeparator {}
            FitMenu {
                id: outputMenu
                title: qsTr("Sortie audio")
                MenuItem {
                    text: qsTr("Celle du système")
                    checkable: true
                    checked: root.player.device === ""
                    onTriggered: {
                        root.player.device = ""
                        checked = Qt.binding(() => root.player.device === "")
                    }
                }
                Instantiator {
                    model: root.player.devices
                    delegate: MenuItem {
                        required property string modelData
                        text: modelData
                        checkable: true
                        checked: root.player.device === modelData
                        onTriggered: {
                            root.player.device = modelData
                            checked = Qt.binding(() => root.player.device === modelData)
                        }
                    }
                    onObjectAdded: (index, object) => outputMenu.insertItem(index + 1, object)
                    onObjectRemoved: (index, object) => outputMenu.removeItem(object)
                }
            }
        }
    }
}

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qacq.App

// Small quantity field of the tool header: a name above a slim box, so that it fits the height of the
// buttons. Shows "2 s", accepts "500 ms", "1,5" (read with the prefix shown)... like QuantityField.
// Place it in a group with `anchors.verticalCenter: parent.verticalCenter`.
ColumnLayout {
    id: root
    property string label
    property real value: 0
    property string unit: ""
    property real minimum: -Number.MAX_VALUE
    property real maximum: Number.MAX_VALUE
    property string tip: ""
    property int fieldWidth: 80
    property bool invalid: false
    signal edited(real value)

    spacing: 3

    function shown() {
        return Units.compact(root.value, root.unit, 6)
    }
    function restore() {
        input.text = Qt.binding(function() { return root.shown() })
    }

    Label {
        text: root.label
        color: Theme.textMuted
        font.pixelSize: Theme.smallText
        visible: root.label.length > 0
    }
    Rectangle {
        Layout.preferredWidth: root.fieldWidth
        Layout.preferredHeight: 24
        radius: 4
        color: Theme.panelAlt
        border.color: input.activeFocus ? Theme.accent : root.invalid ? Theme.error : Theme.border
        opacity: root.enabled ? 1 : 0.4

        TextInput {
            id: input
            anchors.fill: parent
            anchors.leftMargin: 6
            anchors.rightMargin: 6
            verticalAlignment: TextInput.AlignVCenter
            clip: true
            selectByMouse: true
            color: Theme.text
            selectionColor: Qt.alpha(Theme.accent, 0.4)
            font.pixelSize: Theme.smallText + 1
            text: root.shown()
            onEditingFinished: {
                const r = Units.parse(text, root.unit, Units.shownFactor(root.value, root.unit))
                if (!r.ok || r.value < root.minimum || r.value > root.maximum) {
                    root.invalid = true
                    errorTip.text = !r.ok ? r.error
                                  : r.value < root.minimum ? qsTr("Au moins %1").arg(Units.compact(root.minimum, root.unit, 6))
                                  : qsTr("Au plus %1").arg(Units.compact(root.maximum, root.unit, 6))
                    errorTip.open()
                    return
                }
                root.invalid = false
                if (r.value !== root.value)
                    root.edited(r.value)
                root.restore()
            }
            Keys.onEscapePressed: {
                root.invalid = false
                root.restore()
                focus = false
            }
        }
        HoverHandler {
            id: hover
        }
        ToolTip.visible: hover.hovered && root.tip.length > 0 && !input.activeFocus
        ToolTip.delay: 500
        ToolTip.text: root.tip
        ToolTip {
            id: errorTip
            timeout: 3500
            y: parent.height + 2
        }
    }
}

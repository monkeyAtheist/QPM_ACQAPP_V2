import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import Qacq.App

// Text field for a quantity: shows "48 kHz", accepts "44,1 kHz", "2 ms", "3,3e-3 V", "115 200" or a bare
// number (read with the prefix currently shown: "44,1" over "48 kHz" is 44,1 kHz). Emits edited(value)
// only for a valid, changed value; `minimum` and `maximum` bound it.
ColumnLayout {
    id: root
    property string label
    property real value: 0
    property string unit: ""
    property bool readOnly: false
    property real minimum: -Number.MAX_VALUE
    property real maximum: Number.MAX_VALUE
    property string tip: ""
    property int fieldWidth: -1
    signal edited(real value)

    spacing: 2
    Layout.fillWidth: fieldWidth < 0

    function shown() {
        return Units.compact(root.value, root.unit, 6)
    }

    Label {
        text: root.label
        color: Theme.textMuted
        font.pixelSize: Theme.smallText
        visible: root.label.length > 0
    }
    TextField {
        id: field
        property bool invalid: false
        Layout.fillWidth: root.fieldWidth < 0
        Layout.preferredWidth: root.fieldWidth < 0 ? -1 : root.fieldWidth
        readOnly: root.readOnly
        selectByMouse: true
        font.pixelSize: Theme.bodyText
        text: root.shown()
        Material.accent: invalid ? Theme.error : Theme.accent
        ToolTip.delay: 600
        ToolTip.visible: hovered && !invalid && !root.readOnly && !activeFocus
        ToolTip.text: root.tip.length > 0 ? root.tip
                      : root.unit.length > 0 ? qsTr("Exemples : 2 k%1, 1,5 %1, 3,3e-3 %1").arg(root.unit)
                                             : qsTr("Un nombre : 2,5 ; 1 200 ; 3e-3")

        function restore() {
            text = Qt.binding(function() { return root.shown() })
        }
        onEditingFinished: {
            if (readOnly)
                return
            // Left untouched: nothing to do (reading back the rounded text would change the value).
            if (text === root.shown()) {
                invalid = false
                return
            }
            const r = Units.parse(text, root.unit, Units.shownFactor(root.value, root.unit))
            if (!r.ok) {
                invalid = true
                errorTip.text = r.error
                errorTip.open()
                return
            }
            if (r.value < root.minimum || r.value > root.maximum) {
                invalid = true
                errorTip.text = r.value < root.minimum
                        ? qsTr("Au moins %1").arg(Units.compact(root.minimum, root.unit, 6))
                        : qsTr("Au plus %1").arg(Units.compact(root.maximum, root.unit, 6))
                errorTip.open()
                return
            }
            invalid = false
            if (r.value !== root.value)
                root.edited(r.value)
            restore()
        }
        Keys.onEscapePressed: {
            invalid = false
            restore()
            focus = false
        }
        ToolTip {
            id: errorTip
            timeout: 3500
            y: field.height + 2
        }
    }
}

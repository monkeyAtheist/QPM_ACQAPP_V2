import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Text field with a label: emits edited(text) when editing ends on a changed text.
ColumnLayout {
    id: root
    property string label
    property string value: ""
    property string placeholder: ""
    property bool readOnly: false
    property string tip: ""
    property bool monospace: false
    signal edited(string text)

    spacing: 2
    Layout.fillWidth: true

    Label {
        text: root.label
        color: Theme.textMuted
        font.pixelSize: Theme.smallText
        visible: root.label.length > 0
    }
    TextField {
        id: field
        Layout.fillWidth: true
        readOnly: root.readOnly
        selectByMouse: true
        // Shown only while the field is empty and not edited: the Material style would otherwise raise it
        // above the field as a second label (ours is already there).
        placeholderText: text.length === 0 && !activeFocus ? root.placeholder : ""
        font.pixelSize: Theme.bodyText
        font.family: root.monospace ? Theme.monospace : Qt.application.font.family
        text: root.value
        ToolTip.delay: 600
        ToolTip.visible: hovered && root.tip.length > 0 && !activeFocus
        ToolTip.text: root.tip
        onEditingFinished: {
            if (text !== root.value)
                root.edited(text)
            text = Qt.binding(function() { return root.value })
        }
        Keys.onEscapePressed: {
            text = Qt.binding(function() { return root.value })
            focus = false
        }
    }
}

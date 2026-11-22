import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// A formula typed in a panel: its name above, the text on as many lines as it needs (a long formula stays
// readable whole). Entrée or leaving the field applies it (edited, only if it changed); Échap gives back
// what it was.
ColumnLayout {
    id: root
    property string label
    property string value: ""
    property string placeholder: ""
    property string tip: ""
    // What a line break pasted in the text becomes (" ; " for the variables, often written one per line).
    property string lineSeparator: " "
    signal edited(string text)

    spacing: 2
    Layout.fillWidth: true

    Label {
        text: root.label
        color: Theme.textMuted
        font.pixelSize: Theme.smallText
        visible: root.label.length > 0
    }
    TextArea {
        id: area
        Layout.fillWidth: true
        wrapMode: TextEdit.WrapAtWordBoundaryOrAnywhere
        selectByMouse: true
        font.family: Theme.monospace
        font.pixelSize: Theme.bodyText
        // Shown only while empty and not edited (the Material style would raise it as a second label).
        placeholderText: text.length === 0 && !activeFocus ? root.placeholder : ""
        text: root.value
        function restore() {
            text = Qt.binding(function() { return root.value })
        }
        function cleaned(t) {
            return t.replace(/\s*[\r\n]+\s*/g, root.lineSeparator).trim()
        }
        function apply() {
            const typed = cleaned(text)
            if (typed !== cleaned(root.value))
                root.edited(typed)
            restore()
        }
        onActiveFocusChanged: if (!activeFocus) apply()
        Keys.onReturnPressed: focus = false
        Keys.onEnterPressed: focus = false
        Keys.onEscapePressed: {
            restore()
            focus = false
        }
        ToolTip.visible: hovered && root.tip.length > 0 && !activeFocus
        ToolTip.delay: 600
        ToolTip.text: root.tip
    }
}

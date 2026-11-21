import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Field edited with a list of choices; its value is the index of the choice.
ColumnLayout {
    id: root
    property string label
    property var choices: []
    property int value: 0
    signal edited(int index)

    spacing: 2
    Layout.fillWidth: true

    Label {
        text: root.label
        color: Theme.textMuted
        font.pixelSize: Theme.smallText
        visible: root.label.length > 0
    }
    ComboBox {
        Layout.fillWidth: true
        model: root.choices
        currentIndex: root.value
        font.pixelSize: Theme.bodyText
        onActivated: (index) => { if (index !== root.value) root.edited(index) }
    }
}

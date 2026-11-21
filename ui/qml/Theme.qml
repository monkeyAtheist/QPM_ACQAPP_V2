pragma Singleton
import QtQuick

// Design tokens shared by every view (the plots included, so they follow the theme). Same palette as
// QTsignalApp V2.
QtObject {
    property bool dark: true

    readonly property color accent: dark ? "#5AAAFF" : "#0B6BD3"
    readonly property color window: dark ? "#14161A" : "#F3F5F8"
    readonly property color panel: dark ? "#1B1E23" : "#FFFFFF"
    readonly property color panelAlt: dark ? "#22262C" : "#F0F2F5"
    readonly property color border: dark ? "#2C3139" : "#DDE1E7"
    readonly property color text: dark ? "#E6E8EC" : "#1E2227"
    readonly property color textMuted: dark ? "#9AA1AC" : "#5F6672"
    readonly property color canvas: dark ? "#0F1114" : "#FAFBFC"
    readonly property color grid: dark ? Qt.rgba(1, 1, 1, 0.09) : Qt.rgba(0, 0, 0, 0.10)
    readonly property color error: "#FF6B6B"
    readonly property color success: "#4CC38A"
    readonly property color warning: "#FFB74D"

    // Colours of the channels, in order (the same in every plot and in the lists).
    readonly property var channelColors: dark
        ? ["#5AAAFF", "#FFA726", "#4CC38A", "#E57373", "#BA68C8", "#4DD0E1", "#FFD54F", "#A1887F"]
        : ["#0B6BD3", "#E65100", "#1B8A4F", "#C62828", "#7B1FA2", "#00838F", "#B8860B", "#6D4C41"]

    // Colour of an origin of signal ("acquired", "generated", "imported", "derived").
    function originColor(origin) {
        switch (origin) {
        case "acquired": return success
        case "generated": return accent
        case "imported": return warning
        default: return textMuted
        }
    }

    readonly property int radius: 8
    readonly property int gap: 12
    readonly property int smallText: 11
    readonly property int bodyText: 13
    readonly property int titleText: 15
    readonly property string monospace: "Consolas"
}

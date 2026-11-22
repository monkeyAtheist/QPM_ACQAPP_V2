import QtQuick
import QtQuick.Controls

// A menu as wide as its widest item. A menu of this style is 200 pixels wide whatever its items say, and cuts
// the longer ones ("Prédits (le signal cont…"): here the widest item decides, the check mark included, never
// narrower than the style's menu.
Menu {
    width: {
        let widest = 0
        for (let i = 0; i < count; ++i) {
            const item = itemAt(i)
            if (item)
                widest = Math.max(widest, item.implicitWidth)
        }
        return Math.max(implicitWidth, widest + leftPadding + rightPadding)
    }
}

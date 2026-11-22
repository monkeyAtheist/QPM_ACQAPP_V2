import QtQuick

// Group "Vue" of the tool header, the same in every module: everything on screen, zoom in and out, the
// view before. Zoom acts on `target` (a SignalPlot, a SpectrumPlot or a ModulationPlot: the plot pointed at
// last), "Tout voir" on every plot of `plots`, "Précédente" on `backTarget`.
RibbonGroup {
    id: group
    property Item target: null
    property var plots: target ? [target] : []
    property Item backTarget: target
    // Rolling display of an acquisition: "Zoom −" widens the window though nothing is zoomed.
    property bool rolling: false
    property string fitTip: qsTr("Tout le signal, échelles automatiques (double clic sur le tracé)")

    function zoom(factor) {
        if (!target)
            return
        if (typeof target.zoomView === "function")
            target.zoomView(factor)
        else if (typeof target.zoomFrequency === "function")
            target.zoomFrequency(factor)
        else
            target.zoomTime(factor)
    }

    title: qsTr("Vue")
    RibbonButton {
        glyph: "fit"
        text: qsTr("Tout voir")
        enabled: group.plots.some((p) => p && p.hasData)
        tip: group.fitTip
        onClicked: {
            for (const p of group.plots) {
                if (p && p.hasData)
                    p.showAll()
            }
        }
    }
    RibbonButton {
        glyph: "zoomin"
        text: qsTr("Zoom +")
        enabled: group.target !== null && group.target.hasData
        tip: qsTr("Deux fois plus resserré, sur le dernier tracé survolé (molette sur un tracé : autour du pointeur)")
        onClicked: group.zoom(0.5)
    }
    RibbonButton {
        glyph: "zoomout"
        text: qsTr("Zoom −")
        enabled: group.target !== null && group.target.hasData && (group.target.zoomed || group.rolling)
        tip: qsTr("Deux fois plus large")
        onClicked: group.zoom(2)
    }
    RibbonButton {
        glyph: "back"
        text: qsTr("Précédente")
        enabled: group.backTarget !== null && group.backTarget.canGoBack
        tip: qsTr("Revenir à la vue d'avant (zoom, déplacement, tout voir)")
        onClicked: group.backTarget.goBack()
    }
}

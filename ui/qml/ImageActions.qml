import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtCore
import Qacq.App

// Tool header group "Image": the plots of a view copied to the clipboard (to paste in a report) or saved
// as a PNG file, exactly as they are on screen.
RibbonGroup {
    id: group
    property Item target: null
    property string baseName: "graphique"
    signal failed(string message)
    signal done(string message)

    title: qsTr("Image")

    RibbonButton {
        glyph: "copy"
        text: qsTr("Copier")
        enabled: group.target !== null
        tip: qsTr("Le graphique dans le presse-papiers, à coller dans un rapport")
        onClicked: group.target.grabToImage(function (result) {
            if (ImageTools.copy(result.image))
                group.done(qsTr("Graphique copié dans le presse-papiers"))
            else
                group.failed(qsTr("Le graphique n'a pas pu être copié."))
        })
    }
    RibbonButton {
        glyph: "image"
        text: qsTr("Enregistrer")
        enabled: group.target !== null
        tip: qsTr("Le graphique dans un fichier PNG")
        onClicked: imageDialog.open()
    }

    FileDialog {
        id: imageDialog
        title: qsTr("Enregistrer le graphique")
        fileMode: FileDialog.SaveFile
        defaultSuffix: "png"
        nameFilters: [qsTr("Image PNG (*.png)")]
        currentFolder: StandardPaths.writableLocation(StandardPaths.PicturesLocation)
        onAccepted: {
            const file = selectedFile
            group.target.grabToImage(function (result) {
                if (result.saveToFile(file))
                    group.done(qsTr("Graphique enregistré : %1").arg(decodeURIComponent(file.toString().split("/").pop())))
                else
                    group.failed(qsTr("Le graphique n'a pas pu être enregistré."))
            })
        }
    }
}

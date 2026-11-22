# Layer 3 - the Qt Quick interface (executable QTacquisitionApp), on the shell of QTsignalApp V2.
set_source_files_properties(qml/Theme.qml qml/Choices.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_executable(QTacquisitionApp main.cpp)
qt_add_qml_module(QTacquisitionApp
    URI Qacq
    VERSION 1.0
    QML_FILES
        qml/Main.qml
        qml/Theme.qml
        qml/Choices.qml
        qml/Sidebar.qml
        qml/Ribbon.qml
        qml/RibbonGroup.qml
        qml/RibbonButton.qml
        qml/RibbonIcon.qml
        qml/PanelIcon.qml
        qml/FoldButton.qml
        qml/EdgeTab.qml
        qml/SectionTitle.qml
        qml/ChoiceField.qml
        qml/QuantityField.qml
        qml/RibbonField.qml
        qml/TextValueField.qml
        qml/SignalsPanel.qml
        qml/Inspector.qml
        qml/SignalProperties.qml
        qml/AcquisitionSettings.qml
        qml/SignalsView.qml
        qml/AcquisitionView.qml
        qml/PlotToolbar.qml
        qml/StatusStrip.qml
        qml/FirmwareDialog.qml
        qml/ImportDialog.qml
        qml/AnalysisPanel.qml
        qml/CursorReadout.qml
        qml/ImageActions.qml
        qml/ViewScrollBar.qml
        qml/CurveDialog.qml
        qml/CurveChips.qml
        qml/PlotMenu.qml
        qml/ViewGroup.qml
        qml/ScaleMenu.qml
        qml/FitMenu.qml
        qml/WindowField.qml
        qml/SpectrumSettings.qml
        qml/SpectrogramSettings.qml
        qml/GeneratorView.qml
        qml/GeneratorPanel.qml
        qml/WaveCard.qml
        qml/FormulaField.qml
        qml/ListenGroup.qml
        qml/FiltersView.qml
        qml/FilterPanel.qml
        qml/FilterCodeDialog.qml
    SOURCES
        signal_plot.h
        signal_plot.cpp
        spectrum_plot.h
        spectrum_plot.cpp
        spectrogram_plot.h
        spectrogram_plot.cpp
        image_tools.h
        image_tools.cpp
        motif_editor.h
        motif_editor.cpp
        response_plot.h
        response_plot.cpp
)
target_link_libraries(QTacquisitionApp PRIVATE qacq_app Qt6::Gui Qt6::Quick Qt6::QuickControls2 Qt6::Qml)
# Qt's generated QML-cache loader trips GCC's -Wnull-dereference inside qhash.h (inlined Qt code):
# not ours, so silenced for that generated file only.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/.rcc/qmlcache/QTacquisitionApp_qmlcache_loader.cpp"
        PROPERTIES COMPILE_OPTIONS "-Wno-null-dereference")
endif()
target_compile_definitions(QTacquisitionApp PRIVATE QACQ_VERSION="${PROJECT_VERSION}")
qacq_set_warnings(QTacquisitionApp)

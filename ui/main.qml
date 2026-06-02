import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window

import org.kde.kcmutils as KCM
import org.kde.kirigami as Kirigami

KCM.ScrollViewKCM {
    id: root

    implicitHeight: Kirigami.Units.gridUnit * 32
    implicitWidth: Kirigami.Units.gridUnit * 30

    // ── Header: color previews + toggle buttons ──
    header: ColumnLayout {
        spacing: Kirigami.Units.smallSpacing

        RowLayout {
            spacing: Kirigami.Units.largeSpacing

            // Cooler preview
            Rectangle {
                implicitWidth: 48; implicitHeight: 56; radius: 6
                color: Qt.rgba(kcm.engine.previewR(0)/255.0,
                               kcm.engine.previewG(0)/255.0,
                               kcm.engine.previewB(0)/255.0, 1)
                border.color: Kirigami.Theme.textColor
                border.width: 1
                Label { anchors.centerIn: parent; text: "❄"; color: "#cdd6f4"; font.pointSize: 10 }
            }

            // Mouse preview
            Rectangle {
                implicitWidth: 48; implicitHeight: 56; radius: 6
                color: Qt.rgba(kcm.engine.previewR(1)/255.0,
                               kcm.engine.previewG(1)/255.0,
                               kcm.engine.previewB(1)/255.0, 1)
                border.color: Kirigami.Theme.textColor
                border.width: 1
                Label { anchors.centerIn: parent; text: "🖱"; color: "#cdd6f4"; font.pointSize: 10 }
            }

            ColumnLayout {
                Layout.fillWidth: true
                Label {
                    text: i18n("RGB Controller")
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize + 4
                    font.weight: Font.Bold
                    color: Kirigami.Theme.highlightColor
                }
                Label {
                    id: tempLabel
                    text: ""
                    color: Kirigami.Theme.neutralTextColor
                    font.pointSize: Kirigami.Theme.defaultFont.pointSize - 1
                }
            }

            ColumnLayout {
                spacing: Kirigami.Units.smallSpacing
                Button {
                    id: powerBtn
                    text: kcm.engine.enabled ? i18n("System ON") : i18n("System OFF")
                    checked: kcm.engine.enabled
                    checkable: true
                    onClicked: kcm.engine.enabled = checked
                    background: Rectangle {
                        radius: 6
                        color: powerBtn.checked ? "#a6e3a1" : (powerBtn.hovered ? "#45475a" : "#313244")
                        border.color: "#45475a"
                    }
                    contentItem: Label {
                        text: powerBtn.text
                        color: powerBtn.checked ? "#1e1e2e" : "#cdd6f4"
                        font.weight: Font.Bold
                        font.pointSize: Kirigami.Theme.defaultFont.pointSize - 1
                    }
                }
                Button {
                    id: tempBtn
                    text: kcm.engine.tempMode ? i18n("Temp: ON") : i18n("Temp: OFF")
                    checked: kcm.engine.tempMode
                    checkable: true
                    onClicked: kcm.engine.tempMode = checked
                    background: Rectangle {
                        radius: 6
                        color: tempBtn.checked ? "#fab387" : (tempBtn.hovered ? "#45475a" : "#313244")
                        border.color: "#45475a"
                    }
                    contentItem: Label {
                        text: tempBtn.text
                        color: tempBtn.checked ? "#1e1e2e" : "#cdd6f4"
                        font.weight: tempBtn.checked ? Font.Bold : Font.Normal
                        font.pointSize: Kirigami.Theme.defaultFont.pointSize - 1
                    }
                }
            }
        }
    }

    // ── Body: TabBar + StackLayout ──
    ColumnLayout {
        spacing: Kirigami.Units.smallSpacing

        TabBar {
            id: deviceTabs
            Layout.fillWidth: true
            TabButton { text: i18n("❄ Cooler") }
            TabButton { text: i18n("🖱 Mouse") }
            TabButton { text: i18n("🎨 Palettes") }
        }

        StackLayout {
            currentIndex: deviceTabs.currentIndex
            Layout.fillWidth: true
            Layout.fillHeight: true

            // ── Device tab (reusable) ──
            function createDeviceTab(devIdx) {
                return Item {
                    ColumnLayout {
                        anchors.fill: parent
                        spacing: Kirigami.Units.smallSpacing

                        // Effect selector
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Kirigami.Units.smallSpacing
                            Label { text: i18n("Effect:"); Layout.minimumWidth: Kirigami.Units.gridUnit * 3 }
                            ComboBox {
                                id: effectCombo
                                Layout.fillWidth: true
                                model: kcm.engine.effects
                                currentIndex: kcm.engine.effectIndex(devIdx)
                                onCurrentIndexChanged: {
                                    if (currentIndex >= 0)
                                        kcm.engine.setEffectIndex(devIdx, currentIndex)
                                }
                            }
                        }

                        // Palette selector
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Kirigami.Units.smallSpacing
                            Label { text: i18n("Palette:"); Layout.minimumWidth: Kirigami.Units.gridUnit * 3 }
                            ComboBox {
                                id: paletteCombo
                                Layout.fillWidth: true
                                model: kcm.engine.palettes
                                currentIndex: {
                                    var pi = kcm.engine.paletteIndex(devIdx)
                                    return pi < 0 ? kcm.engine.palettes.length - 1 : pi
                                }
                                onCurrentIndexChanged: {
                                    if (currentIndex >= 0 && currentIndex < kcm.engine.palettes.length - 1)
                                        kcm.engine.setPaletteIndex(devIdx, currentIndex)
                                }
                            }
                        }

                        // Palette gradient swatch
                        Rectangle {
                            id: swatch
                            Layout.fillWidth: true
                            Layout.preferredHeight: 20
                            radius: 4
                            border.color: Kirigami.Theme.textColor
                            border.width: 1
                            gradient: Gradient {
                                orientation: Gradient.Horizontal
                                GradientStop { position: 0.0; color: "#89b4fa" }
                                GradientStop { position: 1.0; color: "#cba6f7" }
                            }
                            // Update swatch from palette
                            Connections {
                                target: kcm.engine
                                function onPreviewUpdated() {
                                    // Rebuild gradient from palette data via C++
                                    // For now, use a simple hue-based gradient
                                }
                            }
                        }

                        // ── Speed slider ──
                        Kirigami.FormLayout {
                            Layout.fillWidth: true
                            Layout.topMargin: Kirigami.Units.largeSpacing

                            Item {
                                Kirigami.FormData.label: i18n("Speed:")
                                Layout.fillWidth: true
                                implicitHeight: speedSlider.implicitHeight
                                Slider {
                                    id: speedSlider
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    from: 1; to: 50; value: kcm.engine.speedValue(devIdx)
                                    onMoved: kcm.engine.setSpeedValue(devIdx, value)
                                }
                            }
                            Label {
                                text: i18n("%1x").arg(speedSlider.value / 100.0, 0, 'f', 2)
                                color: Kirigami.Theme.highlightColor
                            }

                            Item {
                                Kirigami.FormData.label: i18n("Brightness:")
                                Layout.fillWidth: true
                                implicitHeight: brightSlider.implicitHeight
                                Slider {
                                    id: brightSlider
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    from: 20; to: 100; value: kcm.engine.intensityValue(devIdx)
                                    onMoved: kcm.engine.setIntensityValue(devIdx, value)
                                }
                            }
                            Label {
                                text: i18n("%1%").arg(brightSlider.value)
                                color: Kirigami.Theme.highlightColor
                            }

                            Item {
                                Kirigami.FormData.label: i18n("Breath:")
                                Layout.fillWidth: true
                                implicitHeight: breathSlider.implicitHeight
                                Slider {
                                    id: breathSlider
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    from: 0; to: 40; value: kcm.engine.breathValue(devIdx)
                                    onMoved: kcm.engine.setBreathValue(devIdx, value)
                                }
                            }
                            Label {
                                text: i18n("%1%").arg(breathSlider.value)
                                color: Kirigami.Theme.highlightColor
                            }

                            // Gradient direction
                            Item {
                                Kirigami.FormData.label: i18n("Gradient:")
                                Layout.fillWidth: true
                                implicitHeight: dirBtn.implicitHeight
                                Button {
                                    id: dirBtn
                                    anchors.left: parent.left
                                    text: kcm.engine.directionReversed(devIdx) ? i18n("Reversed") : i18n("Normal")
                                    checkable: true
                                    checked: kcm.engine.directionReversed(devIdx)
                                    onClicked: kcm.engine.setDirectionReversed(devIdx, checked)
                                    background: Rectangle {
                                        radius: 6
                                        color: dirBtn.checked ? "#89b4fa" : (dirBtn.hovered ? "#45475a" : "#313244")
                                        border.color: "#45475a"
                                    }
                                    contentItem: Label {
                                        text: dirBtn.text
                                        color: dirBtn.checked ? "#1e1e2e" : "#cdd6f4"
                                        font.weight: Font.Bold
                                        font.pointSize: Kirigami.Theme.defaultFont.pointSize - 1
                                    }
                                }
                            }
                        }

                        // Status line
                        Label {
                            Layout.fillWidth: true
                            text: i18n("Live · %1 · %2")
                                .arg(kcm.engine.effects[kcm.engine.effectIndex(devIdx)])
                                .arg(kcm.engine.palettes[
                                    kcm.engine.paletteIndex(devIdx) < 0
                                    ? kcm.engine.palettes.length - 1
                                    : kcm.engine.paletteIndex(devIdx)])
                            color: Kirigami.Theme.positiveTextColor
                            font.pointSize: Kirigami.Theme.defaultFont.pointSize - 1
                            horizontalAlignment: Text.AlignHCenter
                        }
                    }
                }
            }

            // Tab 0: Cooler
            Loader { sourceComponent: createDeviceTab(0) }

            // Tab 1: Mouse
            Loader { sourceComponent: createDeviceTab(1) }

            // ── Tab 2: Palettes ──
            Item {
                ColumnLayout {
                    anchors.fill: parent
                    spacing: Kirigami.Units.smallSpacing

                    Label {
                        text: i18n("Click a palette to preview:")
                        color: Kirigami.Theme.highlightColor
                        font.weight: Font.Bold
                    }

                    Repeater {
                        model: kcm.engine.palettes
                        delegate: Item {
                            Layout.fillWidth: true
                            Layout.preferredHeight: Kirigami.Units.gridUnit + 8

                            property int palIdx: index

                            MouseArea {
                                anchors.fill: parent
                                hoverEnabled: true
                                onClicked: {
                                    // Apply to both devices
                                    kcm.engine.setPaletteIndex(0, palIdx)
                                    kcm.engine.setPaletteIndex(1, palIdx)
                                }
                                Rectangle {
                                    anchors.fill: parent
                                    radius: 4
                                    color: mouseArea.containsMouse ? Qt.rgba(0.5,0.5,0.5,0.2) : "transparent"
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // ── Timer for preview updates ──
    Connections {
        target: kcm.engine
        function onPreviewUpdated() { /* triggers binding re-eval */ }
        function onTempReading(text) { tempLabel.text = text; }
    }
}

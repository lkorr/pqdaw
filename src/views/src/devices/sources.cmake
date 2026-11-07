# T4.6 device panels: device signatures, a leaf block's Device-view frame (GU.11: generated bodies only; the panel
# descriptions, the stock devices' displays and the stand-alone device preview are in retired/), the analysis feed.
set(PQ_VIEWS_DEVICES_SOURCES
  src/devices/device_model.cpp
  src/devices/panel_layout.cpp  # GU.11: a leaf block's generated frame body (autoPanel / facePanel)
  src/devices/device_panel.cpp
  src/devices/face_display.cpp  # TMM.10: a meta module's face as a panel control (a native engine's frame)
  src/devices/analysis.cpp
  src/devices/spectrum_plot.cpp  # TX.spectrum: the reusable spectrum + response display
  src/devices/device_scenes.cpp
  src/devices/knob_curve_menu.cpp)  # TX.knob-curves: the Knob Curve submenu

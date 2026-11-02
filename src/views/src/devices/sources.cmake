# T4.6 device panels: device signatures, panel descriptions, the panel widget and its displays, the analysis feed,
# the Device view (stand-alone device preview until T4.1's device chains).
set(PQ_VIEWS_DEVICES_SOURCES
  src/devices/device_model.cpp
  src/devices/panel_desc.cpp
  src/devices/device_panel.cpp
  src/devices/device_displays.cpp
  src/devices/operator_displays.cpp  # T4.12: Operator's envelope, algorithm picker, partial editor
  src/devices/face_display.cpp  # TMM.10: a meta module's face as a panel control (a chain meta device's panel)
  src/devices/panel_face.cpp  # TX.panel-faces: a device's panel as a circuit block's face (the project patch view)
  src/devices/analysis.cpp
  src/devices/spectrum_plot.cpp  # TX.spectrum: the reusable spectrum + response display
  src/devices/device_view.cpp
  src/devices/device_scenes.cpp)

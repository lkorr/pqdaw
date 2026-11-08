# Device signatures and value mapping, the analysis feed, the Knob Curve menu (GU.13: a leaf block's Device-view frame is
# a group-system panel: pq/views/panel/block_frame.h).
set(PQ_VIEWS_DEVICES_SOURCES
  src/devices/device_model.cpp
  src/devices/analysis.cpp
  src/devices/spectrum_plot.cpp  # TX.spectrum: the reusable spectrum + response display
  src/devices/device_scenes.cpp
  src/devices/knob_curve_menu.cpp)  # TX.knob-curves: the Knob Curve submenu

# T4.1 Device view: the device chain, one T4.6 device panel per device.
set(PQ_VIEWS_DEVICE_CHAIN_SOURCES
  src/device_chain/device_chain.cpp
  src/device_chain/wiring.cpp  # T4.7: the cable layer, jacks → knobs
  src/device_chain/rack_frame.cpp)  # T4.8: racks (macro panel, chain list + mixer, zones, map mode)

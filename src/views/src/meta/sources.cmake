# TMM.4 meta-module faces: the face base and registry, the document target and synthetic voices, the faces.
set(PQ_VIEWS_META_SOURCES
  src/meta/meta_face.cpp
  src/meta/face_targets.cpp
  src/meta/phase_acc_face.cpp
  src/meta/tap_voices.cpp       # TMM.5: the faces' reader over the engine's voice taps
  src/meta/wavetable_face.cpp   # TMM.7: the Wavetable face
  src/meta/envelope_face.cpp    # TMM.11: the Envelope face
  src/meta/wavetable_edit.cpp   # TMM.9: its harmonic editor and draw tools (the math)
  src/meta/wavetable_model.cpp) # TMM.7: its display frames (keyframe spectra, dense frames, the bake at display resolution)

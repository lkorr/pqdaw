#pragma once

// The built-in faces (TMM.4): their factories, registered by meta_face.cpp. Private to views.

#include <memory>

#include "pq/views/meta/meta_face.h"

namespace pq::views {

std::unique_ptr<MetaFace> makePhaseAccFace(const meta::Type& t);
std::unique_ptr<MetaFace> makeWavetableFace(const meta::Type& t);  // TMM.7
std::unique_ptr<MetaFace> makeEnvelopeFace(const meta::Type& t);   // TMM.11

}  // namespace pq::views

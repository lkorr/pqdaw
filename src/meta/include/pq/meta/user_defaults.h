#pragma once

// VZ.1: the user's saved defaults for meta module types — the data a NEW instance of a type starts with (a face's
// SAVE DEFAULT: the display modules' options). doc::defaultMetaInstance reads it, so every way of making an instance
// (a chain device, a circuit call, a library patch) starts from it; instances that exist keep their own data. The app
// fills the store from the settings file at start and persists through the sink (pq/app/user_settings.h
// "metaDefaults"); without an app (tests, scenes) it is empty and types start from Type::defaultData.
//
// UI thread only (not RT code).

#include <functional>
#include <string_view>

#include "pq/meta/data.h"

namespace pq::meta {

// The saved data of `type` (null: none saved).
DataPtr userDefaultData(std::string_view type);
// Sets (or, null, clears) the saved data of `type` in the store only (the app's load; tests).
void setUserDefaultData(std::string_view type, DataPtr data);
// Sets it and hands it to the sink (a face's SAVE DEFAULT): the app writes it to the settings file.
void saveUserDefaultData(std::string_view type, DataPtr data);
// The persister (null: none). The app installs it once at start.
void setUserDefaultsSink(std::function<void(std::string_view type, const DataPtr& data)> sink);

}  // namespace pq::meta

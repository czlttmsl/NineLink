#pragma once
#include <string>

namespace nl {

// Writes a C#-like listing of the game's classes (fields with offsets,
// methods with RVAs) to `path`. Full detail for the game's own assemblies,
// class names only for engine / package assemblies.
bool dump_types(const std::wstring& path);

} // namespace nl

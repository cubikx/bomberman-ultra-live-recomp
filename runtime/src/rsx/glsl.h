#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rsx::glsl {

enum class Stage { vertex, fragment };

// Compile GLSL to SPIR-V; empty on failure (the error and source are logged).
std::vector<uint32_t> compile(Stage stage, const std::string& source);

} // namespace rsx::glsl

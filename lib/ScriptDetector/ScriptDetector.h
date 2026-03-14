#pragma once

#include <cstdint>

namespace ScriptDetector {

bool isArabicCodepoint(uint32_t codepoint);
bool containsArabic(const char* text);

}  // namespace ScriptDetector
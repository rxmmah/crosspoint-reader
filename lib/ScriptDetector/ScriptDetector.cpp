#include "ScriptDetector.h"

#include <Utf8.h>

namespace ScriptDetector {

bool isArabicCodepoint(const uint32_t codepoint) {
  return (codepoint >= 0x0600 && codepoint <= 0x06FF) || (codepoint >= 0x0750 && codepoint <= 0x077F) ||
         (codepoint >= 0x08A0 && codepoint <= 0x08FF) || (codepoint >= 0xFB50 && codepoint <= 0xFDFF) ||
         (codepoint >= 0xFE70 && codepoint <= 0xFEFF) || (codepoint >= 0x1EE00 && codepoint <= 0x1EEFF);
}

bool containsArabic(const char* text) {
  if (text == nullptr) {
    return false;
  }

  const unsigned char* cursor = reinterpret_cast<const unsigned char*>(text);
  while (*cursor != 0) {
    const uint32_t codepoint = utf8NextCodepoint(&cursor);
    if (isArabicCodepoint(codepoint)) {
      return true;
    }
  }

  return false;
}

}  // namespace ScriptDetector
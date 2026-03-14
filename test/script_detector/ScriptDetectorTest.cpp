#include <ScriptDetector.h>

#include <cassert>

int main() {
  using ScriptDetector::containsArabic;
  using ScriptDetector::isArabicCodepoint;

  assert(isArabicCodepoint(0x0627));
  assert(isArabicCodepoint(0x0750));
  assert(isArabicCodepoint(0xFE8E));
  assert(!isArabicCodepoint('A'));
  assert(!isArabicCodepoint(0x4E00));

  assert(containsArabic(u8"السلام عليكم"));
  assert(containsArabic(u8"Latin ثم Arabic"));
  assert(containsArabic(u8"123 مرحبا"));
  assert(!containsArabic("Plain ASCII text"));
  assert(!containsArabic(u8"Русский текст"));
  assert(!containsArabic(nullptr));

  return 0;
}
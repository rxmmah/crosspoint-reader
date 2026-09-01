#pragma once

#include <FreeInkUI.h>
#include <I18n.h>

#include <cstdint>

namespace keyboard_layouts {

struct LayoutInfo {
  freeink::ui::KeyboardLayoutId id;
  // Code of the UI language this layout is the natural default for, matched
  // against LANGUAGE_CODES rather than a Language enumerator: the enum only
  // holds the translations this build ships (lib/I18n/translations/), while
  // every keyboard here stays useful for passwords, search and file names.
  const char* languageCode;
  // Row label in the layout picker. Named for the layout rather than for a UI
  // language, since most of these have no translation in this build.
  StrId name;
};

// Table position is the persisted bit assignment. Keep existing rows in place
// and append new layouts so SDK enum changes cannot reinterpret saved masks.
inline constexpr LayoutInfo ALL[] = {
    {freeink::ui::KeyboardLayoutId::QwertyEn, "EN", StrId::STR_KBD_QWERTY_EN},
    {freeink::ui::KeyboardLayoutId::AzertyFr, "FR", StrId::STR_KBD_AZERTY_FR},
    {freeink::ui::KeyboardLayoutId::QwertzDe, "DE", StrId::STR_KBD_QWERTZ_DE},
    {freeink::ui::KeyboardLayoutId::SpanishEs, "ES", StrId::STR_KBD_SPANISH_ES},
    {freeink::ui::KeyboardLayoutId::CyrillicRu, "RU", StrId::STR_KBD_CYRILLIC_RU},
    {freeink::ui::KeyboardLayoutId::CyrillicUk, "UK", StrId::STR_KBD_CYRILLIC_UK},
    {freeink::ui::KeyboardLayoutId::CyrillicBe, "BE", StrId::STR_KBD_CYRILLIC_BE},
    {freeink::ui::KeyboardLayoutId::CyrillicKk, "KK", StrId::STR_KBD_CYRILLIC_KK},
    {freeink::ui::KeyboardLayoutId::HebrewIl, "HE", StrId::STR_KBD_HEBREW_IL},
};
inline constexpr uint8_t COUNT = sizeof(ALL) / sizeof(ALL[0]);
static_assert(COUNT <= 16, "keyboard layout mask is uint16_t");

inline constexpr uint16_t bitAt(const uint8_t i) { return static_cast<uint16_t>(1u << i); }
// Symbol layers have no Latin letters, so credentials and URLs require at
// least one of these layouts to remain enabled.
inline constexpr uint16_t LATIN_BITS = bitAt(0) | bitAt(1) | bitAt(2) | bitAt(3);

uint16_t enabled();
freeink::ui::KeyboardLayoutId startingLayout();
freeink::ui::KeyboardLayoutId next(freeink::ui::KeyboardLayoutId current);

}  // namespace keyboard_layouts

#pragma once

#include <FreeInkUI.h>
#include <I18nKeys.h>
#include <I18n.h>

#include <cstdint>

namespace keyboard_layouts {

struct LayoutInfo {
  freeink::ui::KeyboardLayoutId id;
  StrId label;
};

// Table position is the persisted bit assignment. Keep existing rows in place
// and append new layouts so SDK enum changes cannot reinterpret saved masks.
inline constexpr LayoutInfo ALL[] = {
    {freeink::ui::KeyboardLayoutId::QwertyEn, StrId::STR_KEYBOARD_EN},
    {freeink::ui::KeyboardLayoutId::AzertyFr, StrId::STR_KEYBOARD_FR},
    {freeink::ui::KeyboardLayoutId::QwertzDe, StrId::STR_KEYBOARD_DE},
    {freeink::ui::KeyboardLayoutId::SpanishEs, StrId::STR_KEYBOARD_ES},
    {freeink::ui::KeyboardLayoutId::CyrillicRu, StrId::STR_KEYBOARD_RU},
    {freeink::ui::KeyboardLayoutId::CyrillicUk, StrId::STR_KEYBOARD_UK},
    {freeink::ui::KeyboardLayoutId::CyrillicBe, StrId::STR_KEYBOARD_BE},
    {freeink::ui::KeyboardLayoutId::CyrillicKk, StrId::STR_KEYBOARD_KK},
    {freeink::ui::KeyboardLayoutId::HebrewIl, StrId::STR_KEYBOARD_HE},
    {freeink::ui::KeyboardLayoutId::ArabicAr, StrId::STR_KEYBOARD_AR},
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

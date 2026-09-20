#pragma once

#include <GfxRenderer.h>

#include "CrossPointSettings.h"

// Standby сам ориентацию не задаёт: SETTINGS.orientation (плитка в шторке, «Настройки → Ориентация»)
// upstream применяет только в читалке, а ReaderActivity при выходе сбрасывает рендерер в Portrait.
// Эти функции вызываются хуками из StandbyActivity (onEnter / loop / onExit), см. hooks/apply_hooks.py.
namespace calendar_orientation {

inline GfxRenderer::Orientation fromSetting(uint8_t o) {
  switch (o) {
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CW:
      return GfxRenderer::Orientation::LandscapeClockwise;
    case CrossPointSettings::ORIENTATION::INVERTED:
      return GfxRenderer::Orientation::PortraitInverted;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CCW:
      return GfxRenderer::Orientation::LandscapeCounterClockwise;
    default:
      return GfxRenderer::Orientation::Portrait;
  }
}

// Привести рендерер к настройке. true — ориентация изменилась (нужна перерисовка).
inline bool sync(GfxRenderer& r) {
  const GfxRenderer::Orientation want = fromSetting(SETTINGS.orientation);
  if (r.getOrientation() == want) return false;
  r.setOrientation(want);
  return true;
}

// Остальные экраны рассчитаны на портрет — при выходе из Standby вернуть его.
inline void restore(GfxRenderer& r) { r.setOrientation(GfxRenderer::Orientation::Portrait); }

}  // namespace calendar_orientation

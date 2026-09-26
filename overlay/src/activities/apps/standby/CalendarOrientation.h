#pragma once

#include <GfxRenderer.h>

#include "CrossPointSettings.h"
#include "activities/RenderLock.h"

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
// Вызывается из onEnter() и loop() — оба менеджер активностей зовёт БЕЗ RenderLock, а кадр рисует другая задача. Менять
// ориентацию посреди кадра нельзя: после шторки (плитка «ориентация») менеджер сразу заказывает перерисовку старой
// ориентацией, и смена в середине кадра уводила остаток рисования за край экрана — тысячи строк «[GFX] Outside range»
// и кадр 0,8 с вместо 0,07 (журнал 26.09.2026, CONCEPT §13.15). Поэтому — под RenderLock: дождаться конца кадра.
inline bool sync(GfxRenderer& r) {
  const GfxRenderer::Orientation want = fromSetting(SETTINGS.orientation);
  if (r.getOrientation() == want) return false;  // ориентацию меняет только главная задача — читать можно без блокировки
  RenderLock lock;
  r.setOrientation(want);
  return true;
}

// Остальные экраны рассчитаны на портрет — при выходе из Standby вернуть его.
// Вызывается из onExit(), а его менеджер зовёт, УЖЕ держа RenderLock (exitActivity), — повторно не берём.
inline void restore(GfxRenderer& r) { r.setOrientation(GfxRenderer::Orientation::Portrait); }

}  // namespace calendar_orientation

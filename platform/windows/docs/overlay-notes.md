# Overlay на Windows 11: выбранный путь и ограничения

## Проверенный способ (proof-of-concept `poc/overlay_poc.cpp`)

Окно на каждый монитор:

```
WS_POPUP
WS_EX_NOREDIRECTIONBITMAP | WS_EX_LAYERED | WS_EX_TRANSPARENT |
WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW
```

* `WS_EX_NOREDIRECTIONBITMAP` — у окна нет GDI-поверхности, содержимое даёт только
  DirectComposition-визуал.
* D3D11 swap chain `CreateSwapChainForComposition`, `DXGI_FORMAT_B8G8R8A8_UNORM`,
  `DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL`, `DXGI_ALPHA_MODE_PREMULTIPLIED`, привязан к HWND
  через `IDCompositionDevice::CreateTargetForHwnd`.
* `WS_EX_LAYERED | WS_EX_TRANSPARENT` — hit-testing проходит сквозь окно (клик получает окно
  под overlay); дополнительно `WM_NCHITTEST → HTTRANSPARENT`, `WM_MOUSEACTIVATE → MA_NOACTIVATE`.
* `WS_EX_TOOLWINDOW` — нет в Alt+Tab и на панели задач; `WS_EX_NOACTIVATE` — не забирает фокус.

Результат PoC на этом ПК (Windows 11 Pro 26200, 1920×1080):
прозрачный пиксель = цвет окна под overlay, непрозрачный белый = `ffffff`,
полупрозрачный 50% белый поверх `ff00ff` = `ff80ff` (корректный premultiplied-блендинг),
`WindowFromPoint` возвращает нижележащее окно, overlay не становится foreground → **PASS**.

Отвергнутые варианты: `UpdateLayeredWindow` (CPU-копирование всей поверхности каждый кадр),
`SetLayeredWindowAttributes` с color key (нет полупрозрачности), DWM blur-behind (устарел).

## Интерактивный режим и click-through

Overlay **всегда** пропускает ввод. Нельзя одновременно обещать полный click-through и перехват
кликов по всей площади экрана, поэтому:

* в обычном режиме приложение вообще не трогает ввод — нет хуков;
* режим взаимодействия включается явно: удержание сочетания (по умолчанию Alt+Shift; если это
  сочетание занято переключением раскладки — Ctrl+Shift или Ctrl+Alt), горячая клавиша
  `Ctrl+Alt+I` или пункт меню трея;
* только в этом режиме ставится `WH_MOUSE_LL`. Он «съедает» нажатие ЛКМ **только** если оно
  попало в hit-region (видимый снег или снежок/снеговик), опубликованный симуляцией. Все остальные
  клики, перетаскивания окон и колесо проходят без изменений;
* клавиатура глобально не перехватывается, сочетание опрашивается через `GetAsyncKeyState`;
* фокус не меняется ни при входе, ни при выходе из режима.

## Слежение за окнами

`EnumWindows` (Z-порядок сверху вниз) + out-of-context `SetWinEventHook`; callback только
ставит флаг, полная пересборка коалесцируется (не чаще ~8 мс) + сверка раз в секунду.
Координаты — `DWMWA_EXTENDED_FRAME_BOUNDS` (видимая рамка без невидимых resize-границ),
fallback `GetWindowRect`. Свёрнутые и `DWMWA_CLOAKED` (другой виртуальный рабочий стол,
приостановленные UWP) окна считаются скрытыми: их сугроб сохраняется, но не рисуется.

## Где снега не будет (ограничения ОС, обходить не пытаемся)

* Secure desktop (UAC), экран блокировки, Ctrl+Alt+Del — другой рабочий стол.
* Эксклюзивный полноэкранный режим игр; при обнаружении fullscreen overlay скрывается.
* Окна поверх topmost-слоя оболочки: меню «Пуск», поиск, центр уведомлений, всплывающие
  меню трея — они рисуются в отдельных слоях/бэндах DWM и могут оказаться выше overlay.
* Окна с UIAccess и некоторые защищённые окна (DRM-видео) могут перекрывать overlay.
* Нестандартные окна (без рамки, с прозрачными областями, со своей формой) — верхняя кромка
  берётся из прямоугольника DWM, снег может лежать на невидимой части.

## HDR

Swap chain в SDR-формате `B8G8R8A8_UNORM` с premultiplied alpha: DWM корректно композитирует
его на HDR-рабочем столе (SDR-контент с яркостью по слайдеру «SDR content brightness»).
Чёрного непрозрачного фона нет, т.к. прозрачность задаётся альфой, а не color key.
На этом ПК HDR не проверялся — см. test-plan.

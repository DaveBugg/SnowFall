// X(id, russian, english) -- included by strings.h (enum) and strings.cpp (table).
// No include guard on purpose.

// ---- general ---------------------------------------------------------------
X(AppName, L"SnowFall", L"SnowFall")
X(SettingsTitle, L"SnowFall — настройки", L"SnowFall — Settings")

// ---- statuses (also for the integrator: tooltips, balloons, status line) ----
X(StatusRunning, L"Работает", L"Running")
X(StatusPaused, L"Пауза", L"Paused")
X(StatusHidden, L"Скрыто", L"Hidden")
X(StatusAutoPausedFullscreen, L"Пауза (полноэкранное приложение)", L"Paused (fullscreen application)")
X(StatusAutoPausedLocked, L"Пауза (сеанс заблокирован)", L"Paused (session locked)")
X(StatusAutoPausedBattery, L"Пауза (питание от батареи)", L"Paused (on battery)")
X(StatusReducedOnBattery, L"Работает (сниженное качество, питание от батареи)", L"Running (reduced quality, on battery)")
X(StatusRenderingDisabled, L"Отрисовка отключена", L"Rendering disabled")
X(StatusLabelFmt, L"Состояние: %ls", L"Status: %ls")

// ---- balloons ----------------------------------------------------------------
X(BalloonHotkeyConflictTitle, L"Горячая клавиша занята", L"Hotkey unavailable")
X(BalloonHotkeyConflictFmt, L"Сочетание %ls уже используется другой программой. Назначьте другое в настройках.",
  L"The combination %ls is already used by another program. Choose a different one in Settings.")
X(BalloonDeviceLostTitle, L"Отрисовка снега отключена", L"Snow rendering disabled")
X(BalloonDeviceLostText, L"Графическое устройство недоступно, эффекты временно отключены. Перезапустите SnowFall, чтобы повторить попытку.",
  L"The graphics device is unavailable, so effects are turned off for now. Restart SnowFall to try again.")
X(InteractionModeOn, L"Режим взаимодействия включён", L"Interaction mode is on")
X(InteractionModeOff, L"Режим взаимодействия выключен", L"Interaction mode is off")
X(HotkeyInUse, L"Занято другой программой", L"Used by another program")
X(HotkeyInvalid, L"Недопустимое сочетание", L"Invalid combination")

// ---- tray menu ---------------------------------------------------------------
X(MenuPause, L"Пауза", L"Pause")
X(MenuResume, L"Продолжить", L"Resume")
X(MenuIntensity, L"Интенсивность", L"Intensity")
X(MenuInteraction, L"Режим взаимодействия", L"Interaction mode")
X(MenuTool, L"Инструмент", L"Tool")
X(MenuClearSnow, L"Очистить снег", L"Clear snow")
X(MenuCallSnowplow, L"Вызвать снегоуборщик", L"Call the snowplow")
X(MenuHideAll, L"Скрыть всё", L"Hide everything")
X(MenuSettings, L"Настройки…", L"Settings…")
X(MenuAutostart, L"Автозапуск", L"Start with Windows")
X(MenuExit, L"Выход", L"Exit")
X(MenuFireworks, L"Салют!", L"Fireworks!")
X(MenuUserPresets, L"Мои пресеты", L"My presets")
X(MenuNoPresets, L"(пока нет)", L"(none yet)")

// ---- shared option names -------------------------------------------------------
X(PresetLight, L"Лёгкий", L"Light")
X(PresetNormal, L"Обычный", L"Normal")
X(PresetBlizzard, L"Метель", L"Blizzard")
X(PresetThaw, L"Оттепель", L"Thaw")
X(SceneLight, L"Тихий вечер", L"Quiet evening")
X(SceneLightSub, L"Лёгкий снег", L"Light snow")
X(SceneNormal, L"Снегопад", L"Snowfall")
X(SceneNormalSub, L"Обычный", L"Normal")
X(SceneBlizzard, L"Метель", L"Blizzard")
X(SceneBlizzardSub, L"Сильный ветер", L"Strong wind")
X(SceneThaw, L"Оттепель", L"Thaw")
X(SceneThawSub, L"Мокрый снег", L"Wet snow")
X(ToolMitten, L"Варежка", L"Mitten")
X(ToolShovel, L"Лопата", L"Shovel")
X(ToolSnowball, L"Снежок", L"Snowball")

// ---- tabs ----------------------------------------------------------------------
X(TabSnow, L"Снег", L"Snow")
X(TabDrifts, L"Сугробы", L"Drifts")
X(TabTools, L"Инструменты", L"Tools")
X(TabPerformance, L"Производительность", L"Performance")
X(TabFestive, L"Праздник", L"Festive")
X(TabSystem, L"Система", L"System")
X(TabAbout, L"О программе", L"About")

// ---- bottom bar ------------------------------------------------------------------
X(BtnRestoreDefaults, L"Восстановить значения по умолчанию", L"Restore defaults")
X(BtnClose, L"Закрыть", L"Close")
X(ThemeNight, L"Зимняя ночь", L"Winter night")
X(ThemeGlobe, L"Снежный шар", L"Snow globe")
X(ThemePostcard, L"Новогодняя открытка", L"New Year card")
X(HeaderSubtitleFmt, L"Настройки · %ls", L"Settings · %ls")
X(PreviewTitle, L"Предпросмотр", L"Preview")
X(PreviewHint, L"ускорено ×4", L"4× speed")
X(BtnResetPreview, L"Сбросить превью", L"Reset preview")
X(CountdownTitle, L"До Нового года", L"Until New Year")
X(CountdownDay1, L"день до Нового года", L"day until New Year")
X(CountdownDay2, L"дня до Нового года", L"days until New Year")
X(CountdownDay5, L"дней до Нового года", L"days until New Year")
X(CountdownHour1, L"час до Нового года", L"hour until New Year")
X(CountdownHour2, L"часа до Нового года", L"hours until New Year")
X(CountdownHour5, L"часов до Нового года", L"hours until New Year")
X(HappyNewYear, L"С Новым годом!", L"Happy New Year!")
X(HappyHolidays, L"Снежных и тёплых праздников!", L"Snowy and cosy holidays!")
X(RestoreConfirmText, L"Вернуть все настройки к значениям по умолчанию?", L"Reset all settings to their defaults?")

// ---- tab «Снег» ------------------------------------------------------------------
X(GroupWeather, L"Режим погоды", L"Weather preset")
X(LblIntensity, L"Интенсивность:", L"Intensity:")
X(LblWind, L"Ветер:", L"Wind:")
X(LblGustiness, L"Порывистость:", L"Gustiness:")
X(LblFlakeSize, L"Размер снежинок:", L"Snowflake size:")
X(LblFallSpeed, L"Скорость падения:", L"Fall speed:")
X(LblOpacity, L"Непрозрачность снежинок:", L"Snowflake opacity:")
X(ChkSkyPerMonitor, L"Свой снегопад на каждом мониторе", L"Snowfall on every monitor")
X(SkyPerMonitorHint, L"Нужно, если мониторы стоят один над другим", L"Needed when monitors are stacked vertically")
X(WindCalm, L"0 (штиль)", L"0 (calm)")
X(GroupUserPresets, L"Мои пресеты", L"My presets")
X(BtnSavePreset, L"Сохранить текущий как…", L"Save current as…")
X(BtnOk, L"OK", L"OK")
X(PresetNameCue, L"Название пресета", L"Preset name")
X(PresetsEmpty, L"Пока пусто — сохраните понравившуюся погоду кнопкой справа", L"Nothing yet — save a look you like with the button on the right")
X(PresetsHint, L"Щелчок — применить, × или Delete — удалить", L"Click to apply; × or Delete removes a preset")
X(ErrPresetLength, L"Введите название от 1 до 40 символов", L"Enter a name of 1 to 40 characters")
X(ErrPresetDuplicate, L"Пресет с таким названием уже есть", L"A preset with this name already exists")
X(ErrPresetTooMany, L"Можно сохранить не больше 20 пресетов", L"You can keep up to 20 presets")
X(PresetSaved, L"Пресет сохранён", L"Preset saved")

// ---- tab «Сугробы» ----------------------------------------------------------------
X(ChkAccumulation, L"Накопление снега", L"Snow accumulation")
X(GroupWhere, L"Где накапливать", L"Where snow settles")
X(ChkOnWindows, L"На окнах", L"On windows")
X(ChkOnTaskbar, L"На панели задач", L"On the taskbar")
X(ChkOnScreenBottom, L"На нижней границе экрана", L"On the bottom edge of the screen")
X(LblMaxHeight, L"Макс. высота сугроба:", L"Max drift height:")
X(GroupShake, L"При резком движении окна", L"When a window is moved sharply")
X(RadioShakeKeep, L"Сохранять", L"Keep the snow")
X(RadioShakeDrop, L"Осыпать", L"Shake it off")
X(ChkSkipMaximized, L"Не накапливать на развёрнутых окнах", L"Do not accumulate on maximized windows")
X(ChkDropOnClose, L"Осыпать снег при закрытии окна", L"Let snow fall when a window closes")
X(LblDriftOpacity, L"Непрозрачность сугробов:", L"Drift opacity:")
X(ChkShakeToClear, L"Стряхивать снег, встряхнув окно", L"Shake a window to clear it")
X(GroupDriftLook, L"Сугробы и сюрпризы", L"Drifts and surprises")
X(ChkNaturalCaps, L"Неровные сугробы", L"Uneven drifts")
X(ChkFallingSnowmen, L"Падающие снеговики", L"Falling snowmen")
X(LblSnowmenInterval, L"Как часто:", L"How often:")
X(SnowmenEveryMinute, L"раз в минуту", L"every minute")
X(SnowmenEvery1Fmt, L"раз в %d минуту", L"every %d min")
X(SnowmenEvery2Fmt, L"раз в %d минуты", L"every %d min")
X(SnowmenEvery5Fmt, L"раз в %d минут", L"every %d min")
X(ChkSnowmenHeavy, L"Только при сильном снегопаде", L"Only in heavy snowfall")
X(LblSnowmenMax, L"Максимум сразу:", L"At most at once:")
X(Snowmen1Fmt, L"%d снеговик", L"%d snowman")
X(Snowmen2Fmt, L"%d снеговика", L"%d snowmen")
X(Snowmen5Fmt, L"%d снеговиков", L"%d snowmen")
X(ChkSnowplow, L"Снегоуборочная машинка на панели задач", L"Snowplow on the taskbar")
X(ChkSnowplowAllTaskbars, L"На каждой панели задач", L"On every taskbar")
X(SnowplowAllTaskbarsHint, L"Соседние панели на одной высоте — один общий маршрут", L"Side-by-side taskbars at the same height share one route")
X(LblSnowplowDelay, L"Выезжает через:", L"Comes out after:")
X(UnitMinFmt, L"%d мин", L"%d min")
X(ChkIcicles, L"Сосульки", L"Icicles")
X(BtnClearAll, L"Очистить всё", L"Clear all")

// ---- tab «Инструменты» -------------------------------------------------------------
X(ChkInteraction, L"Включить взаимодействие", L"Enable interaction")
X(ChkHoldKey, L"Удерживать Alt+Shift для временного режима", L"Hold Alt+Shift for temporary interaction mode")
X(ChkBlockClicks, L"Блокировать клики мимо снега, пока зажаты клавиши режима",
  L"Block clicks off the snow while the mode keys are held")
X(BlockClicksHint, L"Нет выделения рабочего стола при работе инструментом",
  L"No desktop selection rectangle while you use a tool")
X(ChkAllMonitors, L"Снежки и снеговики на всех мониторах", L"Snowballs and snowmen on all monitors")
X(ChkUmbrella, L"Снег обтекает курсор (зонтик)", L"Snow flows around the pointer (umbrella)")
X(GroupTool, L"Инструмент", L"Tool")
X(CaptionTheme, L"Тема", L"Theme")
X(HoldHintFmt, L"Удерживайте %ls, чтобы взаимодействовать со снегом", L"Hold %ls to interact with the snow")
X(HoldHintNone, L"Режим взаимодействия: горячая клавиша или меню значка", L"Interaction mode: use its hotkey or the tray menu")
X(HoldHintShortFmt, L"Удерживайте %ls — и трогайте снег", L"Hold %ls to touch the snow")
X(HoldHintShortNone, L"Включите режим взаимодействия", L"Turn on interaction mode")
X(TipSnowball, L"Снежок: нажмите на сугроб и катите — шар растёт. Бросьте его в окно — шмяк! Положите шар на шар — получится снеговик.",
  L"Snowball: press on a drift and roll — the ball grows. Throw it at a window — splat! Put a ball on another to build a snowman.")
X(TipMitten, L"Варежка: нажмите и держите на сугробе — слепится снеговик. Быстрый взмах смахивает снег.",
  L"Mitten: press and hold on a drift to shape a snowman. A quick swipe brushes the snow away.")
X(TipShovel, L"Лопата: зачерпните снег и швырните — где упадёт, там вырастет сугроб. Медленное движение расчищает снег.",
  L"Shovel: scoop up snow and fling it — a drift grows where it lands. Move slowly to clear the snow.")
X(GroupSkin, L"Скин снеговика", L"Snowman skin")
X(SkinRandom, L"Случайный", L"Random")
X(LblMittenRadius, L"Радиус варежки:", L"Mitten radius:")
X(LblShovelWidth, L"Ширина лопаты:", L"Shovel width:")
X(GroupHelp, L"Как пользоваться", L"How to use")
X(HelpText,
  L"Чтобы трогать снег, удерживайте Alt+Shift или включите режим горячей клавишей либо в меню значка.\n"
  L"• Левая кнопка по сугробу: варежка сметает снег, лопата расчищает полосу; правый щелчок — следующий инструмент.\n"
  L"• Снежок: тяните от сугроба — ком растёт; положите шар на шар, и получится снеговик.\n"
  L"• Снежки и снеговики живут позади окон: на основном мониторе или на всех (флажок выше).",
  L"To touch the snow, hold Alt+Shift or turn the mode on with its hotkey or from the tray menu.\n"
  L"• Left-drag over a drift: the mitten brushes snow away, the shovel clears a strip; right-click picks the next tool.\n"
  L"• Snowball: drag from a drift and the ball grows; drop a ball onto another to build a snowman.\n"
  L"• Snowballs and snowmen live behind windows: on the primary monitor or on all monitors (option above).")

// ---- tab «Производительность» --------------------------------------------------------
X(GroupProfile, L"Профиль качества", L"Quality profile")
X(ProfileLow, L"Экономный", L"Low")
X(ProfileBalanced, L"Сбалансированный", L"Balanced")
X(ProfileHigh, L"Высокий", L"High")
X(GroupFps, L"Частота кадров", L"Frame rate")
X(Fps30, L"30 кадров/с", L"30 FPS")
X(Fps60, L"60 кадров/с", L"60 FPS")
X(ChkPauseFullscreen, L"Пауза при полноэкранных приложениях", L"Pause over fullscreen applications")
X(ChkReduceBattery, L"Снижать качество при работе от батареи", L"Reduce quality on battery power")
X(ChkAnimations, L"Анимации (снимите флажок, чтобы отключить все анимации)", L"Animations (clear to turn all animations off)")
X(ChkDetailedLog, L"Подробный журнал (для поиска проблем)", L"Detailed log (for troubleshooting)")
X(GroupDiagnostics, L"Диагностика", L"Diagnostics")
X(DiagFps, L"Кадров в секунду:", L"Frames per second:")
X(DiagFrame, L"Время кадра:", L"Frame time:")
X(DiagSim, L"Шаг симуляции:", L"Simulation step:")
X(DiagParticles, L"Частицы:", L"Particles:")
X(DiagSurfaces, L"Поверхности:", L"Surfaces:")
X(DiagBalls, L"Снежки / снеговики:", L"Snowballs / snowmen:")
X(DiagMonitors, L"Мониторы:", L"Monitors:")
X(DiagWindows, L"Отслеживаемые окна:", L"Tracked windows:")
X(DiagResets, L"Сбросы устройства:", L"Device resets:")
X(DiagPower, L"Питание:", L"Power:")
X(DiagStatus, L"Состояние:", L"Status:")
X(PowerBattery, L"батарея", L"battery")
X(PowerMains, L"сеть", L"mains")
X(UnitMs, L"мс", L"ms")

// ---- tab «Праздник» ----------------------------------------------------------------------
X(GroupFestive, L"Праздник", L"Festive")
X(ChkGarland, L"Гирлянда на панели задач", L"Garland on the taskbar")
X(ChkFireworks, L"Салют в новогоднюю ночь", L"Fireworks on New Year's Eve")
X(BtnFireworks, L"Салют!", L"Fireworks!")
X(FestiveText, L"Гирлянда мигает вдоль верхнего края панели задач. Салют начнётся сам в полночь 31 декабря и продлится около полутора минут, а кнопка «Салют!» запускает его прямо сейчас.",
  L"The garland twinkles along the top edge of the taskbar. Fireworks start by themselves at midnight on December 31 and last about a minute and a half; the «Fireworks!» button launches them right now.")

// ---- quick panel -------------------------------------------------------------------------
X(QuickIntensity, L"Снег", L"Snow")
X(QuickWind, L"Ветер", L"Wind")
X(QuickNoPresets, L"Свои пресеты сохраняются в настройках", L"Save your own presets in Settings")

// ---- tab «Система» ---------------------------------------------------------------------
X(ChkStartup, L"Запускать вместе с Windows", L"Start with Windows")
X(ChkLeftClick, L"Левый клик по значку — пауза", L"Tray icon left click: pause")
X(LblLanguage, L"Язык интерфейса:", L"Interface language:")
X(LangAuto, L"Авто", L"Auto")
X(LblTheme, L"Оформление:", L"Appearance:")
X(GroupHotkeys, L"Горячие клавиши", L"Hotkeys")
X(LblHotkeyPause, L"Пауза / продолжить:", L"Pause / resume:")
X(LblHotkeyClear, L"Очистить сугробы:", L"Clear drifts:")
X(LblHotkeyInteraction, L"Режим взаимодействия:", L"Interaction mode:")
X(HotkeyHint, L"Щёлкните поле и нажмите сочетание с Ctrl, Alt, Shift или Win. Backspace или Delete — отключить.",
  L"Click a field and press a combination with Ctrl, Alt, Shift or Win. Backspace or Delete disables it.")
X(HotkeyOk, L"✓ Свободно", L"✓ Available")
X(HotkeyDisabled, L"Отключено", L"Disabled")
X(GroupExcluded, L"Исключённые приложения (без снега на их окнах)", L"Excluded applications (no snow on their windows)")
X(LblExcludedList, L"Список исключений:", L"Exclusion list:")
X(LblExeName, L"Имя файла:", L"File name:")
X(ExeCue, L"например, game.exe", L"for example, game.exe")
X(BtnPickRunning, L"Выбрать из запущенных…", L"Choose from running…")
X(PickerTitle, L"Запущенные программы", L"Running programs")
X(PickerHint, L"Программы с открытыми окнами. Выберите одну или несколько (Ctrl, Shift) — их окна останутся без снега.",
  L"Programs with open windows. Select one or more (Ctrl, Shift); their windows will stay free of snow.")
X(PickerEmpty, L"Не найдено программ с видимыми окнами", L"No programs with visible windows were found")
X(BtnCancel, L"Отмена", L"Cancel")
X(LblExclMode, L"Снег у исключённых программ:", L"Snow at excluded apps:")
X(ExclBehind, L"идёт позади их окон", L"falls behind them")
X(ExclAround, L"обходит их окна", L"flows around them")
X(QuickExclusionsFmt, L"Исключения: %d", L"Excluded apps: %d")
X(BtnExcludeApp, L"Исключить приложение…", L"Exclude an app…")
X(BtnAdd, L"Добавить", L"Add")
X(BtnRemove, L"Удалить", L"Remove")

// ---- tab «О программе» -------------------------------------------------------------------
X(PoweredBy, L"Powered by DaveBugg", L"Powered by DaveBugg")
X(AboutVersionFmt, L"Версия %ls", L"Version %ls")
X(AboutDescription,
  L"Интерактивный снег на рабочем столе Windows 11: снежинки падают поверх экрана, оседают на окнах и "
  L"панели задач, а из сугробов можно катать снежки и собирать снеговиков.",
  L"Interactive snow for the Windows 11 desktop: snowflakes fall over the screen, settle on windows and "
  L"the taskbar, and drifts can be rolled into snowballs and snowmen.")
X(GroupPrivacy, L"Конфиденциальность", L"Privacy")
X(AboutPrivacy,
  L"Программа не обращается к сети и не отправляет телеметрию. Она не читает содержимое окон, не делает "
  L"снимков экрана и использует только положение и размеры окон.",
  L"The program never uses the network and sends no telemetry. It does not read window contents or take "
  L"screenshots; it only uses window positions and sizes.")
X(GroupLog, L"Журнал", L"Log")
X(LblLogFile, L"Файл журнала:", L"Log file:")
X(BtnOpenLogFolder, L"Открыть папку журнала", L"Open log folder")
X(AboutLicence,
  L"SnowFall — независимая самостоятельная реализация. Программа не связана с другими продуктами "
  L"со схожими эффектами, не использует их код и графику; все изображения создаются программно.",
  L"SnowFall is an independent implementation. It is not affiliated with other products that offer "
  L"similar effects and uses none of their code or artwork; all images are generated procedurally.")

// ---- macOS variants (the macOS UI prefers <Id>Mac when it exists; Windows never uses them) ----
X(MenuAutostartMac, L"Открывать при входе в систему", L"Open at login")
X(ChkStartupMac, L"Открывать при входе в систему", L"Open at login")
X(ChkOnTaskbarMac, L"На Dock", L"On the Dock")
X(ChkSnowplowMac, L"Снегоуборочная машинка на Dock", L"Snowplow on the Dock")
X(ChkSnowplowAllTaskbarsMac, L"На каждом экране с Dock", L"On every screen with a Dock")
X(SnowplowAllTaskbarsHintMac, L"Dock снизу экрана — маршрут машинки", L"A Dock at the bottom of a screen is a snowplow route")
X(ChkGarlandMac, L"Гирлянда на Dock", L"Garland on the Dock")
X(ChkHoldKeyMac, L"Удерживать Ctrl+Option для временного режима", L"Hold Ctrl+Option for temporary interaction mode")
X(HelpTextMac,
  L"Чтобы трогать снег, удерживайте Ctrl+Option или включите режим горячей клавишей либо в меню значка "
  L"в строке меню. Для этого macOS попросит разрешение «Универсальный доступ».\n"
  L"• Левая кнопка по сугробу: варежка сметает снег, лопата расчищает полосу; правый щелчок — следующий инструмент.\n"
  L"• Снежок: тяните от сугроба — ком растёт; положите шар на шар, и получится снеговик.\n"
  L"• Снежки и снеговики живут позади окон: на основном мониторе или на всех (флажок выше).",
  L"To touch the snow, hold Ctrl+Option or turn the mode on with its hotkey or from the menu-bar icon. "
  L"macOS will ask for the Accessibility permission for this.\n"
  L"• Left-drag over a drift: the mitten brushes snow away, the shovel clears a strip; right-click picks the next tool.\n"
  L"• Snowball: drag from a drift and the ball grows; drop a ball onto another to build a snowman.\n"
  L"• Snowballs and snowmen live behind windows: on the primary monitor or on all monitors (option above).")
X(FestiveTextMac, L"Гирлянда мигает вдоль верхнего края Dock. Салют начнётся сам в полночь 31 декабря и продлится около полутора минут, а кнопка «Салют!» запускает его прямо сейчас.",
  L"The garland twinkles along the top edge of the Dock. Fireworks start by themselves at midnight on December 31 and last about a minute and a half; the «Fireworks!» button launches them right now.")
X(HotkeyHintMac, L"Щёлкните поле и нажмите сочетание с Ctrl, Option, Shift или Cmd. Backspace или Delete — отключить.",
  L"Click a field and press a combination with Ctrl, Option, Shift or Cmd. Backspace or Delete disables it.")
X(PickerHintMac, L"Программы с открытыми окнами. Выберите одну или несколько (Cmd, Shift) — их окна останутся без снега.",
  L"Apps with open windows. Select one or more (Cmd, Shift); their windows will stay free of snow.")
X(AboutDescriptionMac,
  L"Интерактивный снег на рабочем столе macOS: снежинки падают поверх экрана, оседают на окнах и "
  L"Dock, а из сугробов можно катать снежки и собирать снеговиков.",
  L"Interactive snow for the macOS desktop: snowflakes fall over the screen, settle on windows and "
  L"the Dock, and drifts can be rolled into snowballs and snowmen.")
X(AccessibilityNeededTitle, L"Нужно разрешение «Универсальный доступ»", L"Accessibility permission needed")
X(AccessibilityNeededText,
  L"Чтобы снег можно было трогать мышью, SnowFall нужно разрешение «Универсальный доступ». Программа "
  L"перехватывает щелчки только по снегу и только пока включён режим взаимодействия. Откройте «Системные "
  L"настройки → Конфиденциальность и безопасность → Универсальный доступ» и включите SnowFall.",
  L"To let you touch the snow with the mouse, SnowFall needs the Accessibility permission. It only "
  L"intercepts clicks on the snow and only while interaction mode is on. Open System Settings → "
  L"Privacy & Security → Accessibility and turn SnowFall on.")
X(BtnOpenSystemSettings, L"Открыть Системные настройки", L"Open System Settings")
X(ChkHideCursorMac, L"Прятать системный курсор (экспериментально)", L"Hide the system cursor (experimental)")
X(HideCursorHintMac, L"Инструмент заменяет курсор, как на Windows. Если не сработает — инструмент рисуется вокруг курсора.",
  L"The tool replaces the cursor, as on Windows. If it doesn't work, the tool is drawn around the cursor.")
X(CursorReplaceUnavailableMac, L"Не удалось спрятать курсор на этой версии macOS — инструмент рисуется вокруг курсора",
  L"The cursor could not be hidden on this macOS version — the tool is drawn around the cursor")

# IOS-26-Hot for Redmi K20 Pro / Mi 9T Pro (raphael)

[![XNU Kernel CI](https://github.com/timeto2025/IOS-26-Hot-raphael/actions/workflows/xnu.yml/badge.svg)](https://github.com/timeto2025/IOS-26-Hot-raphael/actions/workflows/xnu.yml)
[![iOS Userland & Metal2 CI](https://github.com/timeto2025/IOS-26-Hot-raphael/actions/workflows/userland.yml/badge.svg)](https://github.com/timeto2025/IOS-26-Hot-raphael/actions/workflows/userland.yml)

Порт [IOS-26-Hot](https://github.com/nithinyadavamaraboina-online/IOS-26-Hot) на **Redmi K20 Pro / Mi 9T Pro
(`raphael`, Snapdragon 855 `SM8150`, Adreno 640)**: iOS 26.5.2 (`23F84`, iPhone 11 Pro `D421AP`, `arm64e`) на ядре
`xnu-12377.121.6` с патчами этого порта, без Android.

Исходный проект делался для Realme X2 Pro (`RMX1931`) и Pixel 4 (`flame`); его описание — в
[README-UPSTREAM.md](README-UPSTREAM.md). Здесь — только то, что относится к raphael.

> [!CAUTION]
> **Экспериментальная сборка, не для повседневного использования.** Корневая система iOS записывается в раздел
> `darwin` (разметка HackiOS) и затирает его. Защита Apple (SEP, подпись кода, PAC) заменена заглушками.
> Разделы загрузчика и прошивок (`abl`, `xbl`, `tz`, `modem`, `persist`) **не трогать никогда**, загрузчик не
> блокировать. Перед началом сохраните копии разделов (`tools/raphael-extract-fw.sh` кладёт их в `backup/`,
> в git они не попадают: там IMEI и калибровки радио).

## Состояние (ноябрь 2026, ядро `0187`)

| Подсистема | Состояние | Подробности |
| --- | --- | --- |
| Загрузка | **Работает** | Образ с AVB-подписью в разделе `recovery` (128 МиБ, `tools/raphael-move-recovery.py`), вход — питание + громкость вверх; корень HFS+ в `darwin`, запись разрешена |
| Процессор | **Работает** | Все 8 ядер Kryo 485, раздельные кластеры E/P, управление частотой (OSM) |
| Экран | **Работает** | AMOLED 1080×2340, холст iOS 1125×2436, яркость из ползунка iOS (DCS `0x51`), смена кадров по TE; с `0187` два готовых кадра ждут своей очереди, а не теряются |
| Сенсор | **Работает** | Goodix GT9886 (собственный драйвер ядра, `0153`–`0154`) |
| GPU / Metal | **Работает, медленно** | `HotMetal2` на фронтенде [AdrenoMetal](https://github.com/UniXNU/AdrenoMetal) (ветка `ci/adrenometal`) + `metal2vulkan` + Mesa Turnip (UniXNU `d8210e0`, интерфейс ядра v2); GPU до 585 МГц; 24–59 кадров/с, при нагрузке подтормаживает |
| Звук | **Работает** | ADSP (`adspd`) + усилитель TFA9874 + `HotAudio.driver`; треск устранён таймером 24 МГц для iOS (`0183`, `sweettimebase=24000000`) |
| Wi-Fi | **Работает** | WCN3990 через модем (`wland`), WPA2, настройки iOS |
| Батарея | **Частично** | Заряд, напряжение, ток и зарядка видны; температура не читается (всегда 25 °C) |
| Bluetooth | **Частично** | Включается; поиск устройств LE контроллер отклоняет |
| Вибрация | **Частично** | Мотор в ядре (`0160`); щелчки клавиатуры iOS не работают (`hapticd`) |
| Часы | **Частично** | Часы PMIC сбрасываются при полной разрядке; тогда время идёт от момента сборки образа (`0184`); NTP нет |
| Сеть по USB | **Не работает** | Отладка — по Wi-Fi: `tools/hot-dbg.py` (TCP 2223) и `ssh root@<IP>` |
| Сотовая связь, камера, GPS | **Не сделано** | — |

Текущие задачи и журнал работы — в [HANDOFF.md](HANDOFF.md), раздел «Start here», новые записи сверху.

## Что нужно

- Телефон с разблокированным загрузчиком и разметкой HackiOS (раздел `darwin`). Используется пакет
  `HackiOS-16.5-raphael-voxelsy`: из него берётся `images/twrp-original.img` как основа загрузочного образа.
- Linux (проверено на Arch): `python3`, `clang`, `lld`, `llvm`, `android-tools` (`fastboot`, `adb`), `gh`.
- Свой IPSW iOS 26.5.2 (`23F84`) для iPhone 11 Pro и собранный из него корневой образ (см.
  [README-UPSTREAM.md](README-UPSTREAM.md), раздел про корневой образ).
- Прошивки с самого телефона: `tools/raphael-extract-fw.sh` (модем, Wi-Fi, NV), прошивка GPU (`GPUFW=`).
  В git их нет и быть не должно.

## Сборка и прошивка

Ядро и userland собирает GitHub Actions (нужен macOS): workflow **xnu** (ядро, 10–30 минут) и **userland**
(`HotMetal2`, Turnip). Артефакты скачиваются через `gh run download`.

Корень один раз пишется в `darwin` из TWRP: `tools/raphael-write-darwin.py ~/ios26-root-raphael.img`
(кусками по 32 МиБ с проверкой; повторный запуск дописывает только отличия).

Загрузочный образ и прошивка (телефон в fastboot):

```
cd ~/IOS-26-Hot-raphael && GPUABI=2 METAL2_FROM=<ios-userland.tar.gz из ci/adrenometal> EXTRA_ARGS="raphgpool=324" REDUCE_FX=1 AUDIO=1 BT_ADDR=<адрес BT> WLAN_CONF=~/wlan.conf WIFI=1 EXTRA_USERLAND=<raphael-tools.tar.gz> GPUFW=~/raphael-gpufw tools/raphael-ios-boot.sh <kernel.development.sweet> ~/Downloads/ios-userland-hot-v0.8.1.tar.gz ~/ios26-root-raphael.img ~/Downloads/HackiOS-16.5-raphael-voxelsy-04112026-V2/images/twrp-original.img 2>&1 | tee ~/build.log && fastboot flash recovery shim/out/raphael-ios-avb.img && fastboot reboot recovery
```

Основные параметры `tools/raphael-ios-boot.sh` (полный список — в начале скрипта):

| Параметр | Что делает |
| --- | --- |
| `WIFI=1`, `WLAN_CONF=` | Wi-Fi и файл с сетью (в git не класть) |
| `AUDIO=1` | Звук |
| `BT_ADDR=` | Адрес Bluetooth (в git не класть) |
| `METAL2_FROM=` | Взять `HotMetal2.bundle` из другого архива userland |
| `GPUABI=1\|2` | Версия интерфейса GPU: `2` для Turnip из `ci/adrenometal`, `1` для старого |
| `GPUFREQ=1..6` | Ступень частоты GPU (`5` = 585 МГц, максимум SM8150) |
| `TIMEBASE24=0` | Отключить таймер 24 МГц для iOS |
| `MTL2_ENV=` | Переменные для HotMetal2 (по умолчанию `TU_DARWIN_BO_CACHE=0`) |
| `EXTRA_ARGS=` | Дополнительные параметры загрузки ядра |

`fastboot boot` для iOS не использовать: только `fastboot flash recovery`.

## Диагностика

- Один архив всех логов: `tools/raphael-logs.sh <IP телефона>` → `~/raphael-logs-ДАТА.tar.gz`.
- Журнал ядра и состояние: `python3 tools/hot-dbg.py connect <IP>`, затем `status`, `doctor`, `crashes`, `klog --all`.
- `ssh root@<IP>`: в системе нет `ls`, `cat`, `scp`; файлы читаются через `hot-dbg.py` или `while read`.

## Устройство репозитория

| Путь | Что там |
| --- | --- |
| `xnu/patches-26/` | Патчи ядра по порядку; для raphael — с `0152` |
| `tools/raphael-*` | Сборка образа, запись корня, прошивки, логи, часы, разметка `recovery` |
| `tools/xnu-tree.sh` | Локальное дерево ядра со всеми патчами (для новых патчей) |
| `tools/clone-mirrors.sh` | Зеркала 76 похожих проектов ([docs/prior-art.md](docs/prior-art.md), [список](docs/prior-art-repos.txt)) |
| `phone/` | Демоны и библиотеки iOS: `metal2`, `turnip`, `wifi`, `adsp`, `audio`, `bt`, `tools` |
| [HANDOFF.md](HANDOFF.md), [NOTES.md](NOTES.md) | Журнал работы и аппаратные заметки |
| [docs/raphael-bringup-2026-11.md](docs/raphael-bringup-2026-11.md) | Первые шаги порта: проба shim, первое ядро, сенсор, отличия от RMX1931 |
| [CLAUDE.md](CLAUDE.md) | Правила для работы в репозитории |

Ветки: `master` — то, что проверено на телефоне; `ci/adrenometal` — HotMetal2 на AdrenoMetal (собирается
отдельно, ещё не влита).

## Авторы и лицензии

Основано на [IOS-26-Hot](https://github.com/nithinyadavamaraboina-online/IOS-26-Hot) и проектах, перечисленных в
[README-UPSTREAM.md](README-UPSTREAM.md) и [CREDITS.md](CREDITS.md). Для raphael взято также:
[AdrenoMetal](https://github.com/UniXNU/AdrenoMetal) (MIT) и патчи Turnip из [UniXNU](https://github.com/UniXNU/UniXNU),
исправление часов CoreAudio из [UniXNU-sheng](https://github.com/UniXNU/UniXNU-sheng) (BSD-2-Clause-Patent),
таймер 24 МГц из [UniXNU-droidvm](https://github.com/Droid-VM/UniXNU-droidvm), разметка и TWRP из пакета HackiOS для
raphael. Файлов Apple, Qualcomm и Xiaomi в репозитории нет.

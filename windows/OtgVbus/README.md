# OtgVbus — удержание OTG VBUS под Windows (raphael)

**Статус: UNTESTED.** Собирается в CI, на устройстве ещё не проверялся.

## Зачем

UEFI-драйвер (`uefi/RaphaelOtgLatchDxe`) доказал, что VBUS можно включить: по логу
`OTG_EN=1 / VBUS LATCHED ON`, и подсветка клавиатуры горит на логотипе UEFI. Но при
загрузке Windows закрытый драйвер Qualcomm (`qcpmictcc` / `ACPI\QCOM0582`) снимает
`OTG_EN` при инициализации — VBUS гаснет на экране входа. Host-режим (переключатель
ролей) уже работает, не хватает только VBUS.

`OtgVbus.sys` — крошечный kernel-драйвер, который **работает рядом** с закрытым
драйвером (не заменяет его) и **переподтверждает `OTG_EN`** каждую секунду прямой
записью в PMIC-arb v5 по той же последовательности, что проверена в UEFI (APID 267:
chnls `0x0D6B0000`, obsrvr `0x0E608580`; `CMD_OTG` off 0x40, бит `OTG_EN`).

## Сборка

Собирается в CI (workflow `otgvbus`, Windows-раннер, ARM64) → артефакт
`OtgVbus-arm64` с `OtgVbus.sys`. Скачать так же, как UEFI-артефакт.

## Установка — offline из Arch (клавиатура в Windows не нужна)

1. Смонтировать раздел `win` (NTFS) из Arch (как в прошлых правках реестра):
   ```
   sudo mount -t ntfs3 -o force /dev/disk/by-partlabel/win /mnt/win
   ```
2. Скопировать драйвер:
   ```
   sudo cp OtgVbus.sys /mnt/win/Windows/System32/drivers/OtgVbus.sys
   ```
3. Добавить службу в куст `SYSTEM` (активный ControlSet; его номер — в
   `Select\Current`, обычно `ControlSet001`). Значения службы
   `SYSTEM\ControlSet001\Services\OtgVbus`:
   | имя | тип | значение |
   |---|---|---|
   | `Type` | REG_DWORD | `1` (kernel) |
   | `Start` | REG_DWORD | `1` (system-start) |
   | `ErrorControl` | REG_DWORD | `0` |
   | `ImagePath` | REG_EXPAND_SZ | `system32\drivers\OtgVbus.sys` |

   Пример через `hivexregedit` (как в winusb.py-потоке):
   ```
   cat > /tmp/otg.reg <<'REG'
   [\ControlSet001\Services\OtgVbus]
   "Type"=dword:00000001
   "Start"=dword:00000001
   "ErrorControl"=dword:00000000
   "ImagePath"=hex(2):73,00,79,00,73,00,74,00,65,00,6d,00,33,00,32,00,5c,00,64,00,72,00,69,00,76,00,65,00,72,00,73,00,5c,00,4f,00,74,00,67,00,56,00,62,00,75,00,73,00,2e,00,73,00,79,00,73,00,00,00
   REG
   sudo hivexregedit --merge --prefix 'HKEY_LOCAL_MACHINE\SYSTEM' /mnt/win/Windows/System32/config/SYSTEM /tmp/otg.reg
   sudo umount /mnt/win
   ```
   (`ImagePath` выше — это строка `system32\drivers\OtgVbus.sys` в UTF-16LE.)
4. Загрузиться в Windows — драйвер стартует сам.

Для загрузки неподписанного драйвера на устройстве уже включены `testsigning` и
`nointegritychecks` (см. HANDOFF) — отдельная подпись не нужна.

## Установка — из самой Windows (если есть ввод)
```
sc create OtgVbus type= kernel start= demand binPath= C:\Windows\System32\drivers\OtgVbus.sys
sc start OtgVbus
```

## Проверка

- **Главный признак:** подсветка подключённой клавиатуры **остаётся гореть на экране
  входа** (раньше гасла). Значит `OTG_EN` держится, VBUS не падает.
- Воткнуть флешку — определяется ли в проводнике.
- Логи драйвера (`OTGVBUS: …`) видны в DebugView (режим ядра) или в WinDbg.

## rev2: счётчики в реестре (диагностика пинг-понга)

rev2 переподтверждает `OTG_EN` каждые 100 мс и раз в ~2 с пишет счётчики в свой
ключ службы. После тестовой сессии прочитать их offline из Arch:
```
sudo mount -t ntfs3 -o force <win> /mnt/win
for v in St_Loops St_FoundCleared St_WriteOk St_WriteFail St_LastCmdOtg St_LastOtgCfg; do
  printf '%-16s = ' "$v"
  sudo hivexget /mnt/win/Windows/System32/config/SYSTEM '\ControlSet001\Services\OtgVbus' "$v"
  echo
done
```
Как читать:
- `St_FoundCleared` близко к `St_Loops` (почти каждый цикл находил бит сброшенным)
  → Windows гасит `OTG_EN` **постоянно**, опросом гонку не выиграть — нужен другой
  подход (нейтрализовать само сбрасывание в `qcpmictcc`).
- `St_FoundCleared` мало относительно `St_Loops` → гасит **по событиям**, частое
  переподтверждение должно удержать (можно ещё уменьшить период).
- `St_WriteFail` > 0 → конфликт за шину SPMI (запись не проходит).
- `St_LastCmdOtg` бит0 — последнее наблюдённое значение `OTG_EN`.

## Риски

1. **Гонка с `qcpmictcc`** за канал PMIC-arb. Арбитр сериализует каждую команду, но
   запись, попавшая в середину транзакции чужого драйвера, может её сбить. Окно —
   микросекунды, пишем ~раз в секунду. Если будет нестабильность — увеличить период.
2. **Пинг-понг:** если Windows гасит `OTG_EN` непрерывно, а не один раз — VBUS будет
   мигать (драйвер возвращает бит раз в секунду). Если один раз при инициализации —
   наше переподтверждение удержит.
3. **Не заряжать телефон через порт, пока VBUS удерживается** — два источника 5 В.
4. UNTESTED до первой загрузки.

## Откат

`sc delete OtgVbus` (или удалить ключ службы из куста offline) + удалить `.sys`.
Драйвер не трогает разделы и ничего не пишет на диск, кроме себя.

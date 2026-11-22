# Сборка

## Официальный путь (рекомендуется)

Linux/macOS:

```bash
# 1. devkitPro pacman
wget https://apt.devkitpro.org/install-devkitpro-pacman
sudo bash ./install-devkitpro-pacman
sudo dkp-pacman -Syu

# 2. Switch toolchain + portlibs
sudo dkp-pacman -S switch-dev switch-mbedtls switch-ffmpeg

# 3. Окружение
export DEVKITPRO=/opt/devkitpro
export PATH=$DEVKITPRO/tools/bin:$DEVKITPRO/devkitA64/bin:$PATH

# 4. Сборка
make -j$(nproc)
```

Появится `vkvideo.nro`.

Windows: ставится MSYS2-пакетом devkitPro, далее те же `dkp-pacman`-команды.

## Локальный toolchain без root

Если нет прав на `/opt` или репозиторий devkitPro недоступен (Cloudflare),
`tools/setup-toolchain.sh` собирает эквивалент в `~/devkitpro-local`:

```bash
./tools/setup-toolchain.sh
export DEVKITPRO=$HOME/devkitpro-local
export PATH=$DEVKITPRO/tools/bin:$DEVKITPRO/devkitA64/bin:$PATH
make -j$(nproc)
```

Что делает скрипт:

1. Качает ARM GNU Toolchain `aarch64-none-elf` (xPack, GCC 13.2).
2. Собирает **newlib из devkitPro-форка** с патчем devkitPro — без него нет
   `sys/iosupport.h`, devoptab, динамического `__getreent` и lock-обёрток,
   на которых держится libnx.
3. Кладёт shim `devkitA64/base_rules` + `bin2o` в стиле devkitPro.
4. Собирает `libnx` и правит `switch.specs` (в стоковом binutils нет
   `--nx-module-name` и `-z pack-relative-relocs`).
5. Собирает host-утилиты `elf2nro`, `nacptool`.
6. Кросс-собирает `mbedTLS 2.28.10` с патчем devkitPro (энтропия через
   `csrng`, таймеры через `armGetSystemTick`).
7. Кросс-собирает `ffmpeg 7.1` с патчами devkitPro: только `mov`-демуксер,
   декодеры H.264/AAC/MP3, `swscale`, `swresample`, без сети. Это даёт
   software-плеер; `nvtegra`/NVDEC собирается, но в плеере не используется —
   software-путь стабильнее на контенте VK.

Требования: `git`, `curl`, `python3`, `make`, `cc`, `dpkg-deb` (для `lz4.h`,
если его нет в системе).

UI-зависимости вендорены в репозиторий и не требуют отдельных пакетов:
`stb_image`, `stb_truetype` и шрифт `data/font.bin` (DejaVu Sans, лицензия в
`docs/LICENSE-DejaVu.txt`).

## Особенности путей с пробелами

Makefile использует относительные пути и маркер `VK_BUILD=1` вместо
`$(CURDIR)`, потому что каталог `Проект по умолчанию` содержит пробелы, а
функции make (`notdir`, `wildcard`) режут такие пути по пробелам.

## Запуск на консоли

1. Скопируйте `vkvideo.nro` в `/switch/` на SD.
2. Запустите hbmenu (Album с зажатым R или через Atmosphere).
3. Создайте `/switch/vkvideo/config.ini` (см. README).

Отладка по сети:

```bash
nxlink -s vkvideo.nro
```

`nxlink` слушает stdout приложения; удобно для логов TLS/API.

## Тесты

```bash
./tools/test-host.sh
```

Компилирует только `src/vk.c` + cJSON обычным `cc` и проверяет разбор ссылок,
JSON и выбор качества. Плеер и сеть в хостовые тесты не входят.

## Траблшутинг: краш до появления окна

Симптом: чёрный экран / `2168-0002`, в `atmosphere/crash_reports` процесс
`hbloader`, Data Abort, PC в `_dynProcessRela` (`dynamic.c`).

Причина: `const`-объекты с указателями (`_C_time_locale`, `dotab_stdnull` и
подобные) попали в read-only `.rodata`, а libnx при старте пишет туда
релокации. Лечение:

1. newlib пересобран с этими объектами без `const` (см. патчи в
   `tools/setup-toolchain.sh`).
2. В `switch.specs` оставлен `-z text`: если read-only релокации появятся
   снова, линковка упадёт с понятной ошибкой, а не соберёт падающий NRO.

Проверка после сборки:

```bash
aarch64-none-elf-readelf -rW build/vkvideo.elf | grep RELATIVE | head
```

Смещения (`Offset`) должны лежать в RW-сегменте (`.data.rel.ro`/`.data`,
для текущего layout — `0x3f2000` и выше). Если линковка с `-z text` прошла,
read-only релокаций нет.

Диагностика приложения пишется в `/switch/vkvideo/log.txt` (стадии boot,
config, ui, feed, video.get, download, player).

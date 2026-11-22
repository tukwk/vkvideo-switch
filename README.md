# VK Video для Nintendo Switch

Homebrew-клиент VK Видео для прошитой Nintendo Switch (Atmosphere / hbmenu).
Нативный NRO на C: лента с обложками, категории, поиск, потоковое
воспроизведение с перемоткой, историей и сохранением позиции.

> Статус: **v0.4 — стриминг, перемотка, история**. Один файл `vkvideo.nro`,
> запуск в full-memory режиме. Звук через `audout`, видео через ffmpeg
> (software H.264). На Switch Lite рекомендуется 720p.

## Возможности

- Лента, категории и поиск по VK Видео (`video.getCatalog`,
  `video.getCatalogSection`, `video.search`).
- Обложки с LRU-кэшем, интерфейс на framebuffer 1280×720.
- Прогрессивный HTTPS-стриминг без скачивания на SD: кольцевой буфер 48 МБ,
  `Range`-перемотка, автоматическое переподключение при обрыве CDN.
- Пауза, перемотка ±10 с и ±60 с, громкость, OSD.
- История просмотра и позиция «продолжить с места» на SD.
- Выбор качества (360/480/720; 1080 скрыт в handheld) и ручное переключение.
- Пауза при уходе в сон / home menu и продолжение при возврате.

## Установка

Нужен только файл `vkvideo.nro`.

1. Скопируй его в `/switch/` на SD через MTP (DBI), FTP или картридер.
2. Запусти hbmenu. Для ввода токена с экранной клавиатуры нужен
   **full-memory режим**: зажми `R` и запусти любую игру. В applet-режиме
   (из Album) swkbd может не работать — тогда положи `config.ini` через MTP.
3. Нажми `Y` и вставь токен VK.
4. Дальше — лента, категории, поиск. Ссылки руками вставлять не нужно.

Каталог `/switch/vkvideo/` создаётся сам (конфиг, история, позиции, лог).

### Токен

Нужен доступ к `video`. Быстрее всего:

1. <https://vkhost.github.io/>
2. Kate Mobile / VK Admin → разрешить видео.
3. Скопировать `access_token`, ввести на консоли (`Y`).



## Управление

### Лента

```
VK VIDEO   [Рекомендации] [Музыка] [Игры] [Фильмы] ...
┌──────────┐ ┌──────────┐ ┌──────────┐
│ обложка  │ │ обложка  │ │ обложка  │
│ название │ │ название │ │ название  │
└──────────┘ └──────────┘ └──────────┘
A — смотреть  X — поиск  Y — токен  ZL/ZR — категория  L/R — страница  + — выход
```

| Кнопка | Действие |
|---|---|
| D-pad | выбор карточки |
| `A` | смотреть |
| `ZL` / `ZR` | категория (Рекомендации, Музыка, Игры, …) |
| `L` / `R` | предыдущая / следующая страница |
| `X` | поиск по тексту |
| `Y` | ввести/сменить токен |
| `-` | качество: 360 → 480 → 720 |
| `+` | выход |

### Плеер

| Кнопка | Действие |
|---|---|
| `A` | пауза / продолжить |
| `←` / `→` | перемотка −10 / +10 с |
| `L` / `R` | перемотка −60 / +60 с |
| `↑` / `↓` | громкость |
| `B` / `+` | остановить и вернуться в ленту |

## Конфиг

`sdmc:/switch/vkvideo/config.ini` (создаётся сам):

```ini
token=vk1.a.xxxxxxxx
last=
quality=720
```

История и позиции:

- `/switch/vkvideo/history.txt` — недавно просмотренные;
- `/switch/vkvideo/positions.txt` — позиция для продолжения;
- `/switch/vkvideo/log.txt` — только важные события (без постоянной записи).

## Как это работает

1. `video.search` / `video.getCatalog*` → список видео и URL обложек.
2. UI: framebuffer 1280×720, DejaVu Sans (stb_truetype), сетка 3×2.
3. По `A`: `video.get` → прямая HTTPS-ссылка `mp4_*` (fallback по качеству).
4. `src/stream.c`: фоновый поток качает поток в кольцевой буфер 48 МБ,
   поддерживает `Range`, паузу при переполнении и реконнект без потери позиции.
5. ffmpeg читает контейнер через кастомный AVIO; H.264 декодируется в
   4 frame-thread'а, `swscale` → RGBA → framebuffer с letterbox.
6. AAC → `swresample` → 48 кГц s16 stereo → `audout` (6 буферов).
7. Видео синхронизируется по счётчику проигранных сэмплов; при недогрузке
   звука часы переходят на системное время, чтобы картинка не замирала.
8. После перемотки аудио и видео обрезаются ровно по целевой позиции
   (кейфрейм не сдвигает A/V).
9. Watchdog: если картинка стоит несколько секунд, плеер сам перезапускает
   конвейер с текущей позиции.

## Сборка

Официальный devkitPro:

```bash
sudo dkp-pacman -S switch-dev switch-mbedtls switch-ffmpeg
export DEVKITPRO=/opt/devkitpro
export PATH=$DEVKITPRO/tools/bin:$DEVKITPRO/devkitA64/bin:$PATH
make -j$(nproc)
```

Без root — `tools/setup-toolchain.sh` (GCC + newlib + libnx + mbedTLS +
ffmpeg), см. `docs/BUILD.md`.

Тесты: `./tools/test-host.sh` (ядро VK + парсер ленты).

## Структура

```
src/main.c        цикл UI, ввод, лента, запуск плеера, история
src/ui.c          framebuffer UI: текст, карточки, обложки, загрузка
src/feed.c        video.search / video.getCatalog*, разбор JSON
src/vk.c          ссылки, video.get, выбор качества
src/net.c         HTTPS-клиент API (mbedTLS)
src/http.c        HTTP-клиент API и картинок
src/stream.c      потоковый HTTP(S) с Range и кольцевым буфером
src/player.c      ffmpeg + framebuffer + audout, seek, синхронизация
src/history.c     история просмотра
src/resume.c      позиции «продолжить»
src/config.c      config.ini
src/log.c         лог на SD
src/vendor/       cJSON, stb_image, stb_truetype
data/font.bin     DejaVu Sans (см. docs/LICENSE-DejaVu.txt)
tests/            хостовые тесты
tools/            toolchain и тесты
docs/             архитектура, сборка, roadmap
```

## Ограничения

- 720p — практический максимум; 480p даёт самый ровный результат.
- Только прогрессивный MP4; HLS, лайвы и внешние встраивания не поддержаны.
- NVDEC (`h264_nvtegra`) не используется: software-путь стабильнее.
- DRM/Widevine не поддерживается.
- Рекомендации зависят от того, что VK отдаёт `video.getCatalog` вашему токену.

## Лицензия

Код — MIT (см. `LICENSE`). cJSON — MIT. stb — public domain/MIT.
ffmpeg — LGPL 2.1+. DejaVu Sans — свободная лицензия Bitstream Vera
(см. `docs/LICENSE-DejaVu.txt`).

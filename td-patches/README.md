# Патчи TDLib

Здесь лежат изменения исходников TDLib (`td/`), которые нужны серверу. Submodule `td` указывает на
оригинальный коммит TDLib, а патчи применяются к нему при сборке.

| Патч | Что делает |
|---|---|
| `0001-memory-object-counts.patch` | Метод `getMemoryObjectCounts`: число файлов, чатов, сообщений, пользователей, медиа и т. п. в памяти инстанса. Разрешает ботам `getStorageStatisticsFast`. Нужен для метрик `tgbotapi_bot_tdlib_objects` и `tgbotapi_bot_storage_*` (`METRICS.md`) |

Патч только читает размеры контейнеров и ведёт один счётчик загруженных сообщений. Поведение TDLib
он не меняет.

## Как применяется

CMake на этапе конфигурации применяет каждый `*.patch` по порядку через `git apply`. Уже
применённый патч пропускается, поэтому повторный запуск CMake ничего не меняет. Если патч не
накладывается, конфигурация останавливается с ошибкой.

После этого `git status` показывает submodule `td` как изменённый — так и должно быть.

Нужен `git`. Если его нет в окружении сборки, примените патчи вручную и передайте CMake
`-DTELEGRAM_BOT_API_APPLY_TD_PATCHES=OFF`:

```
cd td && git apply ../td-patches/*.patch
```

## Обновление TDLib

1. Откатите патчи: `git -C td checkout .`
2. Обновите submodule, как обычно.
3. Запустите CMake. Если патч не наложился, поправьте изменения в `td/` вручную и пересоздайте патч,
   сохранив описание в начале файла:

   ```
   git -C td diff > /tmp/diff && { sed '/^diff --git/,$d' td-patches/0001-memory-object-counts.patch; cat /tmp/diff; } > /tmp/patch \
     && mv /tmp/patch td-patches/0001-memory-object-counts.patch
   ```

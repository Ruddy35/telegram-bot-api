# Метрики по ботам

Сервер отдаёт подробные метрики по каждому боту в формате Prometheus. Их можно собирать
VictoriaMetrics (vmagent) или Prometheus и строить по ним отчёты и индекс нагрузки.

## Как получить

Метрики доступны на порту статистики (`--http-stat-port`):

```
curl http://127.0.0.1:<stat-port>/metrics            # все боты и общие метрики сервера
curl http://127.0.0.1:<stat-port>/metrics?bot=<id>   # только один бот
```

`<id>` — числовая часть токена до двоеточия. В метках используется только этот id, токены в
метрики не попадают. Порт статистики должен быть доступен только из внутренней сети.

Пример для vmagent:

```yaml
scrape_configs:
  - job_name: telegram-bot-api
    scrape_interval: 15s
    static_configs:
      - targets: ['bot-api-host:<stat-port>']
```

## Как это устроено и почему ничего не теряется

Все события считаются **накопительными счётчиками**. Каждый запрос, апдейт, доставка вебхука и
скачивание увеличивает счётчик в памяти сервера в момент события. vmagent периодически забирает
текущие значения, а `increase()`/`rate()` в MetricsQL дают точное число событий за любой интервал.
События между опросами не теряются.

- Счётчики бота хранятся в `ClientManager`, а не в TDLib-инстансе, поэтому горячий рестарт
  инстанса (`MEMORY.md`) их не сбрасывает.
- Счётчики удаляются вместе с записью бота: после `logOut`, `close` или остановки неактивного
  клиента. Если бот вернётся, отсчёт начнётся заново. Такие сбросы, как и рестарт процесса,
  `increase()` обрабатывает корректно.
- Потерять можно только то, что произошло между последним опросом и аварийным падением процесса.
- Метрики по ботам отдаются только для ботов, которые хоть раз авторизовались. Запросы с
  невалидными токенами собираются в общие счётчики, чтобы мусорные токены не плодили ряды.

Все счётчики инкрементируются в одном потоке без блокировок. Это несколько десятков наносекунд на
запрос при стоимости самого запроса в десятки и сотни микросекунд. Значения памяти и диска
считаются только в момент опроса или раз в минуту.

## Метрики бота

У всех метрик ниже есть метка `bot="<id>"`. Для ботов тестового DC id имеет суффикс `_test`.

### Запросы

| Метрика | Метки | Что это |
|---|---|---|
| `tgbotapi_bot_requests_total` | `method`, `code`, `reason` | Отвеченные запросы по методу, HTTP-коду и причине отказа |
| `tgbotapi_bot_method_duration_seconds_total` | `method` | Суммарное время от получения запроса до ответа. Делите на `requests_total`, чтобы получить среднее |
| `tgbotapi_bot_request_duration_seconds` | `le` | Гистограмма времени ответа по всем методам бота |
| `tgbotapi_bot_request_bytes_total` | `method` | Объём запросов, включая файлы |
| `tgbotapi_bot_response_bytes_total` | `method` | Объём ответов |
| `tgbotapi_bot_request_files_total`, `tgbotapi_bot_request_file_bytes_total` | `method` | Число и объём файлов, загруженных в запросах (multipart) |
| `tgbotapi_bot_abandoned_requests_total` | `method` | Запросы, клиент которых закрыл соединение, не дождавшись ответа |
| `tgbotapi_bot_active_requests` | | Запросы в обработке сейчас |

`method` — имя метода в нижнем регистре. Несуществующие методы считаются как `unknown`, чтобы бот
не мог раздуть число рядов.

Значения `reason`:

| `reason` | Когда |
|---|---|
| `none` | Обычный ответ, в том числе обычные ошибки 400/403 |
| `telegram_flood` | 429 от серверов Telegram (`FLOOD_WAIT`) |
| `server_flood` | 429 от защиты самого сервера: слишком много активных запросов или загрузок |
| `set_webhook_flood` | 429 за слишком частые `setWebhook` |
| `close_flood` | 429 за `close` в первые 10 минут |
| `closing` | Запрос прерван: инстанс бота закрывается или выходит из аккаунта |
| `restart_queue_full` | 429: переполнилась очередь отложенных запросов на время рестарта |
| `restart_timeout` | 429: рестарт длился слишком долго |
| `dropped` | Запрос был уничтожен без ответа: сервер останавливается или клиент бота уже удалён |

### Файлы, которые отправляет бот

| Метрика | Метки | Что это |
|---|---|---|
| `tgbotapi_bot_input_files_total` | `method`, `type` | Файлы в запросах по способу передачи |
| `tgbotapi_bot_input_file_bytes_total` | `method`, `type` | Их объём, известен для `upload` и `local_path` |

Значения `type`:
- `upload` — файл загружен в запросе;
- `local_path` — путь к файлу на сервере (`--local`);
- `file_id` — повторное использование уже загруженного файла;
- `url` — ссылка. Такой файл скачивают серверы Telegram, а не Bot API.

### Скачивание файлов (`getFile`)

| Метрика | Метки | Что это |
|---|---|---|
| `tgbotapi_bot_file_downloads_started_total` | | Начатые скачивания из Telegram |
| `tgbotapi_bot_file_downloads_total` | `result` | Завершённые попытки скачивания по исходу |
| `tgbotapi_bot_downloaded_bytes_total` | | Объём полностью скачанных файлов |
| `tgbotapi_bot_partially_downloaded_bytes_total` | | Сколько успело скачаться у прерванных скачиваний |
| `tgbotapi_bot_active_file_downloads` | | Файлы, которые скачиваются сейчас |

Каждая попытка учитывается один раз, даже если файл одновременно ждут несколько запросов `getFile`.

Значения `result`:
- `completed` — файл скачан;
- `cached` — файл уже был на диске;
- `too_big` — больше 20 МБ без `--local`;
- `error` — ошибка Telegram или неверный `file_id`;
- `closing` — прервано закрытием инстанса;
- `aborted` — инстанс закрылся, а скачивание так и не завершилось.

Удаление файлов с диска (оптимизатором TDLib или вручную) видно по уменьшению
`tgbotapi_bot_storage_bytes{type="files"}` и `tgbotapi_bot_storage_files`.

### Апдейты и вебхук

| Метрика | Метки | Что это |
|---|---|---|
| `tgbotapi_bot_updates_total` | `type`, `result` | Апдейты по типу (`message`, `callback_query`, ...) и исходу |
| `tgbotapi_bot_fetched_updates_total` | | Апдейты, выданные боту через `getUpdates` |
| `tgbotapi_bot_pending_updates` | | Апдейты в очереди, ещё не полученные ботом |
| `tgbotapi_bot_webhook_deliveries_total` | `result` | Отправки апдейтов на вебхук по исходу |
| `tgbotapi_bot_webhook_response_seconds_total` | | Суммарное время ожидания ответа вебхука |
| `tgbotapi_bot_webhook_connection_errors_total` | | Неудачные подключения к вебхуку |

Значения `result` для `tgbotapi_bot_updates_total`:
- `queued` — апдейт поставлен в очередь;
- `filtered` — отброшен по `allowed_updates`;
- `failed` — не удалось поставить в очередь.

Значения `result` для `tgbotapi_bot_webhook_deliveries_total`:
- `ok` — бот ответил 2xx;
- `http_error` — бот ответил ошибкой;
- `connection_error` — соединение оборвалось;
- `dropped` — апдейт выброшен, потому что истёк его срок жизни.

На каждую `http_error` или `connection_error` апдейт будет отправлен повторно.

### TDLib-инстанс, память и диск

| Метрика | Метки | Что это |
|---|---|---|
| `tgbotapi_bot_tdlib_updates_total` | | Апдейты от TDLib. Растёт вместе с объёмом памяти инстанса |
| `tgbotapi_bot_tdlib_restarts_total` | | Горячие рестарты инстанса |
| `tgbotapi_bot_tdlib_restart_in_progress`, `tgbotapi_bot_tdlib_restart_delayed_requests` | | Идёт ли рестарт сейчас и сколько запросов ждёт |
| `tgbotapi_bot_instance_uptime_seconds` | | Время работы текущего инстанса |
| `tgbotapi_bot_cached_objects` | `type` | Объекты в кэше Bot API: `message`, `user`, `chat`, `supergroup`, ... |
| `tgbotapi_bot_pending_update_chats`, `tgbotapi_bot_pending_sent_messages` | | Чаты с недообработанными апдейтами и отправки, ждущие подтверждения |
| `tgbotapi_bot_storage_bytes` | `type` | Размер на диске: `files`, `database` или `log`. Обновляется раз в минуту |
| `tgbotapi_bot_storage_files` | | Число файлов бота на диске |
| `tgbotapi_bot_tdlib_objects` | `type` | Объекты в памяти TDLib-инстанса, см. ниже. Обновляется раз в минуту |
| `tgbotapi_bot_info` | `username`, `webhook` | Всегда 1; метки удобны для подписей на графиках |

`tgbotapi_bot_tdlib_objects` показывает именно то, что копится в памяти TDLib и освобождается
только рестартом инстанса. Значения `type`:

| `type` | Что это |
|---|---|
| `file_id`, `file_node` | Идентификаторы и узлы файлов в `FileManager`. Растут с каждым фото, видео и документом, которые бот видел. Никогда не удаляются |
| `remote_file_location`, `local_file_location` | Известные файлы на серверах Telegram и на диске |
| `file_source`, `file_reference_node`, `message_file_source` | Источники для обновления file reference: по одному на сообщение с медиа и т. п. Никогда не удаляются |
| `chat` | Чаты (`Dialog`). Никогда не удаляются |
| `message` | Сообщения, загруженные в память. Выгружаются через `message_unload_delay` |
| `user`, `user_full_info` | Пользователи и их полная информация. Никогда не удаляются |
| `basic_group`, `basic_group_full_info`, `supergroup`, `min_supergroup`, `supergroup_full_info` | Группы и каналы. Никогда не удаляются |
| `animation`, `audio`, `document`, `sticker`, `sticker_set`, `video`, `video_note`, `voice_note` | Медиа-объекты. Никогда не удаляются |
| `web_page`, `poll`, `story` | Превью ссылок, опросы и истории |

Счётчики берутся из размеров контейнеров TDLib за константное время, поэтому запрос почти ничего
не стоит. Для этого TDLib пропатчен (`td-patches/`, применяется автоматически при сборке).

### CPU

| Метрика | Метки | Что это |
|---|---|---|
| `tgbotapi_bot_cpu_seconds_total` | `part` | CPU-время бота. `client` — часть Bot API: обработка запросов, JSON, вебхук. `tdlib` — TDLib-инстанс бота, включая его работу в потоках базы данных и файлов |

Все боты работают в общих потоках, поэтому ОС не может разделить их CPU. Сервер считает его
выборочно: каждые 10 мс CPU-времени каждого потока (`--cpu-sampling-rate`, по умолчанию 100 раз в
секунду) поток прерывается, и обработчик сигнала только запоминает, какой actor-контекст сейчас
исполняется. Раз в секунду выборки привязываются к ботам. Это стандартный подход профилировщиков;
накладные расходы порядка 0,05% на загруженный поток. Оценка статистическая: каждая выборка —
10 мс CPU. Если бот потратил за интервал хотя бы секунду CPU (100 выборок), погрешность порядка
10%, и она падает с ростом интервала. Для тех, кто действительно нагружает сервер, оценка точная.

Сумма CPU всех ботов и `tgbotapi_cpu_unattributed_seconds_total` равна CPU всех потоков сервера.
Не привязывается к ботам работа, общая для всех: приём и разбор HTTP-запросов и запись ответов в
сокет, очередь апдейтов на диске, `ClientManager`. Её можно разнести по ботам пропорционально числу
запросов (пример ниже). Туда же попадает CPU ботов, которые так и не авторизовались, после
удаления их записи.

## Общие метрики сервера

| Метрика | Что это |
|---|---|
| `tgbotapi_process_resident_memory_bytes`, `tgbotapi_process_virtual_memory_bytes` | Память процесса |
| `tgbotapi_process_cpu_seconds_total{mode="user"\|"system"}` | CPU процесса |
| `tgbotapi_thread_cpu_seconds_total{scheduler}` | Точное CPU-время каждого потока сервера, см. ниже |
| `tgbotapi_process_disk_read_bytes_total`, `tgbotapi_process_disk_written_bytes_total` | Чтение с диска и запись на диск всем процессом |
| `tgbotapi_thread_disk_read_bytes_total{scheduler}`, `tgbotapi_thread_disk_written_bytes_total{scheduler}` | То же по потокам: видно, сколько пишут базы TDLib, файлы и очередь апдейтов |
| `tgbotapi_cpu_unattributed_seconds_total` | CPU, который не относится ни к одному боту |
| `tgbotapi_cpu_sampling_period_seconds` | Шаг выборки CPU |
| `tgbotapi_uptime_seconds` | Время работы сервера |
| `tgbotapi_bots{state="authorized"\|"unauthorized"}` | Число ботов |
| `tgbotapi_rejected_requests_total{reason}` | Запросы, не дошедшие ни до одного бота. `reason`: `invalid_token`, `misdirected_token`, `closed_bot`, `ip_flood_limit`, `global_flood_limit` или `closing` |
| `tgbotapi_unauthorized_bot_requests_total{code}` | Запросы ботов, которые так и не авторизовались |
| `tgbotapi_tqueue_expired_updates_total` | Апдейты, удалённые из очереди по истечении срока: бот их так и не забрал |
| `tgbotapi_tdlib_restarts_total`, `tgbotapi_tdlib_failed_restarts_total` | Горячие рестарты инстансов |
| `tgbotapi_active_requests`, `tgbotapi_active_network_queries`, `tgbotapi_webhook_connections`, `tgbotapi_buffer_memory_bytes` | Текущая нагрузка |

Потоки (`scheduler`):

| `scheduler` | Что в нём работает |
|---|---|
| `0` | Основная логика всех TDLib-инстансов. Если `rate()` близок к 1, этот поток — узкое место всего сервера |
| `1` | Базы данных TDLib |
| `2` | Сборка мусора TDLib и удаление файлов |
| `3` | Загрузка и скачивание файлов TDLib |
| `4` | HTTP-сервер, `ClientManager` и Bot API-часть всех ботов |
| `6`, `7` | Медленные входящие HTTP-соединения и соединения вебхуков |
| `9` | Очередь апдейтов и вебхуки на диске |
| остальные | Watchdog, DNS, сертификаты вебхуков, статистика |

## Примеры запросов (MetricsQL)

```
# топ-10 ботов по числу запросов за час
topk(10, sum by (bot) (increase(tgbotapi_bot_requests_total[1h])))

# какие методы и как часто вызывает бот
sum by (method) (rate(tgbotapi_bot_requests_total{bot="123"}[5m]))

# доля ответов 429 по ботам и причинам
sum by (bot, reason) (rate(tgbotapi_bot_requests_total{code="429"}[15m]))
  / ignoring(reason) group_left sum by (bot) (rate(tgbotapi_bot_requests_total[15m]))

# кто отправляет файлы ссылками
sum by (bot) (increase(tgbotapi_bot_input_files_total{type="url"}[1d]))

# сколько бот скачивает и сколько скачиваний обрывается
sum by (bot) (increase(tgbotapi_bot_downloaded_bytes_total[1d]))
sum by (bot, result) (increase(tgbotapi_bot_file_downloads_total{result!~"completed|cached"}[1d]))

# 99-й перцентиль времени ответа бота
histogram_quantile(0.99, sum by (bot, le) (rate(tgbotapi_bot_request_duration_seconds_bucket[5m])))

# топ-10 ботов по CPU (в ядрах) и кто из них нагружает TDLib
topk(10, sum by (bot) (rate(tgbotapi_bot_cpu_seconds_total[5m])))
sum by (bot, part) (rate(tgbotapi_bot_cpu_seconds_total[5m]))

# CPU бота вместе с его долей общей работы сервера, пропорционально числу запросов
sum by (bot) (rate(tgbotapi_bot_cpu_seconds_total[5m]))
+ scalar(rate(tgbotapi_cpu_unattributed_seconds_total[5m]))
  * sum by (bot) (rate(tgbotapi_bot_requests_total[5m])) / scalar(sum(rate(tgbotapi_bot_requests_total[5m])))

# загрузка потока всех TDLib-инстансов
rate(tgbotapi_thread_cpu_seconds_total{scheduler="0"}[1m])

# кто копит объекты в памяти TDLib
topk(10, sum by (bot) (tgbotapi_bot_tdlib_objects{type="file_node"}))
topk(10, sum by (bot) (tgbotapi_bot_tdlib_objects{type="chat"}))
```

Индекс нагрузки удобно считать recording rule'ом в VictoriaMetrics, а не в сервере: тогда веса можно
менять без переразвёртывания. Пример — взвешенная сумма долей бота в CPU, запросах, трафике и
объектах в памяти TDLib за сутки:

```
0.4 * (sum by (bot) (increase(tgbotapi_bot_cpu_seconds_total[1d]))
       / ignoring(bot) group_left sum(increase(tgbotapi_bot_cpu_seconds_total[1d])))
+ 0.2 * (sum by (bot) (increase(tgbotapi_bot_requests_total[1d]))
       / ignoring(bot) group_left sum(increase(tgbotapi_bot_requests_total[1d])))
+ 0.2 * (sum by (bot) (increase(tgbotapi_bot_request_bytes_total[1d]) + increase(tgbotapi_bot_response_bytes_total[1d]))
       / ignoring(bot) group_left sum(increase(tgbotapi_bot_request_bytes_total[1d]) + increase(tgbotapi_bot_response_bytes_total[1d])))
+ 0.2 * (sum by (bot) (max_over_time(tgbotapi_bot_tdlib_objects{type="file_node"}[1d]))
       / ignoring(bot) group_left sum(max_over_time(tgbotapi_bot_tdlib_objects{type="file_node"}[1d])))
```

## Ограничения

- **Память в байтах по боту** не считается: все боты делят один аллокатор. Вместо этого есть
  число объектов каждого типа (`tgbotapi_bot_tdlib_objects`) и объектов в кэше Bot API
  (`tgbotapi_bot_cached_objects`). Их рост и показывает, кто занимает память.
- **Дисковый ввод-вывод по боту** не считается: базы всех ботов пишет общий поток, а запись
  идёт через общий page cache. Есть ввод-вывод по процессу и по потокам, а по боту его косвенно
  показывают скачивания и загрузки файлов, `tgbotapi_bot_storage_bytes` и CPU части `tdlib`.
- CPU по ботам и ввод-вывод по потокам считаются только в Linux и только при включённой выборке
  CPU (`--cpu-sampling-rate` больше 0).

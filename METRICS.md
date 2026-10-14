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
запрос при стоимости самого запроса в десятки и сотни микросекунд. Значения диска и объектов TDLib
считаются только в момент опроса или раз в минуту; память по ботам ведётся непрерывно.

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
| `tgbotapi_bot_storage_bytes` | `type` | Размер на диске: `files` или `database`. Обновляется раз в минуту |
| `tgbotapi_bot_storage_files` | | Число файлов бота на диске |
| `tgbotapi_bot_tdlib_objects` | `type` | Объекты в памяти TDLib-инстанса, см. ниже. Обновляется раз в минуту |
| `tgbotapi_bot_info` | `username`, `webhook` | Всегда 1; метки удобны для подписей на графиках |

`tgbotapi_bot_tdlib_objects` показывает именно то, что копится в памяти TDLib и освобождается
только рестартом инстанса. Значения `type`:

| `type` | Что это |
|---|---|
| `file_id`, `file_node` | Созданные идентификаторы и узлы файлов в `FileManager`, включая узлы, объединённые с другими. Растут с каждым фото, видео и документом, которые бот видел. Никогда не удаляются |
| `remote_file_location`, `local_file_location` | Известные файлы на серверах Telegram и на диске |
| `file_source`, `file_reference_node`, `message_file_source` | Источники для обновления file reference: по одному на сообщение с медиа и т. п. Никогда не удаляются |
| `chat` | Чаты (`Dialog`). Никогда не удаляются |
| `message` | Сообщения, загруженные в память. Выгружаются через `message_unload_delay` |
| `user`, `user_full_info` | Пользователи и их полная информация. Никогда не удаляются |
| `basic_group`, `basic_group_full_info`, `supergroup`, `min_supergroup`, `supergroup_full_info` | Группы и каналы. Никогда не удаляются |
| `animation`, `audio`, `document`, `sticker`, `sticker_set`, `video`, `video_note`, `voice_note` | Медиа-объекты. Никогда не удаляются |
| `web_page`, `poll`, `story` | Превью ссылок, опросы и истории |

Счётчики берутся из размеров контейнеров TDLib без их обхода, поэтому запрос почти ничего не
стоит. Запросы разных ботов распределены по минуте. Для этого TDLib пропатчен (`td-patches/`,
применяется автоматически при сборке).

### Память

| Метрика | Метки | Что это |
|---|---|---|
| `tgbotapi_bot_memory_bytes` | `part` | Оперативная память бота в байтах. `client` — часть Bot API: кэш сообщений, пользователей и чатов, запросы в обработке, вебхук. `tdlib` — TDLib-инстанс бота. `updates` — очередь апдейтов, которые бот ещё не забрал |
| `tgbotapi_bot_memory_error_bytes` | | Погрешность оценки: стандартное отклонение суммы `tgbotapi_bot_memory_bytes` по частям. Настоящий объём лежит в пределах ±2 таких отклонений от оценки в 95% случаев и ±3 — в 99,7% |
| `tgbotapi_bot_memory_samples` | | Сколько выделений памяти бота попало в выборку. Если меньше ~100, погрешность сама оценена грубо, и ошибка чаще в сторону недооценки |
| `tgbotapi_bot_memory_exact_bytes` | `part` | Точный объём памяти бота. Только в режиме проверки `TELEGRAM_BOT_API_MEMORY_AUDIT=1`, см. ниже |

Сервер перехватывает все функции выделения памяти (`malloc`, `free`, `realloc` и остальные) и
передаёт их дальше настоящему аллокатору: glibc или подключённому через `LD_PRELOAD`, например
jemalloc. По пути он делает две вещи:

1. **Точно** считает общий объём живых выделений: `tgbotapi_heap_bytes`.
2. **Выборочно** учитывает выделения по ботам. В среднем раз на 64 КБ выделенной памяти
   (`--memory-sampling-interval`) одно выделение запоминается вместе с ботом, в контексте которого
   оно сделано, и «весом» — сколько памяти оно представляет. Блок размером n попадает в выборку с
   вероятностью `1 - exp(-(n + 1) / 64 КБ)`, а его вес — размер, делённый на эту вероятность.
   Поэтому крупные блоки учитываются почти всегда и точно, а мелкие — статистически. Оценка
   несмещённая: сумма весов живых отобранных блоков в среднем равна настоящему объёму.

Память принадлежит тому боту, в контексте которого её выделили, и остаётся за ним, пока её не
освободят, кто бы это ни сделал. Новые апдейты в очередь записываются в часть `updates`: рестарт
инстанса их не освобождает, поэтому лимиты рестартов учитывают только `client` и `tdlib`.

Погрешность сервер считает сам для каждого бота: `tgbotapi_bot_memory_error_bytes`. Она примерно
равна `sqrt(64 КБ × память бота)`, то есть относительная погрешность падает с ростом памяти.
Наибольшая ошибка в тестах при шаге 64 КБ и постоянном выделении и освобождении памяти в 4 потоках:

| Память бота | Наибольшая ошибка |
|---|---|
| 2 ГБ | 2% |
| 500 МБ | 3,5% |
| 100 МБ | 8% |
| 20 МБ | 15% |

Как доказано и проверено, что ошибка действительно такая и что учёт не добавляет задержек и сбоев,
и как проверить это самостоятельно на своём сервере, — в [VERIFICATION.md](VERIFICATION.md). Коротко:
- точный счётчик совпадает со статистикой самого аллокатора до байта, glibc и jemalloc;
- оценка несмещённая, а заявленная погрешность соответствует фактической: на независимых кучах и
  на настоящем сервере точное значение попадает в ±1/2/3 погрешности в 68/95/99,7% случаев;
- после освобождения всей памяти оценки возвращаются ровно в 0, без дрейфа на 20 млн операций;
- многопоточный стресс всех функций выделения памяти проходит без единого повреждения данных;
- A/B-сравнение с сервером без учёта не показывает разницы в задержках и сбоях.

Самопроверка в работе: `tgbotapi_heap_estimated_bytes` должен отличаться от `tgbotapi_heap_bytes` не
больше чем на 3 × `tgbotapi_heap_estimate_error_bytes`.

**Режим проверки.** С переменной окружения `TELEGRAM_BOT_API_MEMORY_AUDIT=1` сервер дополнительно
считает память каждого бота точно: каждое выделение получает 16-байтный заголовок с владельцем. Это
стоит памяти (16 байт на каждое живое выделение), поэтому режим нужен для проверки, например на
одном канареечном сервере, а не постоянно. Выборка и веса от режима не зависят, так что оценки в нём
те же, что без него.

Цена: пара `malloc`+`free` стоит 25 нс вместо 20 нс у одного glibc, ~1 МБ памяти плюс до 8 МБ кэша
освобождённых отобранных блоков. На сервере под нагрузкой это +1,6% CPU на запрос без изменения
задержек ответов. Медленный путь — в среднем раз на 64 КБ выделенной памяти —
берёт общий мьютекс на несколько микросекунд; его длительность видна в
`tgbotapi_memory_sampler_operation_seconds`. Учёт работает только с динамически слинкованной glibc в
Linux; собрать без него: `-DTELEGRAM_BOT_API_HEAP_SAMPLING=OFF`, выключить без пересборки:
`--memory-sampling-interval=0` (тогда остаётся только точный общий счётчик).

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

Не привязывается к ботам работа, общая для всех: приём и разбор HTTP-запросов и запись ответов в
сокет, очередь апдейтов на диске, `ClientManager`. Она идёт в `tgbotapi_cpu_unattributed_seconds_total`,
и её можно разнести по ботам пропорционально числу запросов (пример ниже). Туда же попадает CPU
ботов, которые так и не авторизовались, но только после удаления их записи.

Сумма CPU ботов и `tgbotapi_cpu_unattributed_seconds_total` равна CPU всех потоков сервера с
точностью выборки. Исключения: CPU ботов, которые ещё не авторизовались, и ботов, запись которых уже
удалена, — как и остальные их счётчики. Последняя часть закрытия TDLib-инстанса учитывается как
`client`.

## Общие метрики сервера

| Метрика | Что это |
|---|---|
| `tgbotapi_process_resident_memory_bytes`, `tgbotapi_process_virtual_memory_bytes` | Память процесса |
| `tgbotapi_heap_bytes` | Точный объём всех живых выделений памяти. Разница с `tgbotapi_process_resident_memory_bytes` — память, которую аллокатор освободил, но ещё не вернул ОС, а также стеки потоков и код |
| `tgbotapi_heap_estimated_bytes`, `tgbotapi_heap_estimate_error_bytes` | Сумма оценок по ботам и неатрибутированной памяти и её погрешность (стандартное отклонение); оценка должна быть близка к `tgbotapi_heap_bytes` |
| `tgbotapi_memory_unattributed_bytes` | Память, не относящаяся ни к одному авторизованному боту: HTTP-соединения, загружаемые файлы, общие структуры |
| `tgbotapi_memory_samples`, `tgbotapi_memory_sampling_interval_bytes`, `tgbotapi_memory_sampler_overhead_bytes` | Число отобранных выделений, шаг выборки и память самого учёта |
| `tgbotapi_memory_sampler_operation_seconds` | Гистограмма длительности операций с отобранными блоками, включая ожидание мьютекса. Только эти операции учёта памяти берут блокировку |
| `tgbotapi_memory_sampler_max_operation_seconds`, `tgbotapi_memory_sampler_max_lock_wait_seconds` | Самая долгая такая операция и самое долгое ожидание мьютекса учёта с момента запуска |
| `tgbotapi_heap_audit_bytes`, `tgbotapi_memory_unattributed_exact_bytes` | Точный объём всех выделений и неатрибутированной памяти в единицах оценок. Только в режиме проверки |
| `tgbotapi_scheduler_lag_seconds{scheduler}` | Гистограмма задержки срабатывания таймера в каждом потоке: 20 раз в секунду поток должен проснуться, и задержка — это время, когда он был занят чем-то другим или не получил CPU. Показывает зависания потоков независимо от их причины |
| `tgbotapi_scheduler_max_lag_seconds{scheduler}` | Самая большая такая задержка за последнюю минуту |
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

# топ-10 ботов по памяти и из чего она состоит
topk(10, sum by (bot) (tgbotapi_bot_memory_bytes))
sum by (bot, part) (tgbotapi_bot_memory_bytes{bot="123"})

# память, которую аллокатор держит свободной (фрагментация), и проверка учёта
tgbotapi_process_resident_memory_bytes - tgbotapi_heap_bytes
abs(tgbotapi_heap_estimated_bytes - tgbotapi_heap_bytes) / tgbotapi_heap_estimate_error_bytes

# память бота с границами, в которых настоящее значение лежит с вероятностью 99,7%
sum by (bot) (tgbotapi_bot_memory_bytes) - 3 * tgbotapi_bot_memory_error_bytes
sum by (bot) (tgbotapi_bot_memory_bytes) + 3 * tgbotapi_bot_memory_error_bytes

# зависания: потоки, которые за минуту хоть раз не просыпались дольше 100 мс, и доля долгих
# операций учёта памяти
tgbotapi_scheduler_max_lag_seconds > 0.1
1 - rate(tgbotapi_memory_sampler_operation_seconds_bucket{le="0.001"}[5m])
  / rate(tgbotapi_memory_sampler_operation_seconds_count[5m])

# кто копит объекты в памяти TDLib
topk(10, sum by (bot) (tgbotapi_bot_tdlib_objects{type="file_node"}))
topk(10, sum by (bot) (tgbotapi_bot_tdlib_objects{type="chat"}))
```

Индекс нагрузки удобно считать recording rule'ом в VictoriaMetrics, а не в сервере: тогда веса можно
менять без переразвёртывания. Пример — взвешенная сумма долей бота в CPU, запросах, трафике и
памяти за сутки:

```
0.4 * (sum by (bot) (increase(tgbotapi_bot_cpu_seconds_total[1d]))
       / ignoring(bot) group_left sum(increase(tgbotapi_bot_cpu_seconds_total[1d])))
+ 0.2 * (sum by (bot) (increase(tgbotapi_bot_requests_total[1d]))
       / ignoring(bot) group_left sum(increase(tgbotapi_bot_requests_total[1d])))
+ 0.2 * (sum by (bot) (increase(tgbotapi_bot_request_bytes_total[1d]) + increase(tgbotapi_bot_response_bytes_total[1d]))
       / ignoring(bot) group_left sum(increase(tgbotapi_bot_request_bytes_total[1d]) + increase(tgbotapi_bot_response_bytes_total[1d])))
+ 0.2 * (sum by (bot) (max_over_time(tgbotapi_bot_memory_bytes[1d]))
       / ignoring(bot) group_left sum(max_over_time(tgbotapi_bot_memory_bytes[1d])))
```

## Ограничения

- **Память по боту** — статистическая оценка, погрешность которой сервер сообщает сам. Свободная память,
  которую аллокатор не вернул ОС, не принадлежит никому и видна только по разнице
  `tgbotapi_process_resident_memory_bytes - tgbotapi_heap_bytes`.
- **Дисковый ввод-вывод по боту** не считается: базы всех ботов пишет общий поток, а запись
  идёт через общий page cache. Есть ввод-вывод по процессу и по потокам, а по боту его косвенно
  показывают скачивания и загрузки файлов, `tgbotapi_bot_storage_bytes` и CPU части `tdlib`.
- CPU по ботам, ввод-вывод по потокам и задержки таймеров потоков считаются только в Linux и только
  при включённой выборке CPU (`--cpu-sampling-rate` больше 0).

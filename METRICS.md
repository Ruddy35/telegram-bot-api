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
| `tgbotapi_bot_info` | `username`, `webhook` | Всегда 1; метки удобны для подписей на графиках |

## Общие метрики сервера

| Метрика | Что это |
|---|---|
| `tgbotapi_process_resident_memory_bytes`, `tgbotapi_process_virtual_memory_bytes` | Память процесса |
| `tgbotapi_process_cpu_seconds_total{mode="user"\|"system"}` | CPU процесса |
| `tgbotapi_uptime_seconds` | Время работы сервера |
| `tgbotapi_bots{state="authorized"\|"unauthorized"}` | Число ботов |
| `tgbotapi_rejected_requests_total{reason}` | Запросы, не дошедшие ни до одного бота. `reason`: `invalid_token`, `misdirected_token`, `closed_bot`, `ip_flood_limit`, `global_flood_limit` или `closing` |
| `tgbotapi_unauthorized_bot_requests_total{code}` | Запросы ботов, которые так и не авторизовались |
| `tgbotapi_tqueue_expired_updates_total` | Апдейты, удалённые из очереди по истечении срока: бот их так и не забрал |
| `tgbotapi_tdlib_restarts_total`, `tgbotapi_tdlib_failed_restarts_total` | Горячие рестарты инстансов |
| `tgbotapi_active_requests`, `tgbotapi_active_network_queries`, `tgbotapi_webhook_connections`, `tgbotapi_buffer_memory_bytes` | Текущая нагрузка |

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
```

Индекс нагрузки удобно считать recording rule'ом в VictoriaMetrics, а не в сервере: тогда веса можно
менять без переразвёртывания. Пример — взвешенная сумма долей бота в запросах, трафике и апдейтах
TDLib за сутки:

```
0.4 * (sum by (bot) (increase(tgbotapi_bot_requests_total[1d]))
       / ignoring(bot) group_left sum(increase(tgbotapi_bot_requests_total[1d])))
+ 0.3 * (sum by (bot) (increase(tgbotapi_bot_request_bytes_total[1d]) + increase(tgbotapi_bot_response_bytes_total[1d]))
       / ignoring(bot) group_left sum(increase(tgbotapi_bot_request_bytes_total[1d]) + increase(tgbotapi_bot_response_bytes_total[1d])))
+ 0.3 * (sum by (bot) (increase(tgbotapi_bot_tdlib_updates_total[1d]))
       / ignoring(bot) group_left sum(increase(tgbotapi_bot_tdlib_updates_total[1d])))
```

## Чего пока нет

- **CPU по каждому боту.** Все боты делят одни и те же потоки, поэтому ОС не может разделить их
  CPU. План — выборочный учёт: каждые 10 мс CPU-времени потока смотреть, контекст какого бота
  сейчас исполняется. Нужен небольшой патч `tdactor`.
- **Число объектов в памяти TDLib по боту:** узлы файлов, диалоги, пользователи, чаты. Нужен
  небольшой патч TDLib, который только читает размеры контейнеров. Пока косвенный показатель —
  `tgbotapi_bot_tdlib_updates_total`.

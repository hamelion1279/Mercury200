# light.living_room и sensor.temperature_bedroom — как создать и проверить

**Коротко и честно:** у агента нет прямого доступа к вашему Home Assistant и к вашему
OpenWebUI — в этой среде есть только репозиторий и GitHub. Поэтому «создать сущность
у вас на сервере» и «посмотреть в OpenWebUI» за вас он не может. Это и есть то самое
«не получается».

Зато ниже — три готовых способа, которые создают **ровно эти** `entity_id`, и проверка
в OpenWebUI. Всё лежит в `integrations/homeassistant/`.

```
integrations/homeassistant/
├── ha_entities.py                    # создать / проверить / локальная заглушка
├── templates/configuration_entities.yaml   # вариант «через configuration.yaml»
└── build/                            # генерируется командой mqtt (в git не попадает)
```

Нужен только Python 3.8+ (сторонние библиотеки не требуются).

---

## 1. Выберите способ

| Способ | Команда | Живёт после перезапуска HA | Что нужно |
|---|---|---|---|
| REST API (быстро) | `states` | нет | `HA_URL`, `HA_TOKEN` |
| MQTT Discovery | `mqtt --via-ha` (или `--publish`) | да | интеграции MQTT + MQTT Discovery |
| YAML-шаблоны | `yaml` → в `configuration.yaml` | да | доступ к `configuration.yaml`, перезапуск |

### Способ 1 — REST API, 10 секунд

```bash
export HA_URL=http://homeassistant.local:8123      # ваш адрес HA
export HA_TOKEN=eyJ...                             # Профиль -> Безопасность -> Долгоживущие токены

python3 integrations/homeassistant/ha_entities.py states
```

Скрипт делает `POST /api/states/light.living_room` и
`POST /api/states/sensor.temperature_bedroom` с состояниями `on` / `21.5 °C`
и атрибутами `friendly_name`, `brightness`, `device_class`, `unit_of_measurement`.
Сущности сразу видны в **Обзоре** HA. Ограничение: после перезапуска HA они исчезают.

### Способ 2 — MQTT Discovery (постоянные, без правки YAML)

```bash
# вариант A: публикует сам Home Assistant через сервис mqtt.publish — mqtt-клиент не нужен
python3 integrations/homeassistant/ha_entities.py mqtt --via-ha

# вариант B: напрямую в брокер
python3 integrations/homeassistant/ha_entities.py mqtt --publish      # нужен: pip install paho-mqtt

# вариант C: без Python-клиента, готовый скрипт
cd integrations/homeassistant/build && MQTT_HOST=192.168.1.10 ./publish.sh   # apt install mosquitto-clients
```

Публикуются 4 сообщения с `retain=true`:

```
homeassistant/light/living_room/config              -> light.living_room
homeassistant/sensor/temperature_bedroom/config     -> sensor.temperature_bedroom
mercury200/light/living_room/state                  {"state":"ON","brightness":180}
mercury200/sensor/temperature_bedroom/state         {"temperature":21.5}
```

Через 1–2 секунды сущности появятся сами (Настройки → Устройства и службы → MQTT).
Ими можно управлять: `light.turn_on`, `light.turn_off`, `light.turn_on` с `brightness`.

### Способ 3 — YAML-шаблоны (`light` + `sensor`)

```bash
python3 integrations/homeassistant/ha_entities.py yaml
```

Вывод (он же — `templates/configuration_entities.yaml`) добавьте в `configuration.yaml`.
Создаются помощники `input_boolean.living_room_light`, `input_number.living_room_brightness`,
`input_number.bedroom_temperature` и на их основе:

* `light.living_room` — шаблонный светильник (`state`, `level`, `turn_on`, `turn_off`, `set_level`);
* `sensor.temperature_bedroom` — шаблонный датчик температуры (`device_class: temperature`).

Затем: **Developer tools → YAML → Check configuration → Restart** (или перезагрузка только
шаблонов: *YAML → Template entities*). В шаблонах используется современный синтаксис
`action:`; на HA старше 2024.8 замените его на `service:`.

---

## 2. Проверка, что сущности есть

```bash
export HA_URL=http://homeassistant.local:8123 HA_TOKEN=eyJ...
python3 integrations/homeassistant/ha_entities.py check
```

```
HA: http://homeassistant.local:8123  ->  API running.
OK   light.living_room                state=on brightness=180
OK   sensor.temperature_bedroom       state=21.5 °C
```

Код возврата `0` — обе сущности на месте, `1` — чего-то нет (скрипт подскажет, что делать).

Проверка без Home Assistant (локальная заглушка с той же страницей и тем же API):

```bash
python3 integrations/homeassistant/ha_entities.py mock --port 8765
# в другом терминале:
HA_URL=http://localhost:8765 HA_TOKEN=demo python3 integrations/homeassistant/ha_entities.py states
HA_URL=http://localhost:8765 HA_TOKEN=demo python3 integrations/homeassistant/ha_entities.py check
```

Откройте <http://localhost:8765> — таблица сущностей и опубликованные MQTT-сообщения.
Заглушка слушает `0.0.0.0`, отвечает на `HEAD` (health-check/proxy) и не ставит
`X-Frame-Options`, поэтому открывается во встроенном превью и за любым reverse-proxy.

---

## 3. Проверка в OpenWebUI

Важно: OpenWebUI сам по себе **не видит** сущности HA. Ему нужен мост «чат → Home Assistant».
Самый простой — MCP-сервер с инструментами HA (`ha-mcp`) или аналогичный Tool.

1. **Admin Panel → Settings → Tools** (или *Integrations → MCP*) → добавить сервер:
   * URL: `http://<ваш-ha-mcp>:<порт>/mcp` (часто `http://localhost:8099/mcp`, зависит от образа);
   * переменные для сервера: `HA_URL=http://homeassistant.local:8123`, `HA_TOKEN=eyJ...`.
2. В чате включите этот инструмент (иконка «+» → Tools) и выберите модель с tool-calling.
3. Напишите в чате:

   > покажи состояние `light.living_room` и `sensor.temperature_bedroom`

4. Скриптовая проверка самого OpenWebUI:

```bash
export OPENWEBUI_URL=http://localhost:3000
export OPENWEBUI_KEY=sk-...        # Настройки -> Аккаунт -> API keys
python3 integrations/homeassistant/ha_entities.py openwebui
```

Скрипт печатает список моделей и напоминает, что инструмент HA должен быть включён.
Если ответ — `HTTP 401`, ключ неверный; если `Connection refused` — адрес/порт не тот.

---

## 4. Что делать, если «не получается»

Пришлите вывод этих трёх команд — по нему видно причину:

```bash
curl -s -o /dev/null -w '%{http_code}\n' -H "Authorization: Bearer $HA_TOKEN" $HA_URL/api/
python3 integrations/homeassistant/ha_entities.py check
python3 integrations/homeassistant/ha_entities.py openwebui
```

Частые случаи:

| Симптом | Причина | Решение |
|---|---|---|
| `HTTP 401` на `/api/` | истёк/неверный токен | создать новый долгоживущий токен |
| `Connection refused` | HA слушает другой адрес/порт, блокирует firewall | проверить `HA_URL`, порт 8123, доступ по LAN |
| `НЕТ light.living_room` после `states` | сущность переименована (`light.living_room_2`) | посмотреть `GET /api/states`, поправить `LIGHT_ID` |
| Сущности пропали после перезапуска | способ 1 — временный | использовать `mqtt` или `yaml` |
| OpenWebUI «не знает» про HA | не включён инструмент/MCP | п. 3, проверить `openwebui` и включить Tool в чате |
| HA ругается на YAML | версия старше 2024.8 | заменить `action:` на `service:` |

---

## 5. Как привязать сюда реальный Mercury-200

Этот репозиторий — протокол счётчика Меркурий-200. Мгновенные значения берутся командой
`commands.GetInstants` → `types.Instants{P, U, I}`. Их удобно отдавать в те же MQTT-топики,
что и демо-сущности, тогда в HA появятся настоящие сенсоры:

```
mercury200/sensor/power/state        {"power": 1234.5}    # Вт, device_class: power
mercury200/sensor/voltage/state      {"voltage": 230.1}   # В,   device_class: voltage
mercury200/sensor/current/state      {"current": 5.36}    # А,   device_class: current
mercury200/sensor/energy_t1/state    {"energy": 684.92}   # кВт·ч, state_class: total_increasing
```

Discovery-конфиги для них собираются тем же `ha_entities.py` (см. `SENSOR_DISCOVERY`) —
скажите, и добавлю команду `bridge`, которая читает счётчик по COM-порту и публикует значения.

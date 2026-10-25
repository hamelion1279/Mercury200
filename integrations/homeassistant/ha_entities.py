#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Создание и проверка сущностей Home Assistant:

    light.living_room
    sensor.temperature_bedroom

Скрипт не требует сторонних библиотек (только стандартная библиотека Python 3.8+).

Команды
-------
    states              создать сущности через REST API (быстро, до перезапуска HA)
    mqtt [--publish]    MQTT Discovery: постоянные сущности без правки configuration.yaml
    yaml                вывести готовый YAML для configuration.yaml (постоянные сущности)
    check               проверить, что обе сущности есть в HA и отдают состояние
    openwebui           проверить, что OpenWebUI отвечает и какие модели/инструменты включены
    mock                локальная заглушка HA (для проверки без реального Home Assistant)

Переменные окружения
--------------------
    HA_URL        http://homeassistant.local:8123
    HA_TOKEN      долгоживущий токен: Профиль -> Безопасность -> Токены доступа
    MQTT_HOST     локальный MQTT-брокер (по умолчанию localhost)
    MQTT_PORT     1883
    MQTT_USER / MQTT_PASSWORD
    OPENWEBUI_URL http://localhost:3000
    OPENWEBUI_KEY API-ключ из OpenWebUI: Настройки -> Аккаунт -> API keys

Примеры
-------
    HA_URL=http://192.168.1.50:8123 HA_TOKEN=xxx python3 ha_entities.py states
    HA_URL=http://192.168.1.50:8123 HA_TOKEN=xxx python3 ha_entities.py check
    python3 ha_entities.py mock            # затем: HA_URL=http://localhost:8765 HA_TOKEN=demo ... states
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

LIGHT_ID = "light.living_room"
SENSOR_ID = "sensor.temperature_bedroom"

LIGHT_STATE = {
    "state": "on",
    "attributes": {
        "friendly_name": "Living room",
        "supported_color_modes": ["brightness"],
        "color_mode": "brightness",
        "brightness": 180,
    },
}

SENSOR_STATE = {
    "state": "21.5",
    "attributes": {
        "friendly_name": "Temperature bedroom",
        "unit_of_measurement": "\u00b0C",
        "device_class": "temperature",
        "state_class": "measurement",
    },
}

DEVICE = {
    "identifiers": ["mercury200_ha_demo"],
    "name": "Mercury-200 demo",
    "manufacturer": "Arena",
    "model": "HA entity demo",
}

LIGHT_DISCOVERY = {
    "name": "Living room",
    "unique_id": "mercury200_living_room_light",
    "schema": "json",
    "command_topic": "mercury200/light/living_room/set",
    "state_topic": "mercury200/light/living_room/state",
    "brightness": True,
    "brightness_scale": 255,
    "supported_color_modes": ["brightness"],
    "device": DEVICE,
}

SENSOR_DISCOVERY = {
    "name": "Temperature bedroom",
    "unique_id": "mercury200_temperature_bedroom",
    "state_topic": "mercury200/sensor/temperature_bedroom/state",
    "unit_of_measurement": "\u00b0C",
    "device_class": "temperature",
    "state_class": "measurement",
    "value_template": "{{ value_json.temperature }}",
    "device": DEVICE,
}

YAML_SNIPPET = """# Вставьте этот блок в configuration.yaml, затем:
#   Developer tools -> YAML -> "Check configuration" -> перезапуск Home Assistant
# (или перезагрузите только шаблоны: Developer tools -> YAML -> Template entities)

input_boolean:
  living_room_light:
    name: Living room light

input_number:
  living_room_brightness:
    name: Living room brightness
    min: 0
    max: 255
    step: 1
    initial: 180
  bedroom_temperature:
    name: Bedroom temperature
    min: -30
    max: 60
    step: 0.1
    unit_of_measurement: "°C"
    initial: 21.5

template:
  - light:
      - name: "Living room"          # -> light.living_room
        unique_id: living_room_light
        state: "{{ is_state('input_boolean.living_room_light', 'on') }}"
        level: "{{ states('input_number.living_room_brightness') | int(0) }}"
        turn_on:
          - action: input_boolean.turn_on
            target:
              entity_id: input_boolean.living_room_light
        turn_off:
          - action: input_boolean.turn_off
            target:
              entity_id: input_boolean.living_room_light
        set_level:
          - action: input_number.set_value
            target:
              entity_id: input_number.living_room_brightness
            data:
              value: "{{ brightness }}"
  - sensor:
      - name: "Temperature bedroom"  # -> sensor.temperature_bedroom
        unique_id: temperature_bedroom
        state: "{{ states('input_number.bedroom_temperature') }}"
        unit_of_measurement: "°C"
        device_class: temperature
        state_class: measurement
"""


# --------------------------------------------------------------------------- #
# HTTP helper
# --------------------------------------------------------------------------- #
def http_json(url: str, token: str | None = None, method: str = "GET", body=None, timeout: int = 10):
    data = None
    headers = {"Content-Type": "application/json"}
    if body is not None:
        data = json.dumps(body).encode("utf-8")
    if token:
        headers["Authorization"] = "Bearer " + token
    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        raw = resp.read().decode("utf-8")
    return json.loads(raw) if raw.strip() else {}


def ha_config() -> tuple[str, str]:
    url = os.environ.get("HA_URL", "http://homeassistant.local:8123").rstrip("/")
    token = os.environ.get("HA_TOKEN", "").strip()
    if not token:
        sys.exit("Не задан HA_TOKEN. Создайте токен: HA -> Профиль -> Безопасность -> "
                 "Долгоживущие токены доступа, затем: export HA_TOKEN=...")
    return url, token


# --------------------------------------------------------------------------- #
# commands
# --------------------------------------------------------------------------- #
def cmd_states(_args) -> int:
    """Создать сущности через POST /api/states (живут до перезапуска HA)."""
    url, token = ha_config()
    for entity_id, payload in ((LIGHT_ID, LIGHT_STATE), (SENSOR_ID, SENSOR_STATE)):
        data = {"state": payload["state"], "attributes": payload["attributes"]}
        res = http_json(f"{url}/api/states/{entity_id}", token, "POST", data)
        print(f"OK  {entity_id:32} state={res.get('state')}")
    print("\nГотово. Откройте Обзор (Overview) в Home Assistant — обе сущности уже там.")
    print("Важно: такие сущности исчезнут после перезапуска HA. "
          "Для постоянных используйте `mqtt` или `yaml`.")
    return 0


def cmd_mqtt(args) -> int:
    """Сгенерировать (и при желании отправить) MQTT Discovery + состояния."""
    out_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "build")
    os.makedirs(out_dir, exist_ok=True)

    messages = [
        ("homeassistant/light/living_room/config", LIGHT_DISCOVERY),
        ("homeassistant/sensor/temperature_bedroom/config", SENSOR_DISCOVERY),
        ("mercury200/light/living_room/state", {"state": "ON", "brightness": 180}),
        ("mercury200/sensor/temperature_bedroom/state", {"temperature": 21.5}),
    ]

    files = []
    for topic, payload in messages:
        name = topic.replace("/", "__") + ".json"
        path = os.path.join(out_dir, name)
        with open(path, "w", encoding="utf-8") as fh:
            json.dump(payload, fh, ensure_ascii=False, indent=2)
        files.append((topic, path))
        print(f"payload  {topic}")

    script = os.path.join(out_dir, "publish.sh")
    with open(script, "w", encoding="utf-8") as fh:
        fh.write("#!/bin/sh\n# Сгенерировано ha_entities.py\nset -e\n")
        fh.write('HOST="${MQTT_HOST:-localhost}"\nPORT="${MQTT_PORT:-1883}"\n')
        fh.write('AUTH=""\n[ -n "$MQTT_USER" ] && AUTH="-u $MQTT_USER -P $MQTT_PASSWORD"\n')
        for topic, path in files:
            fh.write(f'mosquitto_pub -h "$HOST" -p "$PORT" $AUTH -r '
                     f'-t {topic} -f "{os.path.basename(path)}"\n')
    os.chmod(script, 0o755)
    print(f"\nГотово: {script}")
    print("Запуск:  cd integrations/homeassistant/build && MQTT_HOST=<брокер> ./publish.sh")
    print("Сущности появятся в HA автоматически (нужна интеграция MQTT с discovery).")

    if args.via_ha:
        url, token = ha_config()
        for topic, payload in messages:
            http_json(f"{url}/api/services/mqtt/publish", token, "POST",
                      {"topic": topic, "payload": json.dumps(payload), "retain": True, "qos": 1})
            print(f"mqtt.publish -> {topic}")
        print("\nОтправлено через Home Assistant (нужны интеграции MQTT и MQTT Discovery).")
        return 0

    if not args.publish:
        return 0
    try:
        import paho.mqtt.publish as mqtt_publish  # type: ignore
    except ImportError:
        print("\n--publish: модуль paho-mqtt не установлен (pip install paho-mqtt). "
              "Используйте build/publish.sh.", file=sys.stderr)
        return 2
    host = os.environ.get("MQTT_HOST", "localhost")
    port = int(os.environ.get("MQTT_PORT", "1883"))
    auth = None
    if os.environ.get("MQTT_USER"):
        auth = {"username": os.environ["MQTT_USER"], "password": os.environ.get("MQTT_PASSWORD", "")}
    mqtt_publish.multiple([{"topic": t, "payload": json.dumps(p), "retain": True} for t, p in messages],
                          hostname=host, port=port, auth=auth)
    print(f"Отправлено в MQTT {host}:{port}")
    return 0


def cmd_yaml(_args) -> int:
    print(YAML_SNIPPET)
    return 0


def cmd_check(_args) -> int:
    url, token = ha_config()
    ok = True
    try:
        api = http_json(f"{url}/api/", token)
        print(f"HA: {url}  ->  {api.get('message', 'ok')}")
    except urllib.error.HTTPError as exc:
        print(f"HA: {url}  ->  HTTP {exc.code} (проверьте HA_TOKEN)", file=sys.stderr)
        return 1
    except Exception as exc:  # noqa: BLE001
        print(f"HA: {url}  ->  недоступен: {exc}", file=sys.stderr)
        return 1

    for entity_id in (LIGHT_ID, SENSOR_ID):
        try:
            st = http_json(f"{url}/api/states/{entity_id}", token)
        except urllib.error.HTTPError as exc:
            if exc.code == 404:
                print(f"НЕТ  {entity_id}")
                ok = False
                continue
            print(f"ERR  {entity_id}: HTTP {exc.code}", file=sys.stderr)
            ok = False
            continue
        attrs = st.get("attributes", {})
        extra = attrs.get("unit_of_measurement", "")
        if entity_id == LIGHT_ID:
            extra = f"brightness={attrs.get('brightness')}"
        print(f"OK   {entity_id:32} state={st.get('state')} {extra}".rstrip())

    if not ok:
        print("\nЧего-то не хватает. Быстрый способ: ha_entities.py states")
    return 0 if ok else 1


def cmd_openwebui(_args) -> int:
    base = os.environ.get("OPENWEBUI_URL", "http://localhost:3000").rstrip("/")
    key = os.environ.get("OPENWEBUI_KEY", "").strip()
    try:
        models = http_json(f"{base}/api/models", key or None)
    except urllib.error.HTTPError as exc:
        print(f"OpenWebUI: {base} -> HTTP {exc.code} (нужен OPENWEBUI_KEY?)", file=sys.stderr)
        return 1
    except Exception as exc:  # noqa: BLE001
        print(f"OpenWebUI: {base} -> недоступен: {exc}", file=sys.stderr)
        return 1
    items = models.get("data", models if isinstance(models, list) else [])
    print(f"OpenWebUI: {base} -> моделей: {len(items)}")
    for item in items[:20]:
        print("  -", item.get("id") or item.get("name"))
    print("\nЧтобы OpenWebUI видел сущности HA, должно быть выполнено одно из условий:\n"
          "  * в чате выбран инструмент Home Assistant (Admin -> Tools / MCP-сервер ha-mcp), либо\n"
          "  * модель вызывает HA через tool-calling пайплайн.\n"
          "Проверка в чате: «покажи состояние light.living_room и sensor.temperature_bedroom».")
    return 0


# --------------------------------------------------------------------------- #
# mock Home Assistant (для проверки без реального HA)
# --------------------------------------------------------------------------- #
def cmd_mock(args) -> int:
    states: dict[str, dict] = {}
    published: list[dict] = []

    class Handler(BaseHTTPRequestHandler):
        head_only = False   # HEAD-запрос: заголовки без тела

        def _write(self, body: bytes, ctype: str, code: int = 200) -> None:
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            if not self.head_only:
                self.wfile.write(body)

        def _send(self, code: int, payload) -> None:
            self._write(json.dumps(payload, ensure_ascii=False).encode("utf-8"),
                        "application/json; charset=utf-8", code)

        def do_HEAD(self):  # noqa: N802  (health-check/proxy probes)
            self.head_only = True
            self.do_GET()

        def log_message(self, fmt, *a):  # тихий лог
            if args.verbose:
                super().log_message(fmt, *a)

        def do_GET(self):  # noqa: N802
            if self.path in ("/", "/index.html"):
                rows = "".join(
                    f"<tr><td>{eid}</td><td>{st.get('state')}</td>"
                    f"<td><code>{json.dumps(st.get('attributes', {}), ensure_ascii=False)}</code></td></tr>"
                    for eid, st in states.items()
                ) or "<tr><td colspan='3'>пусто — запустите: python3 ha_entities.py states</td></tr>"
                mq = "".join(f"<li><code>{m.get('topic')}</code> {m.get('payload')}</li>"
                             for m in published[-20:]) or "<li>пусто — запустите: python3 ha_entities.py mqtt --via-ha</li>"
                html = ("<!doctype html><meta charset='utf-8'><title>Mock Home Assistant</title>"
                        "<h2>Mock Home Assistant</h2><table border='1' cellpadding='6'>"
                        "<tr><th>entity_id</th><th>state</th><th>attributes</th></tr>"
                        f"{rows}</table><h3>mqtt.publish</h3><ul>{mq}</ul>").encode("utf-8")
                self._write(html, "text/html; charset=utf-8")
                return
            if self.path == "/api/":
                self._send(200, {"message": "API running."})
                return
            if self.path == "/api/states":
                self._send(200, [dict(entity_id=k, **v) for k, v in states.items()])
                return
            if self.path.startswith("/api/states/"):
                eid = self.path.rsplit("/", 1)[-1]
                if eid in states:
                    self._send(200, dict(entity_id=eid, **states[eid]))
                else:
                    self._send(404, {"message": "Entity not found."})
                return
            self._send(404, {"message": "Not found"})

        def do_POST(self):  # noqa: N802
            if self.path == "/api/services/mqtt/publish":
                length = int(self.headers.get("Content-Length") or 0)
                body = json.loads(self.rfile.read(length) or b"{}")
                published.append(body)
                self._send(200, [])
                return
            if not self.path.startswith("/api/states/"):
                self._send(404, {"message": "Not found"})
                return
            eid = self.path.rsplit("/", 1)[-1]
            length = int(self.headers.get("Content-Length") or 0)
            body = json.loads(self.rfile.read(length) or b"{}")
            states[eid] = {"state": body.get("state"), "attributes": body.get("attributes", {})}
            self._send(200, dict(entity_id=eid, **states[eid]))

    server = ThreadingHTTPServer((args.host, args.port), Handler)
    print(f"Mock Home Assistant: http://{args.host}:{args.port}")
    print("Проверка: HA_URL=http://localhost:%d HA_TOKEN=demo python3 %s states|check"
          % (args.port, os.path.basename(__file__)))
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="light.living_room + sensor.temperature_bedroom для Home Assistant")
    sub = parser.add_subparsers(dest="cmd", required=True)

    sub.add_parser("states", help="создать сущности через REST API").set_defaults(func=cmd_states)

    p_mqtt = sub.add_parser("mqtt", help="MQTT Discovery: постоянные сущности")
    p_mqtt.add_argument("--publish", action="store_true", help="отправить в брокер через paho-mqtt")
    p_mqtt.add_argument("--via-ha", action="store_true",
                        help="опубликовать через сервис mqtt.publish самого Home Assistant (без mqtt-клиента)")
    p_mqtt.set_defaults(func=cmd_mqtt)

    sub.add_parser("yaml", help="YAML для configuration.yaml").set_defaults(func=cmd_yaml)
    sub.add_parser("check", help="проверить сущности в HA").set_defaults(func=cmd_check)
    sub.add_parser("openwebui", help="проверить OpenWebUI").set_defaults(func=cmd_openwebui)

    p_mock = sub.add_parser("mock", help="локальная заглушка HA")
    p_mock.add_argument("--host", default="0.0.0.0")
    p_mock.add_argument("--port", type=int, default=8765)
    p_mock.add_argument("--verbose", action="store_true")
    p_mock.set_defaults(func=cmd_mock)

    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())

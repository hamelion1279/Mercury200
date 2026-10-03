# Home Assistant: учёт воды (в паре с esphome/watermet-zigbee.yaml)

Файлы:

- `configuration_water.yaml` — input_*, template-сенсоры, utility_meter, таблица;
- `automations_water.yaml` — кнопка «Ввести показания» + запись месяца в таблицу;
- `scripts_water_sim.yaml` — эмулятор расхода для проверки конвейера;
- `dashboard_water.yaml` — страница Lovelace.

## Почему НЕТ перезагрузки платы по кнопке

План «нажать кнопку → ZHA шлёт Basic cluster command 0 → плата ребутается →
счетчик сессии обнуляется» не работает так, как задуман:

- `cluster_id: 0, command: 0` — это ZCL **Reset to Factory Defaults**, а не ребут;
- в прошивке (ESPHome, `zigbee_esp32.cpp`) выход из сети с типом
  `ESP_ZB_NWK_LEAVE_TYPE_RESET` вызывает `esp_zb_factory_reset()` — стираются
  `zb_storage`/`zb_fct`, и устройство выпадает из ZHA: после каждого нажатия
  пришлось бы пересопрягать его вручную;
- «мягкого ребута» среди выставленных наружу кластеров у ESP32-версии
  компонента zigbee просто нет (наружу отдаются только analog_input и
  binary_input).

Поэтому кнопка «Ввести показания» пересчитывает базу на стороне HA:
`base = табло − импульсы_сессии − симуляция`. Плата не трогается.
См. комментарий в `automations_water.yaml`.

Известная цена: при случайном ребуте платы (замена батареи) счётчик сессии
обнуляется и «Реальные показания» проседают на накопленное за сессию.
Лечится повторным нажатием кнопки с актуальными цифрами табло.

## Таблица помесячно: SQL не понадобился

Строки пишет автоматизация 14-го в 23:55 в `input_text.water_table_json`
(последние 4 строки — лимит input_text 255 символов; полная история живёт в
long-term statistics сенсоров «Реальные показания…»). Дашборд читает атрибут
`table` сенсора `sensor.tablitsa_ucheta_vody_pomesiachno` — как в исходной
разметке.

Если всё же хочется тянуть историю из БД recorder — черновик запроса
(НЕ проверен на живой БД, схемы recorder менялись; использовать как
отправную точку):

```sql
SELECT strftime('%Y-%m', s.created)            AS month,
       MAX(s.state) - MIN(s.state)             AS cold_m3
FROM   states s
JOIN   states_meta m ON m.metadata_id = s.metadata_id
WHERE  m.metadata = 'sensor.realnye_pokazaniia_khvs_betar'
  AND  s.state NOT IN ('unknown','unavailable')
GROUP  BY month
ORDER  BY month;
```

(для старых схем recorder: `states.entity_id` вместо `states_meta`;
лучше использовать таблицу `statistics`/`statistics_short_term` с
`statistics_meta.statistic_id` и колонкой `sum` — там сбросы
total_increasing уже учтены.)

## После сопряжения: что проверить

1. Реальные entity_id датчиков платы в ZHA/Z2M — подставить в
   `configuration_water.yaml` (места помечены комментариями).
2. Один шаг эмуляции: «Реальные показания» должны вырасти на шаг,
   `utility_meter` — тикнуть; серия из N шагов останавливается кнопкой СТОП.
3. `sensor.battery` ≈ 100 и `sensor.battery_voltage` ≈ 4.19–4.20 при свежей
   банке (прошивка шлёт их по Zigbee; если цифры «прибитые» — проверьте
   делитель и калибровку, см. esphome/README.md).
4. Строка в таблице появится 14-го в 23:55 (для быстрой проверки поменяйте
   время в автоматизации `water_month_log`).

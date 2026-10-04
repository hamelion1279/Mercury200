/*
 * Wi-Fi репитер (повторитель) на ESP32-S3 — ESP-IDF 5.5 / 6.x
 * ===========================================================
 *
 * Устройство одновременно делает две вещи:
 *
 *   1) STA (station)  — подключается к внешней Wi-Fi сети, которую «принимаем»;
 *   2) SoftAP         — поднимает свою Wi-Fi сеть, которую «раздаём»;
 *   3) NAPT (NAT) + IP-форвардинг — клиенты своей сети выходят в интернет
 *      через внешнюю сеть, получая адреса по DHCP от самого репитера.
 *
 * Схема:
 *
 *   [ роутер / внешняя сеть ]            [ ESP32-S3 ]             [ клиенты ]
 *        SSID: MyWiFi            <---  STA   |   SoftAP  --->    SSID: MyWiFi-EXT
 *        192.168.1.1                 192.168.1.50  192.168.4.1    192.168.4.x
 *                                         NAT/DNS-трансляция
 *
 * Важно про одно радио:
 *   у ESP32-S3 один Wi-Fi-радиомодуль, поэтому приём (STA) и раздача (SoftAP)
 *   всегда идут на ОДНОМ канале — точка доступа автоматически переходит на
 *   канал подключения к внешней сети (это делает драйвер, см. документацию
 *   esp_wifi_set_config). Отсюда следствия, описанные в README:
 *   скорость делится примерно вдвое, а при смене канала клиенты кратко
 *   теряют связь.
 *
 * Настройка — через `idf.py menuconfig` → «Wi-Fi repeater (ESP32-S3)»
 * либо правкой sdkconfig.defaults (см. README.md).
 */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_idf_version.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"

/* Светодиод статуса — необязателен, включается в menuconfig */
#if defined(CONFIG_REPEATER_STATUS_LED_GPIO) && (CONFIG_REPEATER_STATUS_LED_GPIO >= 0)
#include "driver/gpio.h"
#define REPEATER_LED_ENABLED 1
#else
#define REPEATER_LED_ENABLED 0
#endif

/* ---------- значения из menuconfig ---------- */
#define UPSTREAM_SSID      CONFIG_REPEATER_UPSTREAM_SSID
#define UPSTREAM_PASSWORD  CONFIG_REPEATER_UPSTREAM_PASSWORD
#define UPSTREAM_BSSID     CONFIG_REPEATER_UPSTREAM_BSSID
#define AP_SSID_CFG        CONFIG_REPEATER_AP_SSID
#define AP_PASSWORD        CONFIG_REPEATER_AP_PASSWORD
#define AP_CHANNEL_CFG     CONFIG_REPEATER_AP_CHANNEL
#define AP_MAX_STA         CONFIG_REPEATER_AP_MAX_STA
#define AP_IP_CFG          CONFIG_REPEATER_AP_IP
#define AP_NETMASK_CFG     CONFIG_REPEATER_AP_NETMASK
#define AP_DNS_CFG         CONFIG_REPEATER_AP_DNS
#define HOSTNAME_CFG       CONFIG_REPEATER_HOSTNAME
#define RESCAN_PERIOD_S    CONFIG_REPEATER_RESCAN_PERIOD
#define LOG_PERIOD_S       CONFIG_REPEATER_STATUS_LOG_PERIOD

/* Порог шифрования внешней сети (choice в menuconfig) */
#if CONFIG_REPEATER_AUTH_OPEN
#define UPSTREAM_AUTHMODE WIFI_AUTH_OPEN
#elif CONFIG_REPEATER_AUTH_WEP
#define UPSTREAM_AUTHMODE WIFI_AUTH_WEP
#elif CONFIG_REPEATER_AUTH_WPA_PSK
#define UPSTREAM_AUTHMODE WIFI_AUTH_WPA_PSK
#elif CONFIG_REPEATER_AUTH_WPA_WPA2_PSK
#define UPSTREAM_AUTHMODE WIFI_AUTH_WPA_WPA2_PSK
#elif CONFIG_REPEATER_AUTH_WPA3_PSK
#define UPSTREAM_AUTHMODE WIFI_AUTH_WPA3_PSK
#elif CONFIG_REPEATER_AUTH_WPA2_WPA3_PSK
#define UPSTREAM_AUTHMODE WIFI_AUTH_WPA2_WPA3_PSK
#else
#define UPSTREAM_AUTHMODE WIFI_AUTH_WPA2_PSK
#endif

/* Логические опции Kconfig: если опция выключена, её макрос не определён вовсе */
#ifdef CONFIG_REPEATER_AP_HIDDEN
#define AP_HIDDEN 1
#else
#define AP_HIDDEN 0
#endif

/* DHCP-опция 6: «выдать клиенту адрес DNS-сервера» */
#define DHCPS_OFFER_DNS 0x02

/* Сколько максимум точек доступа показывать в результатах сканирования */
#define SCAN_MAX_AP 32

/* Подключение не удалось / нет IP — перезапускаем попытку через это время */
#define CONNECT_TIMEOUT_US (20 * 1000 * 1000)
#define DHCP_TIMEOUT_US    (30 * 1000 * 1000)

/* Флаги состояния (EventGroup) */
#define EV_CONNECTED BIT0 /* подключены к внешней сети (ассоциация) */
#define EV_GOT_IP    BIT1 /* получили IP от внешней сети — можно раздавать интернет */

/* Событие «DHCP-сервер выдал адрес клиенту»: имя изменилось в IDF 6.0 */
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
#define REPEATER_EVENT_CLIENT_IP IP_EVENT_ASSIGNED_IP_TO_CLIENT
typedef ip_event_assigned_ip_to_client_t repeater_client_ip_t;
#else
#define REPEATER_EVENT_CLIENT_IP IP_EVENT_AP_STAIPASSIGNED
typedef ip_event_ap_staipassigned_t repeater_client_ip_t;
#endif

#if !CONFIG_LWIP_IPV4_NAPT
#warning "CONFIG_LWIP_IPV4_NAPT выключен — клиенты не получат доступ в интернет. См. sdkconfig.defaults"
#endif
#if !CONFIG_LWIP_IP_FORWARD
#warning "CONFIG_LWIP_IP_FORWARD выключен — маршрутизация пакетов работать не будет. См. sdkconfig.defaults"
#endif

static const char *TAG = "repeater";

/* ---------- состояние устройства ---------- */
typedef struct {
    esp_netif_t *netif_ap;   /* свой интерфейс точки доступа (внутренняя сеть) */
    esp_netif_t *netif_sta;  /* интерфейс клиента внешней сети */
    EventGroupHandle_t events;

    char ap_ssid[33];        /* итоговое имя своей сети */

    volatile bool reconnect_needed;  /* нужно (пере)подключиться к внешней сети */
    volatile int failures;           /* подряд неудачных попыток подключения */
    volatile int64_t next_connect_us; /* когда разрешена следующая попытка */
    volatile int64_t attempt_started_us; /* когда начата текущая попытка */
    volatile uint16_t clients;       /* клиентов на своей точке доступа */
    volatile bool napt_enabled;      /* NAT уже включён */

    uint8_t upstream_channel;        /* канал внешней сети (0 = не найден) */
    uint8_t upstream_bssid[6];       /* лучшая найденная точка доступа внешней сети */
} repeater_ctx_t;

static repeater_ctx_t s_ctx;

/* MAC, заданный вручную в menuconfig (необязательно) */
static uint8_t s_bssid_filter[6];
static bool s_bssid_filter_set;

/* Результаты сканирования (статический буфер, чтобы не растить стек задачи) */
static wifi_ap_record_t s_scan_records[SCAN_MAX_AP];

/* ============================ вспомогательное ============================ */

/* Копирование строки в буфер фиксированной длины с обрезкой и нулём в конце */
static void str_copy(char *dst, size_t dst_size, const char *src)
{
    size_t len = strlen(src);
    if (len > dst_size - 1) {
        len = dst_size - 1;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

/* Дописать строку в конец (если осталось место) */
static void str_append(char *dst, size_t dst_size, const char *suffix)
{
    size_t len = strlen(dst);
    if (len >= dst_size - 1) {
        return;
    }
    size_t room = dst_size - 1 - len;
    size_t n = strlen(suffix);
    if (n > room) {
        n = room;
    }
    memcpy(dst + len, suffix, n);
    dst[len + n] = '\0';
}

/* Строка "AA:BB:CC:DD:EE:FF" -> 6 байт */
static bool mac_from_string(const char *str, uint8_t out[6])
{
    unsigned int v[6];
    if (str == NULL || *str == '\0') {
        return false;
    }
    if (sscanf(str, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        out[i] = (uint8_t)v[i];
    }
    return true;
}

/* Понятная подсказка по коду отключения от внешней сети */
static const char *disconnect_hint(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_AUTH_FAIL:
        return "скорее всего неверный пароль внешней сети";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return "неверный пароль или несовместимое шифрование";
    case WIFI_REASON_NO_AP_FOUND:
        return "сеть не найдена: слишком далеко или выключена";
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        return "сеть найдена, но её шифрование слабее заданного порога";
    case WIFI_REASON_ASSOC_FAIL:
    case WIFI_REASON_CONNECTION_FAIL:
        return "роутер отказал в подключении (перегрузка, фильтр MAC)";
    default:
        return "см. код причины Wi-Fi";
    }
}

/* Индикация состояния светодиодом */
static void repeater_led_init(void)
{
#if REPEATER_LED_ENABLED
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << CONFIG_REPEATER_STATUS_LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
#endif
}

static void repeater_led_update(bool on)
{
#if REPEATER_LED_ENABLED
#if CONFIG_REPEATER_STATUS_LED_ACTIVE_LOW
    gpio_set_level(CONFIG_REPEATER_STATUS_LED_GPIO, on ? 0 : 1);
#else
    gpio_set_level(CONFIG_REPEATER_STATUS_LED_GPIO, on ? 1 : 0);
#endif
#else
    (void)on;
#endif
}

/* ====================== своя точка доступа (SoftAP) ====================== */

/* Подсеть своей сети: по умолчанию 192.168.4.1/24, можно поменять в menuconfig */
static void repeater_configure_ap_netif(void)
{
    if (strcmp(AP_IP_CFG, "192.168.4.1") == 0 && strcmp(AP_NETMASK_CFG, "255.255.255.0") == 0) {
        return; /* стандартные значения — netif уже сконфигурирован по умолчанию */
    }

    esp_ip4_addr_t ip, mask;
    if (esp_netif_str_to_ip4(AP_IP_CFG, &ip) != ESP_OK || esp_netif_str_to_ip4(AP_NETMASK_CFG, &mask) != ESP_OK) {
        ESP_LOGE(TAG, "Не разобрать IP/маску (%s / %s), оставляю 192.168.4.1/24", AP_IP_CFG, AP_NETMASK_CFG);
        return;
    }

    esp_netif_ip_info_t info = {
        .ip = ip,
        .netmask = mask,
        .gw = ip,
    };

    /* DHCP-сервер придётся перезапустить: пул адресов считается от IP интерфейса */
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_stop(s_ctx.netif_ap));
    ESP_ERROR_CHECK(esp_netif_set_ip_info(s_ctx.netif_ap, &info));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_start(s_ctx.netif_ap));
    ESP_LOGI(TAG, "Своя сеть: адрес репитера %s, маска %s", AP_IP_CFG, AP_NETMASK_CFG);
}

/* Настройка точки доступа: имя, пароль, канал, число клиентов */
static void repeater_apply_ap_config(void)
{
    /* Имя своей сети: из menuconfig либо "<внешняя сеть>-EXT" */
    if (AP_SSID_CFG[0] != '\0') {
        str_copy(s_ctx.ap_ssid, sizeof(s_ctx.ap_ssid), AP_SSID_CFG);
    } else {
        str_copy(s_ctx.ap_ssid, sizeof(s_ctx.ap_ssid), UPSTREAM_SSID);
        str_append(s_ctx.ap_ssid, sizeof(s_ctx.ap_ssid), "-EXT");
    }

    /* Канал: канал внешней сети, если он известен, иначе — заданный в menuconfig */
    uint8_t channel = (s_ctx.upstream_channel != 0) ? s_ctx.upstream_channel
                                                    : (uint8_t)(AP_CHANNEL_CFG ? AP_CHANNEL_CFG : 1);

    wifi_config_t cfg = { 0 };
    str_copy((char *)cfg.ap.ssid, sizeof(cfg.ap.ssid), s_ctx.ap_ssid);
    str_copy((char *)cfg.ap.password, sizeof(cfg.ap.password), AP_PASSWORD);
    cfg.ap.ssid_len = strlen(s_ctx.ap_ssid);
    cfg.ap.channel = channel;
    cfg.ap.max_connection = AP_MAX_STA;
    cfg.ap.ssid_hidden = AP_HIDDEN;
    cfg.ap.pmf_cfg.capable = true;   /* совместимость с WPA3-клиентами */
    cfg.ap.pmf_cfg.required = false;

    /* Пароль короче 8 символов запрещён в WPA2 — такая сеть будет открытой */
    if (strlen(AP_PASSWORD) >= 8) {
        cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        cfg.ap.authmode = WIFI_AUTH_OPEN;
        if (AP_PASSWORD[0] != '\0') {
            ESP_LOGW(TAG, "Пароль своей сети короче 8 символов — точка доступа будет ОТКРЫТОЙ");
        } else {
            ESP_LOGW(TAG, "Пароль своей сети не задан — точка доступа ОТКРЫТАЯ, рекомендуется задать пароль");
        }
    }

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &cfg));

    ESP_LOGI(TAG, "Своя сеть: \"%s\"%s, канал %u, до %u клиентов",
             s_ctx.ap_ssid,
             (cfg.ap.authmode == WIFI_AUTH_OPEN) ? " (без пароля)" : "",
             channel, AP_MAX_STA);
}

/* ========================== внешняя сеть (STA) ========================== */

/*
 * Поиск внешней сети: заодно узнаём канал и лучшую точку доступа.
 * Сканируем лишь когда связи нет — во время сканирования своя точка доступа
 * кратко «уходит» с канала, и клиенты могут отключиться.
 */
static void repeater_scan_upstream(void)
{
    wifi_scan_config_t scan_cfg = {
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = {
            .active = {
                .min = 60,
                .max = 200,
            },
        },
    };

    if (UPSTREAM_SSID[0] != '\0') {
        scan_cfg.ssid = (uint8_t *)UPSTREAM_SSID; /* ищем только нужную сеть */
    }

    ESP_LOGI(TAG, "Поиск внешней сети \"%s\"...", UPSTREAM_SSID[0] ? UPSTREAM_SSID : "<любая>");

    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true /* ждать завершения */);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Сканирование не удалось: %s", esp_err_to_name(err));
        return;
    }

    uint16_t found = 0;
    uint16_t num = 0;
    if (esp_wifi_scan_get_ap_num(&found) == ESP_OK) {
        num = (found > SCAN_MAX_AP) ? SCAN_MAX_AP : found;
    }
    /* Обязательно забираем результаты: драйвер освобождает их память */
    if (esp_wifi_scan_get_ap_records(&num, s_scan_records) != ESP_OK) {
        num = 0;
    }

    int best = -1;
    for (int i = 0; i < num; i++) {
        if (s_bssid_filter_set && memcmp(s_scan_records[i].bssid, s_bssid_filter, 6) != 0) {
            continue; /* нужна конкретная точка доступа */
        }
        if (best < 0 || s_scan_records[i].rssi > s_scan_records[best].rssi) {
            best = i;
        }
    }

    if (best < 0) {
        ESP_LOGW(TAG, "Внешняя сеть не найдена (%u сетей вокруг). Проверьте SSID/дальность/диапазон 2,4 ГГц", (unsigned)num);
        s_ctx.upstream_channel = 0;
        return;
    }

    s_ctx.upstream_channel = s_scan_records[best].primary;
    memcpy(s_ctx.upstream_bssid, s_scan_records[best].bssid, sizeof(s_ctx.upstream_bssid));

    ESP_LOGI(TAG, "Найдена \"%s\": канал %u, RSSI %d дБм, BSSID " MACSTR,
             s_scan_records[best].ssid, s_scan_records[best].primary,
             s_scan_records[best].rssi, MAC2STR(s_scan_records[best].bssid));
}

/* Настройка станции по результатам сканирования */
static void repeater_apply_sta_config(void)
{
    wifi_config_t cfg = { 0 };

    str_copy((char *)cfg.sta.ssid, sizeof(cfg.sta.ssid), UPSTREAM_SSID);
    str_copy((char *)cfg.sta.password, sizeof(cfg.sta.password), UPSTREAM_PASSWORD);

    /*
     * Если канал известен — ищем только на нём (подключение быстрее),
     * иначе прочёсываем все каналы.
     */
    cfg.sta.channel = s_ctx.upstream_channel;
    cfg.sta.scan_method = s_ctx.upstream_channel ? WIFI_FAST_SCAN : WIFI_ALL_CHANNEL_SCAN;
    cfg.sta.threshold.authmode = UPSTREAM_AUTHMODE;
    cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH; /* для WPA3-сетей */
    cfg.sta.pmf_cfg.capable = true;
    cfg.sta.pmf_cfg.required = false;

    if (s_bssid_filter_set) {
        cfg.sta.bssid_set = 1;
        memcpy(cfg.sta.bssid, s_bssid_filter, sizeof(cfg.sta.bssid));
    }

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK) {
        /* Такое бывает, если драйвер прямо сейчас занят попыткой подключения */
        ESP_LOGW(TAG, "esp_wifi_set_config(STA): %s", esp_err_to_name(err));
    }
}

/*
 * Включает «интернет для клиентов»:
 *   - сообщает клиентам DNS-сервер (DHCP-опция 6);
 *   - включает NAPT (NAT) на интерфейсе своей точки доступа.
 * Вызывается после каждого получения IP от внешней сети.
 */
static void repeater_start_internet(void)
{
    /* 1. Какой DNS отдавать клиентам: из menuconfig или от внешней сети */
    esp_netif_dns_info_t dns = { 0 };
    bool have_dns = false;

    if (AP_DNS_CFG[0] != '\0') {
        esp_ip4_addr_t ip4;
        if (esp_netif_str_to_ip4(AP_DNS_CFG, &ip4) == ESP_OK) {
            dns.ip.type = ESP_IPADDR_TYPE_V4;
            dns.ip.u_addr.ip4 = ip4;
            have_dns = true;
        } else {
            ESP_LOGW(TAG, "CONFIG_REPEATER_AP_DNS: не разобрать \"%s\"", AP_DNS_CFG);
        }
    }

    if (!have_dns &&
        esp_netif_get_dns_info(s_ctx.netif_sta, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK &&
        dns.ip.type == ESP_IPADDR_TYPE_V4 && dns.ip.u_addr.ip4.addr != 0) {
        have_dns = true;
    }

    if (have_dns) {
        uint8_t offer_dns = DHCPS_OFFER_DNS;
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_stop(s_ctx.netif_ap));
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_option(s_ctx.netif_ap, ESP_NETIF_OP_SET,
                                                             ESP_NETIF_DOMAIN_NAME_SERVER,
                                                             &offer_dns, sizeof(offer_dns)));
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_set_dns_info(s_ctx.netif_ap, ESP_NETIF_DNS_MAIN, &dns));
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_start(s_ctx.netif_ap));
        ESP_LOGI(TAG, "Клиенты получат DNS " IPSTR, IP2STR(&dns.ip.u_addr.ip4));
    } else {
        ESP_LOGW(TAG, "DNS внешней сети неизвестен — клиенты будут использовать адрес репитера как DNS");
    }

    /* 2. NAT: пакеты из своей сети уходят во внешнюю, подменяя адрес отправителя */
    esp_err_t err = esp_netif_napt_enable(s_ctx.netif_ap);
    if (err == ESP_OK) {
        s_ctx.napt_enabled = true;
        ESP_LOGI(TAG, "NAT (NAPT) включён — клиенты своей сети выходят в интернет");
    } else {
        ESP_LOGE(TAG, "Не удалось включить NAT: %s (нужны CONFIG_LWIP_IP_FORWARD=y и CONFIG_LWIP_IPV4_NAPT=y)",
                 esp_err_to_name(err));
    }
}

/* ============================ события Wi-Fi ============================ */

static void repeater_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            /* Интерфейс станции готов — пора подключаться (этим займётся repeater_task) */
            s_ctx.reconnect_needed = true;
            s_ctx.next_connect_us = 0;
            break;

        case WIFI_EVENT_STA_CONNECTED: {
            wifi_event_sta_connected_t *e = (wifi_event_sta_connected_t *)data;
            ESP_LOGI(TAG, "Подключились к \"%.*s\": канал %u, BSSID " MACSTR,
                     e->ssid_len, (const char *)e->ssid, e->channel, MAC2STR(e->bssid));
            s_ctx.failures = 0;
            xEventGroupSetBits(s_ctx.events, EV_CONNECTED);
            break;
        }

        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *e = (wifi_event_sta_disconnected_t *)data;
            xEventGroupClearBits(s_ctx.events, EV_CONNECTED | EV_GOT_IP);
            s_ctx.napt_enabled = false; /* после переподключения включим NAT заново */

            /* Задержка перед новой попыткой: 1, 2, 4, 8, 16 секунд... */
            s_ctx.failures++;
            int shift = (s_ctx.failures > 5) ? 4 : (s_ctx.failures - 1);
            s_ctx.next_connect_us = esp_timer_get_time() + ((int64_t)1000 << shift) * 1000;
            s_ctx.attempt_started_us = 0;
            s_ctx.reconnect_needed = true;

            ESP_LOGW(TAG, "Связь с \"%s\" потеряна (код %d: %s). Следующая попытка через %d с",
                     UPSTREAM_SSID, e->reason, disconnect_hint(e->reason), 1 << shift);
            break;
        }

        case WIFI_EVENT_AP_STACONNECTED: {
            wifi_event_ap_staconnected_t *e = (wifi_event_ap_staconnected_t *)data;
            s_ctx.clients++;
            ESP_LOGI(TAG, "Клиент подключился к своей сети: " MACSTR " (всего %u)",
                     MAC2STR(e->mac), (unsigned)s_ctx.clients);
            break;
        }

        case WIFI_EVENT_AP_STADISCONNECTED: {
            wifi_event_ap_stadisconnected_t *e = (wifi_event_ap_stadisconnected_t *)data;
            if (s_ctx.clients > 0) {
                s_ctx.clients--;
            }
            ESP_LOGI(TAG, "Клиент отключился от своей сети: " MACSTR " (осталось %u)",
                     MAC2STR(e->mac), (unsigned)s_ctx.clients);
            break;
        }

        default:
            break;
        }
    } else if (base == IP_EVENT) {
        if (id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
            ESP_LOGI(TAG, "Получен IP от внешней сети: " IPSTR ", шлюз " IPSTR,
                     IP2STR(&e->ip_info.ip), IP2STR(&e->ip_info.gw));
            s_ctx.failures = 0;
            s_ctx.attempt_started_us = 0;
            xEventGroupSetBits(s_ctx.events, EV_CONNECTED | EV_GOT_IP);
        } else if (id == REPEATER_EVENT_CLIENT_IP) {
            repeater_client_ip_t *e = (repeater_client_ip_t *)data;
            ESP_LOGI(TAG, "Клиенту " MACSTR " выдан адрес " IPSTR, MAC2STR(e->mac), IP2STR(&e->ip));
        }
    }
}

/* ======================= задача сопровождения (мозг) ======================= */

/* Короткий отчёт о состоянии в лог */
static void repeater_log_status(void)
{
    uint8_t primary = 0;
    wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
    wifi_ap_record_t ap = { 0 };

    esp_wifi_get_channel(&primary, &second);

    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        ESP_LOGI(TAG, "Статус: внешняя сеть \"%s\", канал %u, RSSI %d дБм | своя сеть \"%s\", клиентов %u | NAT %s",
                 ap.ssid, primary, ap.rssi, s_ctx.ap_ssid, (unsigned)s_ctx.clients,
                 s_ctx.napt_enabled ? "включён" : "выключен");
    } else {
        ESP_LOGI(TAG, "Статус: своя сеть \"%s\", канал %u, клиентов %u | внешняя сеть недоступна",
                 s_ctx.ap_ssid, primary, (unsigned)s_ctx.clients);
    }
}

/*
 * Вся логика подключения и восстановления связи:
 *   - после потери связи пытается подключиться с растущей паузой;
 *   - не реже RESCAN_PERIOD_S заново сканирует эфир, обновляя канал и BSSID;
 *   - после получения IP включает NAT и раздаёт DNS;
 *   - мигает светодиодом и раз в LOG_PERIOD_S пишет статус в лог.
 */
static void repeater_task(void *arg)
{
    const TickType_t tick = pdMS_TO_TICKS(500);
    bool led_blink = false;
    /* Первое сканирование уже сделано в app_main — следующее не раньше, чем через RESCAN_PERIOD_S */
    int64_t next_scan_us = esp_timer_get_time() + (int64_t)RESCAN_PERIOD_S * 1000000;
    int64_t next_log_us = 0;

    for (;;) {
        EventBits_t bits = xEventGroupGetBits(s_ctx.events);
        int64_t now = esp_timer_get_time();

        /* Светодиод: горит — интернет есть, мигает — связи с внешней сетью нет */
        led_blink = !led_blink;
        repeater_led_update((bits & EV_GOT_IP) ? true : led_blink);

        if (bits & EV_GOT_IP) {
            /* Связь есть: включаем NAT (один раз на каждое подключение)... */
            if (!s_ctx.napt_enabled) {
                repeater_start_internet();
            }
            /* ...и периодически отчитываемся о состоянии */
            if (LOG_PERIOD_S > 0 && now >= next_log_us) {
                repeater_log_status();
                next_log_us = now + (int64_t)LOG_PERIOD_S * 1000000;
            }
        } else {
            /* Связи нет — восстанавливаем */
            if (s_ctx.reconnect_needed && now >= s_ctx.next_connect_us) {
                if (now >= next_scan_us) {
                    repeater_scan_upstream();      /* ищем сеть и её канал */
                    repeater_apply_sta_config();   /* настраиваем станцию */
                    next_scan_us = now + (int64_t)RESCAN_PERIOD_S * 1000000;
                }
                s_ctx.reconnect_needed = false;
                s_ctx.attempt_started_us = now;
                esp_err_t err = esp_wifi_connect();
                if (err == ESP_OK) {
                    ESP_LOGI(TAG, "Подключаюсь к \"%s\"%s", UPSTREAM_SSID,
                             s_ctx.upstream_channel ? "" : " (поиск по всем каналам)");
                } else if (err != ESP_ERR_WIFI_CONN) {
                    ESP_LOGW(TAG, "esp_wifi_connect: %s", esp_err_to_name(err));
                }
            } else if (s_ctx.attempt_started_us != 0) {
                /* Попытка «зависла» — начинаем заново */
                int64_t timeout = (bits & EV_CONNECTED) ? DHCP_TIMEOUT_US : CONNECT_TIMEOUT_US;
                if (now - s_ctx.attempt_started_us > timeout) {
                    ESP_LOGW(TAG, "%s — перезапускаю подключение",
                             (bits & EV_CONNECTED) ? "IP не получен по DHCP" : "Нет ответа от внешней сети");
                    s_ctx.attempt_started_us = 0;
                    esp_wifi_disconnect(); /* обработчик выставит reconnect_needed */
                }
            }
        }

        vTaskDelay(tick);
    }
}

/* ============================== запуск ============================== */

void app_main(void)
{
    ESP_LOGI(TAG, "Запуск Wi-Fi репитера: принимаю \"%s\", раздаю \"%s\"",
             UPSTREAM_SSID, AP_SSID_CFG[0] ? AP_SSID_CFG : "<имя внешней сети>-EXT");

    if (strlen(UPSTREAM_SSID) > 32) {
        ESP_LOGE(TAG, "Имя внешней сети длиннее 32 символов — исправьте в menuconfig");
        return;
    }
    if (strlen(UPSTREAM_SSID) == 0) {
        ESP_LOGE(TAG, "Не задано имя внешней сети (SSID) — настройте в menuconfig и прошейте заново");
        return;
    }
    if (s_bssid_filter_set == false && UPSTREAM_BSSID[0] != '\0') {
        s_bssid_filter_set = mac_from_string(UPSTREAM_BSSID, s_bssid_filter);
        if (!s_bssid_filter_set) {
            ESP_LOGW(TAG, "CONFIG_REPEATER_UPSTREAM_BSSID: не разобрать \"%s\", игнорирую", UPSTREAM_BSSID);
        }
    }

    /* 1. NVS — нужна драйверу Wi-Fi */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* 2. Сетевая подсистема и цикл событий */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_ctx.events = xEventGroupCreate();

    repeater_led_init();

    /* 3. Обработчики событий Wi-Fi и IP */
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        repeater_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                                        repeater_event_handler, NULL, NULL));

    /* 4. Инициализация Wi-Fi */
    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));
    /* Конфигурацию не пишем во флеш: при переподключениях это лишний износ */
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    /* Точка доступа + станция одновременно */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    /* Без энергосбережения — меньше задержки у клиентов */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    /* 5. Интерфейсы: внутренний (своя сеть) и внешний (принимаемая) */
    s_ctx.netif_ap = esp_netif_create_default_wifi_ap();
    s_ctx.netif_sta = esp_netif_create_default_wifi_sta();
    ESP_ERROR_CHECK(esp_netif_set_hostname(s_ctx.netif_sta, HOSTNAME_CFG));
    repeater_configure_ap_netif();

    /* 6. Конфигурируем точку доступа до старта Wi-Fi,
     *    чтобы наружу не «мигнуло» стандартное имя ESP_xxxx */
    repeater_apply_ap_config();

    /* 7. Старт Wi-Fi */
    ESP_ERROR_CHECK(esp_wifi_start());

    /* 8. Ищем внешнюю сеть и настраиваем свою сеть на её канал.
     *    Одно радио: точка доступа и станция обязаны быть на одном канале. */
    repeater_scan_upstream();
    repeater_apply_sta_config();
    repeater_apply_ap_config();

    /*
     * Трафик, порождённый самим устройством (NTP, HTTP-запросы и т. п.),
     * отправляем во внешнюю сеть. Клиенты при этом ходят через NAT.
     */
    ESP_ERROR_CHECK(esp_netif_set_default_netif(s_ctx.netif_sta));

    /* 9. MAC-адреса: пригодятся, чтобы найти устройство в списке клиентов роутера */
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_STA, mac));
    ESP_LOGI(TAG, "MAC станции (её видит роутер): " MACSTR, MAC2STR(mac));
    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_AP, mac));
    ESP_LOGI(TAG, "MAC точки доступа (её видят клиенты): " MACSTR, MAC2STR(mac));

    /* 10. Задача сопровождения: подключение, повторные попытки, NAT, индикация */
    xTaskCreate(repeater_task, "repeater", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "Готово. Подключитесь к сети \"%s\"%s", s_ctx.ap_ssid,
             (strlen(AP_PASSWORD) >= 8) ? "" : " (без пароля)");
}

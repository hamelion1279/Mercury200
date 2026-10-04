# Минимальные требования проекта: сам проект, а также репитер использует
# Wi-Fi, esp_netif (NAPT), lwip, nvs_flash и стандартные драйверы.
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(wifi-repeater)

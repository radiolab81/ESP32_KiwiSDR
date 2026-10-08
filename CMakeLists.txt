# ESP-IDF-Projekt: KiwiSDR-Audioclient mit DAC-Ausgabe (klassischer ESP32)
#
#   idf.py set-target esp32
#   idf.py menuconfig      # -> "KiwiSDR Client" : WLAN, Host, Frequenz ...
#   idf.py build flash monitor
#
# Die Komponente "kiwi_core" liegt in components/ und wird automatisch gefunden.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(kiwi_esp32)

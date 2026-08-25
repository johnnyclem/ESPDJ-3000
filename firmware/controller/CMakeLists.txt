# ESP-DJ3000 controller firmware — ESP32-C3 custom PCB, pure control surface.
# Build: idf.py set-target esp32c3 && idf.py build flash monitor
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(espdj_controller)

cmake_minimum_required(VERSION 3.16)

# Version del firmware para OTA (24 ago): connectivity_check_ota compara
# esto contra el "version" que devuelve GET /firmware/latest, string exacto
# (no semver real) -- este es un proyecto de una sola persona, no hace
# falta parsear rangos, solo "es igual o no". Subir a mano en cada release
# que se publique en la tabla firmware_releases del backend.
set(PROJECT_VER "0.1.0")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(car_companion)

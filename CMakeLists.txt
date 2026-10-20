# The Orb OS — ESP-IDF project (stage 2 of REWRITE_PLAN.md).
#
# Lives beside the Arduino/PlatformIO tree (src/, platformio.ini) for the length of the
# port: the Arduino firmware keeps building and flashing from there, and this project
# grows module by module in main/ and components/ until it replaces it. How to build,
# and why this version, is in docs/esp-idf.md.
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Pinned. A component that builds on 5.5.6 and not on whatever happens to be installed is
# the kind of fault that costs an evening, so a different ESP-IDF stops the build here
# rather than producing a binary that differs from the one that was tested.
set(ORB_IDF_VERSION "5.5.6")
if(NOT "${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}.${IDF_VERSION_PATCH}" STREQUAL "${ORB_IDF_VERSION}")
    message(FATAL_ERROR "The Orb OS is pinned to ESP-IDF v${ORB_IDF_VERSION}, but this is "
                        "v${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}.${IDF_VERSION_PATCH}. "
                        "See docs/esp-idf.md.")
endif()

project(orb_os)

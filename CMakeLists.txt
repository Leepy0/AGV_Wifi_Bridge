cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "0.1.0")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# main 이 의존하는 컴포넌트만 빌드 (빌드 시간 단축)
idf_build_set_property(MINIMAL_BUILD ON)
project(agv_wifi_bridge)

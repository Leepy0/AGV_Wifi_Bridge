cmake_minimum_required(VERSION 3.16)

# 버전: CI 에서 -DPROJECT_VER=... 로 덮어씀 (태그 또는 커밋 해시 포함)
if(NOT DEFINED PROJECT_VER)
    set(PROJECT_VER "0.2.0")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# main 이 의존하는 컴포넌트만 빌드 (빌드 시간 단축)
idf_build_set_property(MINIMAL_BUILD ON)
project(agv_wifi_bridge)

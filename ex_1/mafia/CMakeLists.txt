# CMake-сборка проекта «Мафия» — альтернатива Makefile (для IDE и ctest).
#
#   cmake -S . -B build/cmake
#   cmake --build build/cmake
#   ctest --test-dir build/cmake --output-on-failure

cmake_minimum_required(VERSION 3.20)
project(mafia LANGUAGES CXX)

# Ровно -std=c++20, без GNU-расширений (иначе CMake подставит -std=gnu++20).
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Проверяем, что в системе есть потоки. Сам флаг -pthread ставим явно ниже:
# при glibc >= 2.34 Threads::Threads считает его ненужным и ничего не добавляет.
find_package(Threads REQUIRED)

# Все .cpp из src/; CONFIGURE_DEPENDS — перепроверять список при каждой сборке.
file(GLOB_RECURSE MAFIA_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/*.cpp)
# Код проекта без main.cpp — его линкуют и программа, и тесты.
set(MAFIA_LIB_SOURCES ${MAFIA_SOURCES})
list(REMOVE_ITEM MAFIA_LIB_SOURCES ${CMAKE_SOURCE_DIR}/src/main.cpp)

# cmake -S . -B build/cmake-tsan -DMAFIA_TSAN=ON — сборка под ThreadSanitizer.
option(MAFIA_TSAN "Собрать с -fsanitize=thread" OFF)

# Общие настройки для любой цели проекта: те же флаги, что в Makefile.
function(mafia_setup_target target)
    target_include_directories(${target} PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_options(${target} PRIVATE -O2 -Wall -Wextra -Wpedantic -pthread)
    target_link_options(${target} PRIVATE -pthread)
    target_link_libraries(${target} PRIVATE Threads::Threads)
    if(MAFIA_TSAN)
        target_compile_options(${target} PRIVATE -fsanitize=thread -g)
        target_link_options(${target} PRIVATE -fsanitize=thread)
    endif()
endfunction()

add_executable(mafia src/main.cpp ${MAFIA_LIB_SOURCES})
mafia_setup_target(mafia)

# Тесты: каждый tests/test_*.cpp — отдельный исполняемый файл и отдельный ctest-тест.
enable_testing()
file(GLOB MAFIA_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/test_*.cpp)
foreach(test_src ${MAFIA_TEST_SOURCES})
    get_filename_component(test_target ${test_src} NAME_WE)
    string(REGEX REPLACE "^test_" "" test_name ${test_target})
    add_executable(${test_target} ${test_src} ${MAFIA_LIB_SOURCES})
    mafia_setup_target(${test_target})
    # Из корня проекта, как в make test: тесты читают config/*.yaml.
    add_test(NAME ${test_name} COMMAND ${test_target} WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endforeach()

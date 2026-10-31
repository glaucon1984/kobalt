cmake_minimum_required(VERSION 3.22)
project(KobaltLauncher LANGUAGES CXX)
if(NOT APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "The launcher supports macOS and Linux")
endif()
find_package(Qt6 6.11 REQUIRED COMPONENTS Core Gui Widgets)
get_filename_component(KOBALT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
add_executable(kobalt-launcher main.cpp)
target_compile_features(kobalt-launcher PRIVATE cxx_std_17)
target_compile_options(kobalt-launcher PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(kobalt-launcher PRIVATE
    KOBALT_ROOT="${KOBALT_ROOT}" KOBALT_QT_DIR="${Qt6_DIR}")
target_link_libraries(kobalt-launcher PRIVATE Qt6::Widgets)
enable_testing()
add_test(NAME launcher.actions-and-processes COMMAND kobalt-launcher --self-test)
set_tests_properties(launcher.actions-and-processes PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=minimal;QT_FORCE_STDERR_LOGGING=1" TIMEOUT 20)

add_test(NAME launcher.kobo-cleanup COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/check-kobo-cleanup.sh")
set_tests_properties(launcher.kobo-cleanup PROPERTIES TIMEOUT 20)

add_test(NAME launcher.dependency-guidance COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/check-dependency-guidance.sh")
set_tests_properties(launcher.dependency-guidance PROPERTIES TIMEOUT 20)

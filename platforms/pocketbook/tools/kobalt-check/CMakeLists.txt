# Kobalt's third-party dependencies (CREngine and friends) built with the PocketBook toolchain
# (target kobalt-dependencies; tools/rebuild-deps.sh copies the result to KOBALT_DEPS for the
# application build), plus a compile check of the portable sources (src/) without linking.
# Mirrors platforms/kobo/CMakeLists.txt without the Nickel parts. Started as the port's
# feasibility check (doc/pocketbook-notes.md).
cmake_minimum_required(VERSION 3.22)
project(KobaltPocketBookCheck LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(KOBALT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." CACHE PATH "Kobalt source tree (this repository)")
# Normalised: the same file under two spellings is "multiple rules" to ninja.
get_filename_component(KOBALT_ROOT "${KOBALT_ROOT}" ABSOLUTE)
set(KOBALT_QT_MAJOR 5)
# Qt 5.15 already provides Qt5::GuiPrivate (the Kobo build shims it for Kobo's Qt 5.2).
find_package(Qt5 REQUIRED COMPONENTS Core Gui Widgets Network Sql Sensors)

add_subdirectory("${KOBALT_ROOT}/dependencies" "${CMAKE_CURRENT_BINARY_DIR}/dependencies")
add_subdirectory("${KOBALT_ROOT}/assets" "${CMAKE_CURRENT_BINARY_DIR}/assets")
set(source "${KOBALT_ROOT}/src")
file(READ "${KOBALT_ASSETS}/manifest.json" assets)
string(JSON KOBALT_VERSION GET "${assets}" version)

file(READ "${CRENGINE_SOURCE}/cr3gui/data/html5.css" READER_HTML_CSS)
configure_file("${source}/engine/crengine/reader_styles.h.in" reader_styles.h @ONLY)
file(GLOB patterns RELATIVE "${CRENGINE_SOURCE}/cr3gui/data/hyph" "${CRENGINE_SOURCE}/cr3gui/data/hyph/*.pattern")
list(SORT patterns)
list(TRANSFORM patterns PREPEND "\"")
list(TRANSFORM patterns APPEND "\"")
list(JOIN patterns ", " READER_HYPHENATION_PATTERNS)
configure_file("${source}/engine/crengine/reader_hyphenation.h.in" reader_hyphenation.h @ONLY)
set(KOBALT_COMMIT "")
option(KOBALT_COMMIT_MODIFIED "" OFF)
configure_file("${source}/support/build_info.h.in" build_info.h @ONLY)

# Everything portable, including the library and the Qt Sensors based orientation code.
file(GLOB_RECURSE shared CONFIGURE_DEPENDS "${source}/*.cpp" "${source}/*.h")
add_library(kobalt-check OBJECT ${shared})
set_source_files_properties("${source}/storage/annotation_database.cpp" PROPERTIES
    COMPILE_DEFINITIONS KOBALT_SQLITE_ANNOTATIONS_DEFAULT=1)
target_include_directories(kobalt-check PRIVATE "${CMAKE_CURRENT_BINARY_DIR}"
    "${source}/interfaces/reader/ui/controls")
target_include_directories(kobalt-check SYSTEM PRIVATE "${CRENGINE_SOURCE}/crengine/include" "${CRENGINE_BUILD}"
    "${prefix}/include" "${prefix}/include/harfbuzz" "${prefix}/include/freetype2" "${prefix}/include/fribidi")
target_compile_options(kobalt-check PRIVATE -Wall -Wextra -Wno-missing-field-initializers -fvisibility=hidden
    "-iquote${KOBALT_ROOT}" "-iquote${source}")
target_compile_definitions(kobalt-check PRIVATE KOBALT_READER_HARDWARE KOBALT_PAGE_ANIMATIONS
    KOBALT_VERSION="${KOBALT_VERSION}"
    KOBALT_CONFIG_DIR="/mnt/ext1/system/config/kobalt" KOBALT_SYSTEM_FONT_DIRECTORY="/ebrmain/share/fonts")
include("${KOBALT_ROOT}/platforms/cache-policy.cmake")
target_compile_definitions(kobalt-check PRIVATE KOBALT_LAYOUT_POLICY="${layout_policy}")
# OFF compiles src/ even when a dependency failed to build (headers come from the fetched
# sources and the partial install prefix), to see src/ errors on their own.
option(KOBALT_CHECK_GATE_ON_DEPS "Build the third-party libraries before compiling src/" ON)
if(KOBALT_CHECK_GATE_ON_DEPS)
    add_dependencies(kobalt-check kobalt-dependencies)
endif()
target_link_libraries(kobalt-check PRIVATE Qt5::Widgets Qt5::GuiPrivate Qt5::Network Qt5::Sql Qt5::Sensors)

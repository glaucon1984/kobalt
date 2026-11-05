# Kobalt's third-party dependencies (CREngine and friends) built with the PocketBook toolchain
# into the shared dependency cache at configure time (dependencies/cache.cmake;
# tools/rebuild-deps.sh copies the set to KOBALT_DEPS for the application build), plus a compile
# check of the portable sources (src/) without linking.
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

# The set includes libpng (dependencies/cache.cmake), which the static Qt links too.
option(KOBALT_POCKETBOOK "Build the dependency set for the PocketBook sysroot" ON)
include("${KOBALT_ROOT}/dependencies/cache.cmake")
kobalt_dependencies()
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
target_include_directories(kobalt-check PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_include_directories(kobalt-check SYSTEM PRIVATE "${CRENGINE_SOURCE}/crengine/include" "${CRENGINE_BUILD}"
    "${prefix}/include" "${prefix}/include/harfbuzz" "${prefix}/include/freetype2" "${prefix}/include/fribidi")
target_compile_options(kobalt-check PRIVATE -Wall -Wextra -Wno-missing-field-initializers -fvisibility=hidden
    "-iquote${KOBALT_ROOT}" "-iquote${source}")
target_compile_definitions(kobalt-check PRIVATE KOBALT_READER_HARDWARE KOBALT_PAGE_ANIMATIONS
    KOBALT_VERSION="${KOBALT_VERSION}"
    KOBALT_CONFIG_DIR="/mnt/ext1/system/config/kobalt" KOBALT_SYSTEM_FONT_DIRECTORY="/ebrmain/share/fonts")
include("${KOBALT_ROOT}/platforms/cache-policy.cmake")
target_compile_definitions(kobalt-check PRIVATE KOBALT_LAYOUT_POLICY="${layout_policy}")
target_link_libraries(kobalt-check PRIVATE Qt5::Widgets Qt5::GuiPrivate Qt5::Network Qt5::Sql Qt5::Sensors)

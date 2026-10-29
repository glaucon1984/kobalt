# Kobalt for PocketBook: Kobalt's reader (src/ without the standalone library), the
# ReaderController from the Android target (with its KOBALT_POCKETBOOK hooks), the pbink inkview
# platform plugin (pbink/) and a small host (host/), linked into one static-Qt executable.
# Third-party libraries come prebuilt from KOBALT_DEPS (Kobalt's own dependencies/ recipes, built
# once by tools/kobalt-check with the same toolchain; tools/rebuild-deps.sh). See doc/building.md.
cmake_minimum_required(VERSION 3.22)
project(KobaltPocketBook LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(KOBALT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "Kobalt source tree (this repository)")
# Normalised: the same file under two spellings is "multiple rules" to ninja.
get_filename_component(KOBALT_ROOT "${KOBALT_ROOT}" ABSOLUTE)
set(KOBALT_DEPS "/work/kobalt-deps" CACHE PATH "Prebuilt Kobalt dependencies: install/, crengine-src/, crengine-build/")
set(PB_SYSROOT "/work/sdk/SDK-B288/usr/arm-obreey-linux-gnueabi/sysroot" CACHE PATH "PocketBook SDK sysroot (inkview)")
set(PB_CACERT "/work/cacert.pem" CACHE FILEPATH "Mozilla CA bundle packaged for TLS (the firmware has no root store Qt finds)")
set(prefix "${KOBALT_DEPS}/install")
set(CRENGINE_SOURCE "${KOBALT_DEPS}/crengine-src")
set(CRENGINE_BUILD "${KOBALT_DEPS}/crengine-build")
set(KOBALT_QT_MAJOR 5)

find_package(Qt5 REQUIRED COMPONENTS Core Gui Widgets Network Sql Sensors
    FbSupport EventDispatcherSupport ServiceSupport FontDatabaseSupport)

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
set(KOBALT_COMMIT "" CACHE STRING "Source commit, passed by the launcher (tools/remote-build-kobalt.sh)")
option(KOBALT_COMMIT_MODIFIED "Source has uncommitted changes" OFF)
configure_file("${source}/support/build_info.h.in" build_info.h @ONLY)

# Like Kobo: the reader without the standalone Library model and view.
file(GLOB_RECURSE shared CONFIGURE_DEPENDS "${source}/*.cpp" "${source}/*.h")
list(FILTER shared EXCLUDE REGEX "/(library|interfaces/library)/")
# The scenario player belongs to Kobo's evaluation packages only.
list(FILTER shared EXCLUDE REGEX "/src/scenario/")
set(plugin "${CMAKE_CURRENT_SOURCE_DIR}/pbink")
set(host "${CMAKE_CURRENT_SOURCE_DIR}/host")
add_executable(kobalt-pb ${shared}
    "${KOBALT_ROOT}/platforms/android/reader.cpp" "${KOBALT_ROOT}/platforms/android/reader.h"
    "${host}/main.cpp" "${host}/host.cpp" "${host}/host.h" "${host}/services.cpp" "${host}/services.h"
    "${host}/display.cpp" "${host}/orientation_sensor.cpp" "${host}/orientation_sensor.h"
    "${host}/sleep_screen_files.cpp" "${host}/sleep_screen_files.h"
    "${host}/explorer_progress.cpp" "${host}/explorer_progress.h" "${host}/epub_cfi.cpp" "${host}/epub_cfi.h"
    "${host}/futex_compat.cpp"
    "${plugin}/main.cpp" "${plugin}/pbinkintegration.cpp" "${plugin}/pbinkintegration.h"
    "${plugin}/pbinkscreen.cpp" "${plugin}/pbinkscreen.h" "${plugin}/pbinkevents.cpp" "${plugin}/pbinkevents.h")
set_source_files_properties("${source}/storage/annotation_database.cpp" PROPERTIES
    COMPILE_DEFINITIONS KOBALT_SQLITE_ANNOTATIONS_DEFAULT=1)
# Reading time on a clock that keeps counting through the device's light sleep, as on Kobo.
set_source_files_properties("${source}/reading/reading_timer.cpp" PROPERTIES
    COMPILE_DEFINITIONS KOBALT_READING_CLOCK_BOOTTIME=1)
target_include_directories(kobalt-pb PRIVATE "${CMAKE_CURRENT_BINARY_DIR}" "${host}" "${plugin}"
    "${source}/interfaces/reader/ui/controls")
target_include_directories(kobalt-pb SYSTEM PRIVATE "${CRENGINE_SOURCE}/crengine/include" "${CRENGINE_BUILD}"
    "${prefix}/include" "${prefix}/include/harfbuzz" "${prefix}/include/freetype2" "${prefix}/include/fribidi"
    "${PB_SYSROOT}/usr/local/include"
    ${Qt5Gui_PRIVATE_INCLUDE_DIRS} ${Qt5Core_PRIVATE_INCLUDE_DIRS} ${Qt5Widgets_PRIVATE_INCLUDE_DIRS}
    ${Qt5FbSupport_PRIVATE_INCLUDE_DIRS} ${Qt5EventDispatcherSupport_PRIVATE_INCLUDE_DIRS}
    ${Qt5ServiceSupport_PRIVATE_INCLUDE_DIRS} ${Qt5FontDatabaseSupport_PRIVATE_INCLUDE_DIRS})
target_compile_options(kobalt-pb PRIVATE -Wall -Wextra -Wno-missing-field-initializers
    "-iquote${KOBALT_ROOT}" "-iquote${source}")
target_compile_definitions(kobalt-pb PRIVATE KOBALT_READER_HARDWARE KOBALT_POCKETBOOK=1 QT_STATICPLUGIN
    KOBALT_VERSION="${KOBALT_VERSION}"
    KOBALT_CONFIG_DIR="/mnt/ext1/applications/kobalt/profile"
    KOBALT_SYSTEM_FONT_DIRECTORY="/mnt/ext1/system/fonts")
include("${KOBALT_ROOT}/platforms/cache-policy.cmake")
target_compile_definitions(kobalt-pb PRIVATE KOBALT_LAYOUT_POLICY="${layout_policy}")

# Notices and acknowledgments, as the Kobo build embeds them.
set(resources "${KOBALT_ROOT}/assets")
file(GLOB notices CONFIGURE_DEPENDS "${resources}/notices/*")
set(qrc "<RCC><qresource prefix=\"/kobalt\">")
foreach(path "${resources}/acknowledgments.json" ${notices})
    file(RELATIVE_PATH alias "${resources}" "${path}")
    string(APPEND qrc "<file alias=\"${alias}\">${path}</file>")
endforeach()
string(APPEND qrc "</qresource></RCC>")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/notices.qrc" CONTENT "${qrc}" @ONLY)
qt5_add_resources(compiled_resources "${CMAKE_CURRENT_BINARY_DIR}/notices.qrc")
target_sources(kobalt-pb PRIVATE ${compiled_resources})

# As on Kobo and Android: route CREngine's glyph loading through Kobalt's wrappers
# (src/engine/crengine/font_weight.cpp) by renaming the FreeType calls inside libcrengine.a.
set(engine "${CMAKE_CURRENT_BINARY_DIR}/libcrengine-reader.a")
add_custom_command(OUTPUT "${engine}"
    COMMAND "${CMAKE_OBJCOPY}" --redefine-sym FT_Load_Glyph=kobalt_font_load_glyph
        --redefine-sym FT_Render_Glyph=kobalt_font_render_glyph "${CRENGINE_BUILD}/libcrengine.a" "${engine}"
    DEPENDS "${CRENGINE_BUILD}/libcrengine.a" VERBATIM)
add_custom_target(reader-engine DEPENDS "${engine}")
add_dependencies(kobalt-pb reader-engine)

target_link_directories(kobalt-pb PRIVATE "${prefix}/lib" "${PB_SYSROOT}/usr/local/lib")
target_link_libraries(kobalt-pb PRIVATE
    Qt5::Widgets Qt5::Network Qt5::Sensors Qt5::Sql
    Qt5::FbSupport Qt5::EventDispatcherSupport Qt5::ServiceSupport Qt5::FontDatabaseSupport
    Qt5::QJpegPlugin Qt5::QGifPlugin
    "${engine}"
    "${prefix}/lib/libharfbuzz.a" "${prefix}/lib/libxxhash.a" "${prefix}/lib/libutf8proc.a"
    "${prefix}/lib/libjpeg.a" "${prefix}/lib/liblunasvg.a" "${prefix}/lib/libfreetype.a"
    "${prefix}/lib/libfribidi.a" "${prefix}/lib/libunibreak.a" "${prefix}/lib/libpng16.a"
    inkview z dl pthread)
target_link_options(kobalt-pb PRIVATE
    "LINKER:--whole-archive,${prefix}/lib/libfreetype.a,--no-whole-archive")

# Package: binary + runtime assets, ready to copy to applications/kobalt/ on the device.
set(package "${CMAKE_CURRENT_BINARY_DIR}/package")
add_custom_target(package
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${package}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${package}/kobalt"
    COMMAND "${CMAKE_STRIP}" -o "${package}/kobalt/kobalt-pb" "$<TARGET_FILE:kobalt-pb>"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${KOBALT_ASSETS}" "${package}/kobalt/assets"
    COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_CURRENT_SOURCE_DIR}/device/kobalt.app" "${package}/kobalt.app"
    # CA bundle for TLS (Mozilla's, fetched by the build host); the launcher points OpenSSL at it.
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${PB_CACERT}" "${package}/kobalt/cacert.pem"
    COMMAND "${CMAKE_COMMAND}" -E chdir "${package}" "${CMAKE_COMMAND}" -E tar czf "${CMAKE_CURRENT_BINARY_DIR}/kobalt-pb.tar.gz" -- kobalt kobalt.app
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    DEPENDS kobalt-pb ${KOBALT_ASSET_INPUTS} VERBATIM)

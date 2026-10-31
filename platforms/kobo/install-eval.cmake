# Copies an evaluation package onto a Kobo connected over USB.
#   cmake -DPACKAGE=build/kobo-qt5-eval/Kobalt-qt5-eval.zip -DDEVICE=/Volumes/KOBOeReader
#       -P platforms/kobo/install-eval.cmake
if(NOT DEFINED PACKAGE OR NOT DEFINED DEVICE)
    message(FATAL_ERROR "PACKAGE and DEVICE are required")
endif()
if(NOT EXISTS "${PACKAGE}")
    message(FATAL_ERROR "No package at ${PACKAGE}. Build it with: bash platforms/kobo/build.sh qt5-eval")
endif()
if(NOT EXISTS "${DEVICE}/.kobo")
    message(FATAL_ERROR "${DEVICE} is not the storage of a Kobo")
endif()
if(DEFINED SCENARIO_FILTER AND NOT SCENARIO_FILTER MATCHES "^[a-z0-9.-]+$")
    message(FATAL_ERROR "SCENARIO_FILTER must be a scenario identifier or prefix")
endif()
file(ARCHIVE_EXTRACT INPUT "${PACKAGE}" DESTINATION "${DEVICE}")
if(DEFINED SCENARIO_FILTER)
    set(scenarios "${DEVICE}/.adds/kobalt/scenarios/scenarios.json")
    file(READ "${scenarios}" scenario_json)
    string(JSON scenario_json SET "${scenario_json}" filter "\"${SCENARIO_FILTER}\"")
    file(WRITE "${scenarios}" "${scenario_json}\n")
endif()
# Results from an earlier run must not pass for this one.
file(REMOVE "${DEVICE}/.adds/kobalt/diagnostics/scenarios.jsonl")
file(GLOB dictionaries "${DEVICE}/.kobo/dict/dicthtml*.zip")
if(NOT dictionaries)
    message(WARNING "No dictionary is installed on the Kobo. The dictionary scenarios will be skipped.")
endif()
message(STATUS "Installed ${PACKAGE}. Eject the Kobo and let it restart. Then open "
    "\"Kobalt Benchmark\" in Kobalt and leave it alone. The scenarios start two seconds after "
    "the book opens and end with a scenario-run-end event in .adds/kobalt/diagnostics/events.jsonl. "
    "Collect the results with platforms/kobo/collect-eval.cmake.")

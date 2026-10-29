# CMake toolchain for the PocketBook SDK 6.5 (B288) gcc 6.3 toolchain, inside the pbqt-build
# container (SDK at /work/sdk, static Qt at /work/qt-pb).
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(PB_SDK "/work/sdk/SDK-B288" CACHE PATH "PocketBook SDK root")
set(PB_QT "/work/qt-pb" CACHE PATH "Static Qt 5 built for the SDK")
set(CMAKE_SYSROOT "${PB_SDK}/usr/arm-obreey-linux-gnueabi/sysroot")
set(CMAKE_C_COMPILER "${PB_SDK}/usr/bin/arm-obreey-linux-gnueabi-gcc")
set(CMAKE_CXX_COMPILER "${PB_SDK}/usr/bin/arm-obreey-linux-gnueabi-g++")
set(CMAKE_AR "${PB_SDK}/usr/bin/arm-obreey-linux-gnueabi-ar" CACHE FILEPATH "")
set(CMAKE_RANLIB "${PB_SDK}/usr/bin/arm-obreey-linux-gnueabi-ranlib" CACHE FILEPATH "")
set(CMAKE_STRIP "${PB_SDK}/usr/bin/arm-obreey-linux-gnueabi-strip" CACHE FILEPATH "")
set(CMAKE_OBJCOPY "${PB_SDK}/usr/bin/arm-obreey-linux-gnueabi-objcopy" CACHE FILEPATH "")
# The toolchain wrapper already injects -mcpu=cortex-a8 -march=armv7-a -mfpu=neon -mfloat-abi=softfp.
set(CMAKE_C_FLAGS_INIT "-fsigned-char")
set(CMAKE_CXX_FLAGS_INIT "-fsigned-char")
# CACHE so that Kobalt's dependency recipes can override it with -DCMAKE_FIND_ROOT_PATH=<prefix>
# (a plain set() here would shadow their value and make harfbuzz find the sysroot's old FreeType).
set(CMAKE_FIND_ROOT_PATH "${CMAKE_SYSROOT};${PB_QT}" CACHE STRING "Target dependency roots")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_PREFIX_PATH "${PB_QT};${PB_QT}/lib/cmake" CACHE STRING "Target dependency prefixes")

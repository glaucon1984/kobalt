# CMake toolchain: Debian's arm-linux-gnueabi GCC 12 paired with the PocketBook SDK 6.5 sysroot
# (glibc 2.23), the way kobuild pairs Ubuntu's GCC 11 with Kobo's sysroot. -B/-L make the link
# use the sysroot's crt and libc (Debian's cross GCC otherwise prefers its own glibc 2.36),
# -isystem puts the sysroot's C headers before Debian's, libstdc++/libgcc are linked statically.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(PB_SDK "/work/sdk/SDK-B288" CACHE PATH "PocketBook SDK root")
set(PB_QT "/work/qt-pb" CACHE PATH "Static Qt 5 built for the SDK")
set(CMAKE_SYSROOT "${PB_SDK}/usr/arm-obreey-linux-gnueabi/sysroot")
set(CMAKE_C_COMPILER arm-linux-gnueabi-gcc)
set(CMAKE_CXX_COMPILER arm-linux-gnueabi-g++)
set(CMAKE_AR arm-linux-gnueabi-ar CACHE FILEPATH "")
set(CMAKE_RANLIB arm-linux-gnueabi-ranlib CACHE FILEPATH "")
set(CMAKE_STRIP arm-linux-gnueabi-strip CACHE FILEPATH "")
set(CMAKE_OBJCOPY arm-linux-gnueabi-objcopy CACHE FILEPATH "")
# Header search order rebuilt by hand so that the sysroot's glibc 2.23 headers replace Debian's
# glibc 2.36 headers while GCC's own and libstdc++'s wrappers keep their place in front.
set(gcc_inc "/usr/lib/gcc-cross/arm-linux-gnueabi/12")
set(cxx_inc "/usr/arm-linux-gnueabi/include/c++/12")
set(pb_flags "-B${CMAKE_SYSROOT}/usr/lib/ -march=armv7-a -mfpu=neon -mfloat-abi=softfp -fsigned-char -no-pie -fno-pie")
set(c_inc "-nostdinc -isystem ${gcc_inc}/include -isystem ${gcc_inc}/include-fixed -isystem ${CMAKE_SYSROOT}/usr/include")
# /work/gcc12-overlay: tools/make-cxxconfig-overlay.sh (libstdc++ config without glibc>2.23 features)
set(cxx_incs "-nostdinc -isystem /work/gcc12-overlay -isystem ${cxx_inc} -isystem ${cxx_inc}/arm-linux-gnueabi -isystem ${cxx_inc}/backward -isystem ${gcc_inc}/include -isystem ${gcc_inc}/include-fixed -isystem ${CMAKE_SYSROOT}/usr/include")
set(CMAKE_C_FLAGS_INIT "${pb_flags} ${c_inc}")
set(CMAKE_CXX_FLAGS_INIT "${pb_flags} ${cxx_incs}")
# /work/gcc12-compat/glibc_compat.o (tools/build-glibc-compat.sh) supplies glibc 2.3x symbols that
# GCC 12's libstdc++ archive references (__libc_single_threaded, getentropy).
# -pthread: libstdc++ 12 expects glibc >= 2.34 (pthread inside libc); here libpthread is separate.
# GCC 12's own lib dir first: with the sysroot dirs ahead of it, -static-libstdc++ would pick the
# sysroot's libstdc++ 6.3 archive (missing std::__throw_bad_array_new_length and friends).
set(pb_link "-L${gcc_inc} -L${CMAKE_SYSROOT}/lib -L${CMAKE_SYSROOT}/usr/lib -no-pie -static-libstdc++ -static-libgcc -pthread -Wl,-z,noexecstack /work/gcc12-compat/glibc_compat.o")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${pb_link}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${pb_link}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${pb_link}")
set(CMAKE_FIND_ROOT_PATH "${CMAKE_SYSROOT};${PB_QT}" CACHE STRING "Target dependency roots")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_PREFIX_PATH "${PB_QT};${PB_QT}/lib/cmake" CACHE STRING "Target dependency prefixes")

cmake_minimum_required(VERSION 3.16)
project(swdplot LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBDW REQUIRED IMPORTED_TARGET libdw)   # elfutils: DWARF
pkg_check_modules(LIBELF REQUIRED IMPORTED_TARGET libelf) # elfutils: ELF symbols

# Everything except main() lives in a library so the tests can link against it.
add_library(swdcore STATIC
    src/Types.h
    src/DwarfNode.cpp        src/DwarfNode.h
    src/ElfModel.cpp         src/ElfModel.h
    src/SvdRegs.cpp          src/SvdRegs.h
    src/Series.cpp           src/Series.h
    src/Processing.cpp       src/Processing.h
    src/ValueParse.cpp       src/ValueParse.h
    src/Poller.cpp           src/Poller.h
    src/StreamFormat.cpp     src/StreamFormat.h
    src/StreamSink.cpp       src/StreamSink.h
    src/PlotWidget.cpp       src/PlotWidget.h
    src/GraphTab.cpp         src/GraphTab.h
    src/StreamDialog.cpp     src/StreamDialog.h
    src/RegistersDialog.cpp  src/RegistersDialog.h
    src/MainWindow.cpp       src/MainWindow.h
    src/Theme.cpp            src/Theme.h
)
target_include_directories(swdcore PUBLIC src)
target_link_libraries(swdcore PUBLIC Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network
                                     PkgConfig::LIBDW PkgConfig::LIBELF)
target_compile_options(swdcore PUBLIC -Wall -Wextra -Wno-missing-field-initializers)

add_executable(swdplot src/main.cpp)
target_link_libraries(swdplot PRIVATE swdcore)

option(SWDPLOT_BUILD_TESTS "Build the unit tests" ON)
if(SWDPLOT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

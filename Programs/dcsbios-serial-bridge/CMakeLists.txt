cmake_minimum_required(VERSION 3.20)
project(dcsbios_serial_bridge LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(dcsbios-serial-bridge WIN32
    src/main.cpp
    src/BiosProtocol.hpp
    src/ControlDatabase.hpp
    src/DeviceRegistry.hpp
)

target_compile_definitions(dcsbios-serial-bridge PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_link_libraries(dcsbios-serial-bridge PRIVATE
    ws2_32
)

if(MSVC)
    target_compile_options(dcsbios-serial-bridge PRIVATE /W4 /permissive-)
endif()

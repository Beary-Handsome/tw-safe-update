cmake_minimum_required(VERSION 3.16)
project(twsu-tray LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets DBus)
find_package(KF6StatusNotifierItem REQUIRED)
find_package(KF6Parts REQUIRED)

add_executable(twsu-tray twsu-tray.cpp)
target_link_libraries(twsu-tray PRIVATE
    Qt6::Widgets Qt6::DBus KF6::StatusNotifierItem KF6::Parts)

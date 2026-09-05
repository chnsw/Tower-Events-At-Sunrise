cmake_minimum_required(VERSION 3.20)
project(TowerEventsTests LANGUAGES CXX)
enable_testing()
set(EVENT_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/../Sunrise/src")
add_executable(loot_pickup_test loot_pickup_test.cpp
    "${EVENT_SOURCE}/middleware/bap/activity_message/loot_pickup.cpp"
    "${EVENT_SOURCE}/middleware/encoding/bit_reader.cpp")
target_include_directories(loot_pickup_test PRIVATE "${EVENT_SOURCE}")
target_compile_features(loot_pickup_test PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(loot_pickup_test PRIVATE /W4 /UNDEBUG)
else()
    target_compile_options(loot_pickup_test PRIVATE -Wall -Wextra -UNDEBUG)
endif()
add_test(NAME loot_pickup COMMAND loot_pickup_test)

if(WIN32)
    add_executable(festival_pickups_test festival_pickups_test.cpp
        "${EVENT_SOURCE}/server/bap/encrypted/activity_message/festival_pickups.cpp"
        "${EVENT_SOURCE}/middleware/bap/activity_message/loot_pickup.cpp"
        "${EVENT_SOURCE}/middleware/bap/activity_message/activity_incident_parser.cpp"
        "${EVENT_SOURCE}/middleware/bap/activity_message/activity_incident_encoder.cpp"
        "${EVENT_SOURCE}/middleware/encoding/bit_reader.cpp"
        "${EVENT_SOURCE}/middleware/encoding/bit_writer.cpp")
    target_include_directories(festival_pickups_test PRIVATE "${EVENT_SOURCE}")
    target_compile_features(festival_pickups_test PRIVATE cxx_std_20)
    target_compile_definitions(festival_pickups_test PRIVATE NOMINMAX)
    target_compile_options(festival_pickups_test PRIVATE /W4 /UNDEBUG)
    add_test(NAME festival_pickups COMMAND festival_pickups_test)
endif()

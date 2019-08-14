# Required to avoid warnings when setting visibility
cmake_policy(SET CMP0063 NEW)

add_executable(include-tests ${CMAKE_CURRENT_SOURCE_DIR}/include.cpp)

target_link_libraries(include-tests PRIVATE
    mapbox-base
)

add_test(NAME include-tests COMMAND include-tests)

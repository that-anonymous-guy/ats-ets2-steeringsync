cmake_minimum_required(VERSION 3.20)
project(SteeringSyncUniversal VERSION 1.0 LANGUAGES CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "This project requires Windows x64 (Visual Studio 2022 x64).")
endif()

foreach(game ATS ETS2)
    add_library(SteeringSync_${game}_1_61 SHARED src/SteeringSync.cpp)
    target_compile_features(SteeringSync_${game}_1_61 PRIVATE cxx_std_17)
    target_compile_definitions(SteeringSync_${game}_1_61 PRIVATE UNICODE _UNICODE)
    target_link_libraries(SteeringSync_${game}_1_61 PRIVATE dinput8 dxguid user32)
    set_target_properties(SteeringSync_${game}_1_61 PROPERTIES
        PREFIX ""
        OUTPUT_NAME "SteeringSync_Universal_${game}_1_61"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/output"
    )
    if(MSVC)
        target_compile_options(SteeringSync_${game}_1_61 PRIVATE /W4 /EHsc)
    endif()
endforeach()

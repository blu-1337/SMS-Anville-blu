# Shared audio analysis (onsets, tempo, slice selection). Free of DPF and of any
# plug-in dependency; strict warnings apply to every consumer build.
add_library(sms_analysis STATIC
  OnsetAnalysis.cpp
  RealFft.cpp
  TempoEstimate.cpp)
target_include_directories(sms_analysis PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_compile_features(sms_analysis PUBLIC cxx_std_20)
set_target_properties(sms_analysis PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(sms_analysis PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
elseif(MSVC)
  target_compile_options(sms_analysis PRIVATE /W4 /permissive-)
endif()

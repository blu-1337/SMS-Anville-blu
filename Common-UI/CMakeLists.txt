add_library(sms_common_ui INTERFACE)

add_library(sms_file_io STATIC
  ../Common-Src/IO/FileIO.cpp
  ../Common-Src/IO/DefaultsFile.cpp)
target_include_directories(sms_file_io PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../Common-Src")
target_compile_features(sms_file_io PUBLIC cxx_std_20)
set_target_properties(sms_file_io PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(WIN32)
  target_compile_definitions(sms_file_io PRIVATE _WIN32_WINNT=0x0A00)
  target_link_libraries(sms_file_io PRIVATE shell32 ole32)
endif()
if(MSVC)
  target_compile_options(sms_common_ui INTERFACE /utf-8)
endif()
# Native file drop for editor windows; other platforms compile a null target.
if(WIN32)
  add_library(sms_file_drop STATIC FileDropTargetWin32.cpp)
  target_compile_definitions(sms_file_drop PRIVATE _WIN32_WINNT=0x0A00)
  target_link_libraries(sms_file_drop PRIVATE ole32 shell32 uuid)
else()
  add_library(sms_file_drop STATIC FileDropTargetNull.cpp)
endif()
target_include_directories(sms_file_drop PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(sms_file_drop PUBLIC cxx_std_20)
set_target_properties(sms_file_drop PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
  target_compile_options(sms_file_drop PRIVATE /W4 /permissive- /utf-8)
else()
  target_compile_options(sms_file_drop PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()

target_link_libraries(sms_common_ui INTERFACE sms_file_io sms_file_drop)
if(WIN32)
  target_link_libraries(sms_common_ui INTERFACE shell32 ole32)
endif()

target_include_directories(sms_common_ui INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/../Common-Src")

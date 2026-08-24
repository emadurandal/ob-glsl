cmake_minimum_required(VERSION 3.11)
project(ob-glsl-module)

set(CMAKE_CXX_STANDARD 17)

find_package(SDL2 REQUIRED)
find_library(SDL2_IMAGE_LIB SDL2_image)
find_library(GLBINDING_LIB glbinding)
find_path(EMACS_INCLUDE_DIR emacs-module.h
  HINTS
    "$ENV{EMACS_ROOT}/include"
    "C:/Emacs/include"
    "/usr/local/include/emacs"
    "/usr/include/emacs")

if(NOT SDL2_IMAGE_LIB)
  message(FATAL_ERROR "SDL2_image was not found")
endif()
if(NOT GLBINDING_LIB)
  message(FATAL_ERROR "glbinding was not found")
endif()
if(NOT EMACS_INCLUDE_DIR)
  message(FATAL_ERROR "Emacs 32 emacs-module.h was not found; set EMACS_INCLUDE_DIR")
endif()

file(GLOB SRCS "*.cpp")
add_library(${PROJECT_NAME} SHARED ${SRCS})
set_target_properties(${PROJECT_NAME} PROPERTIES PREFIX "")
target_include_directories(${PROJECT_NAME} PRIVATE ${EMACS_INCLUDE_DIR} "/usr/local/include")
target_link_libraries(${PROJECT_NAME} ${SDL2_LIBRARIES} ${SDL2_IMAGE_LIB} ${GLBINDING_LIB})

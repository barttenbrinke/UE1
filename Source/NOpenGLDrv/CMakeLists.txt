project(NOpenGLDrv C CXX)

if(NOT USE_SDL)
  message(FATAL_ERROR "NOpenGLDrv requires NSDLDrv to function.")
endif()

if(PSP)
  # glad.c is the runtime loader; the PSP uses statically linked pspgl instead.
  set(SRC_FILES "NOpenGLDrv.cpp" "glad_psp.cpp")
else()
  set(SRC_FILES "NOpenGLDrv.cpp" "${CMAKE_SOURCE_DIR}/../Thirdparty/glad/glad.c")
endif()

add_library(${PROJECT_NAME} ${LIB_TYPE} ${SRC_FILES})

target_include_directories(${PROJECT_NAME}
  PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/Inc
  PRIVATE
  ${CMAKE_SOURCE_DIR}/../Thirdparty/glad
  ${SDL2_INCLUDE_DIR}
)

target_link_libraries(${PROJECT_NAME} ${SDL2_LIBRARY})

target_link_libraries(${PROJECT_NAME} Render Engine Core)

target_compile_definitions(${PROJECT_NAME} PRIVATE NOPENGLDRV_EXPORTS UPACKAGE_NAME=${PROJECT_NAME})



if(PSP)
  # Verbose startup diagnostics (GL strings, window flags, heap headroom probe).
  # The probe allocates/frees up to 64MB, so keep it off unless investigating.
  option(PSP_DIAGNOSTICS "Verbose PSP startup diagnostics" OFF)
  if(PSP_DIAGNOSTICS)
    target_compile_definitions(${PROJECT_NAME} PRIVATE PSP_DIAGNOSTICS)
  endif()
endif()

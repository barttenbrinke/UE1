project(NOpenALDrv C CXX)

set(SRC_FILES "NOpenALDrv.cpp")
if(PSP)
  # Own mixer (Media Engine or a blocking CPU thread) behind a thin OpenAL
  # shim; the SDK's openal-soft 1.6 is not linked at all.
  list(APPEND SRC_FILES "PspMix.cpp" "PspAL.cpp")
endif()

add_library(${PROJECT_NAME} ${LIB_TYPE} ${SRC_FILES})

target_include_directories(${PROJECT_NAME}
  PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/Inc
  PRIVATE
  ${OPENAL_INCLUDE_DIR}
  ${LIBXMP_INCLUDE_DIR}
)

if(MSVC)
  # openal-soft release libs are built without safeseh for some reason?
  target_link_options(${PROJECT_NAME} PRIVATE "/SAFESEH:NO")
endif()

if(PSP)
  target_link_libraries(${PROJECT_NAME} ${LIBXMP_LIBRARY})
else()
  target_link_libraries(${PROJECT_NAME} ${OPENAL_LIBRARY} ${LIBXMP_LIBRARY})
endif()

target_link_libraries(${PROJECT_NAME} Engine Core)

target_compile_definitions(${PROJECT_NAME} PRIVATE NOPENALDRV_EXPORTS UPACKAGE_NAME=${PROJECT_NAME})

if(PSP)
  # PSP's OpenAL implements the EFX entry points but the SDK ships no efx.h /
  # efx-presets.h, so the reverb paths are compiled out. Plain positional audio
  # and music still work.
  target_compile_definitions(${PROJECT_NAME} PRIVATE PSP_NO_EFX)

  # Silence the music (sound effects still play). Handy when you are working
  # alongside the emulator:  cmake -DPSP_NO_MUSIC=ON ...
  option(PSP_NO_MUSIC "Disable music playback on PSP" OFF)
  if(PSP_NO_MUSIC)
    target_compile_definitions(${PROJECT_NAME} PRIVATE PSP_NO_MUSIC)
    message(STATUS "PSP: music disabled")
  endif()
endif()

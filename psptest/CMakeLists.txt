cmake_minimum_required(VERSION 3.15)
project(psptest C)
add_executable(psptest main.c)
target_link_libraries(psptest SDL2main SDL2 GLU GL
  pspvram pspaudio pspvfpu pspdisplay pspgu pspge psphprm pspctrl psppower m)
create_pbp_file(TARGET psptest TITLE "GL Clear Test")

if(NOT MSVC)
  set(CMAKE_CXX_FLAGS "--std=c++11 -Wall -Wno-unknown-pragmas -Wno-vla -fexceptions ${CMAKE_CXX_FLAGS}")
  set(CMAKE_C_FLAGS "-fexceptions ${CMAKE_C_FLAGS}")

  if (CMAKE_VERSION VERSION_LESS "3.1")
    set(CMAKE_C_FLAGS "-std=c11 ${CMAKE_C_FLAGS}")
  else ()
    set(CMAKE_C_STANDARD 11)
  endif ()
else()
  # disable some verbose warnings
  set(CMAKE_CXX_FLAGS "/wd4267 /wd4251 /wd4522 /wd4522 /wd4838 /wd4305 /wd4244 /wd4190 /wd4101 /wd4996 /wd4275 ${CMAKE_CXX_FLAGS}")
  # windef.h will define max/min macros if NOMINMAX is not defined
  add_definitions(/DNOMINMAX)
  # we want to respect the standard, and we are bored of those **** .
  add_definitions(-D_CRT_SECURE_NO_DEPRECATE=1)
  # Define this so we declare these as dllexport declarations (this is
  # inappropriate if you're trying to link against TH, NB!)
  add_definitions(-DTH_EXPORTS)
endif()

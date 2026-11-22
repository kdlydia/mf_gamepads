set(MF_MIN_VERSION "1.0.0")
set(MF_NEEDS_LILA OFF)

file(GLOB_RECURSE _mf_gamepads_sources CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/community/mf_gamepads/src/*.cpp"
)

add_library(mf_gamepads OBJECT
    ${_mf_gamepads_sources}
)

target_include_directories(mf_gamepads PUBLIC
    "${CMAKE_SOURCE_DIR}/community/mf_gamepads/src"
)

set_target_properties(mf_gamepads PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
)

target_link_libraries(mf_gamepads PUBLIC MayaFlux::MayaFluxLib)

if(MF_NEEDS_LILA)
    target_link_libraries(mf_gamepads PUBLIC MayaFlux::MayaFluxHost)
endif()

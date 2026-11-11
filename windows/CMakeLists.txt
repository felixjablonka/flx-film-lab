cmake_minimum_required(VERSION 3.21)
# the Mac release this port matches
project(flx_film_lab_win VERSION 1.5.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The same Qt on every machine: QRhi's API may change between Qt versions.
# QRhi is in Qt's private Gui module, which is what the warning would say.
set(QT_NO_PRIVATE_MODULE_WARNING ON)
find_package(Qt6 6.12.0 EXACT REQUIRED COMPONENTS Gui GuiPrivate ShaderTools Widgets Network Multimedia)
qt_standard_project_setup()

set(ENGINE_SOURCES
    engine/Dump.cpp
    engine/Setup.cpp
    engine/Filters.cpp
    engine/GpuGraph.cpp
    engine/FilmRenderer.cpp
    engine/Packs.cpp
    engine/Recipe.cpp
    engine/PrintCurves.cpp
    engine/FilmMath.cpp
    engine/ToneCurve.cpp
    engine/SetupBuilder.cpp
    engine/FrameGeometry.cpp
    engine/MaskRasterizer.cpp
    engine/LogFormats.cpp
    engine/LookLUT.cpp
)

# The kernels, GLSL 440 compute, baked by qsb into SPIR-V (Vulkan), HLSL 5.0
# (Direct3D 12 and 11) and MSL (Metal). No GLSL for OpenGL: the port runs on
# neither. On Windows the HLSL is compiled when the program is built (fxc), so
# no kernel is compiled while it runs.
set(FLX_PRECOMPILE "")
if(WIN32)
    set(FLX_PRECOMPILE PRECOMPILE)
endif()
set(ENGINE_KERNELS
    engine/shaders/film_expose.comp
    engine/shaders/scatter_mix.comp
    engine/shaders/accumulate_scaled.comp
    engine/shaders/halation_add.comp
    engine/shaders/film_log.comp
    engine/shaders/film_develop.comp
    engine/shaders/couplers_amount.comp
    engine/shaders/film_develop_couplers.comp
    engine/shaders/film_grain_layer.comp
    engine/shaders/grain_convolve.comp
    engine/shaders/grain_convolve_add.comp
    engine/shaders/print_and_scan.comp
    engine/shaders/grain_mirror.comp
    engine/shaders/grain_even_mean.comp
    engine/shaders/grain_restore_mean.comp
    engine/shaders/encode_display.comp
    engine/shaders/apply_curves.comp
    engine/shaders/histogram_rgb.comp
    engine/shaders/apply_geometry.comp
    engine/shaders/effects_blur.comp
    engine/shaders/effects_bloom_select.comp
    engine/shaders/effects_bloom_composite.comp
    engine/shaders/effects_mix.comp
    engine/shaders/effects_glow_select.comp
    engine/shaders/effects_halation_select.comp
    engine/shaders/effects_light_combine.comp
    engine/shaders/motion_grade.comp
    engine/shaders/motion_screen.comp
    engine/shaders/motion_luma.comp
    engine/shaders/motion_chroma.comp
)

# LibRaw, compiled in (cmake/LibRaw.cmake): the fallback decoder.
include(cmake/LibRaw.cmake)
add_library(flx_libraw STATIC ${LIBRAW_SOURCES})
target_include_directories(flx_libraw PUBLIC third_party/LibRaw)
target_compile_definitions(flx_libraw PUBLIC LIBRAW_NODLL PRIVATE _CRT_SECURE_NO_WARNINGS)
set_target_properties(flx_libraw PROPERTIES CXX_STANDARD 17)
if(MSVC)
    target_compile_options(flx_libraw PRIVATE /w)
else()
    target_compile_options(flx_libraw PRIVATE -w)
endif()

# RawSpeed, compiled in (cmake/RawSpeed.cmake): the standard decoder.
include(cmake/RawSpeed.cmake)

# The CPU side rounds as the Mac's Swift does, which fuses no multiply and add
# into one instruction; so the Mac's build and the PC's compute alike.
function(flx_rounding name)
    if(MSVC AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(${name} PRIVATE /clang:-ffp-contract=off)
    elseif(MSVC)
        target_compile_options(${name} PRIVATE /fp:precise)
    else()
        target_compile_options(${name} PRIVATE -Wall -Wextra -ffp-contract=off)
    endif()
endfunction()

function(flx_tool name main)
    qt_add_executable(${name} ${main} ${ENGINE_SOURCES})
    set_target_properties(${name} PROPERTIES MACOSX_BUNDLE OFF WIN32_EXECUTABLE OFF)
    target_include_directories(${name} PRIVATE engine)
    target_link_libraries(${name} PRIVATE Qt6::Gui Qt6::GuiPrivate)
    flx_rounding(${name})
    qt_add_shaders(${name} "${name}_kernels"
        NOGLSL
        HLSL 50
        ${FLX_PRECOMPILE}
        MSL 12
        BASE engine/shaders
        PREFIX "/shaders"
        FILES ${ENGINE_KERNELS}
    )
endfunction()

flx_tool(flxport-render tools/port-render/main.cpp)
flx_tool(flxport-setup tools/port-setup/main.cpp)
flx_tool(flxport-lut tools/port-lut/main.cpp)
flx_tool(flxport-checks tools/port-checks/main.cpp)
# the undo steps' names are the app's (EditNames)
target_sources(flxport-checks PRIVATE app/EditNames.cpp app/Vocabulary.cpp app/Presets.cpp)
target_include_directories(flxport-checks PRIVATE app)

# The port's RAW decoder, against Apple's (tools/mac-reference/flxdecode).
# On the CPU, and on the GPU with these kernels (GpuDecoder), as the film's.
set(RAW_KERNELS
    engine/shaders/raw_demosaic_mhc.comp
    engine/shaders/raw_demosaic_green.comp
    engine/shaders/raw_demosaic_colours.comp
    engine/shaders/raw_highlight_blocks.comp
    engine/shaders/raw_colour.comp
    engine/shaders/raw_noise_samples.comp
    engine/shaders/raw_repair.comp
    engine/shaders/raw_false_colour.comp
    engine/shaders/raw_cnr_guide.comp
    engine/shaders/raw_cnr_half.comp
    engine/shaders/raw_box.comp
    engine/shaders/raw_cnr_ab.comp
    engine/shaders/raw_cnr_apply.comp
    engine/shaders/raw_sharp_luma.comp
    engine/shaders/raw_blur.comp
    engine/shaders/raw_sharp_energy.comp
    engine/shaders/raw_sharp_apply.comp
    engine/shaders/raw_lens.comp
    engine/shaders/raw_turn.comp
    engine/shaders/raw_lanczos.comp
)
qt_add_executable(flxport-decode tools/port-decode/main.cpp engine/RawUnpackRawSpeed.cpp engine/RawUnpackLibRaw.cpp
    engine/RawDecoder.cpp engine/RawDetail.cpp engine/GpuDecoder.cpp engine/AppleColour.cpp engine/LensCorrection.cpp
    engine/Dump.cpp)
set_target_properties(flxport-decode PROPERTIES MACOSX_BUNDLE OFF WIN32_EXECUTABLE OFF)
target_include_directories(flxport-decode PRIVATE engine)
target_link_libraries(flxport-decode PRIVATE Qt6::Gui Qt6::GuiPrivate flx_rawspeed flx_libraw)
flx_rounding(flxport-decode)
qt_add_shaders(flxport-decode "flxport-decode_kernels"
    NOGLSL
    HLSL 50
    ${FLX_PRECOMPILE}
    MSL 12
    BASE engine/shaders
    PREFIX "/shaders"
    FILES ${RAW_KERNELS}
)
# Apple's camera profiles (tools/calibrate), next to the programs; the decode
# falls back to the cameras' daylight matrices while there is none
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/engine/data/apple-colour.json)
    configure_file(engine/data/apple-colour.json ${CMAKE_CURRENT_BINARY_DIR}/apple-colour.json COPYONLY)
endif()
# Lensfun's lens database (third_party/lensfun, CC BY-SA 3.0), its XML alone,
# next to the programs: the lens correction's, where a camera has no data of
# its own. The port reads it itself (LensCorrection); Lensfun's library is
# not used.
file(GLOB LENSFUN_DB CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lensfun/data/db/*.xml)
foreach(xml ${LENSFUN_DB})
    get_filename_component(name ${xml} NAME)
    configure_file(${xml} ${CMAKE_CURRENT_BINARY_DIR}/lensfun/${name} COPYONLY)
endforeach()

# FFmpeg, for Motion's clips: Homebrew's on a Mac; on Windows the app's own
# LGPL build (tools/ffmpeg/build.sh, into build/ffmpeg, or FLX_FFMPEG), its
# libraries copied next to the programs.
if(WIN32)
    set(FLX_FFMPEG "${CMAKE_CURRENT_SOURCE_DIR}/build/ffmpeg" CACHE PATH "FFmpeg, as tools/ffmpeg/build.sh builds it")
    set(FFMPEG_DIR "${FLX_FFMPEG}")
    if(NOT EXISTS "${FFMPEG_DIR}/lib/avcodec.lib")
        message(FATAL_ERROR "no FFmpeg at ${FFMPEG_DIR}: build it with tools/ffmpeg/build.sh")
    endif()
    add_library(flx_ffmpeg INTERFACE)
    target_include_directories(flx_ffmpeg INTERFACE "${FFMPEG_DIR}/include")
    target_link_libraries(flx_ffmpeg INTERFACE "${FFMPEG_DIR}/lib/avformat.lib" "${FFMPEG_DIR}/lib/avcodec.lib"
                                               "${FFMPEG_DIR}/lib/avutil.lib" "${FFMPEG_DIR}/lib/swscale.lib"
                                               "${FFMPEG_DIR}/lib/swresample.lib")
    file(GLOB FFMPEG_DLLS "${FFMPEG_DIR}/bin/*.dll")
    foreach(dll ${FFMPEG_DLLS})
        get_filename_component(name ${dll} NAME)
        configure_file(${dll} ${CMAKE_CURRENT_BINARY_DIR}/${name} COPYONLY)
    endforeach()
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil libswscale libswresample)
    add_library(flx_ffmpeg INTERFACE)
    target_link_libraries(flx_ffmpeg INTERFACE PkgConfig::FFMPEG)
endif()

# Motion's sources, which read and write movies with FFmpeg
set(MOTION_SOURCES engine/Clip.cpp engine/Motion.cpp engine/ClipExport.cpp engine/ClipAudio.cpp)
add_executable(flxport-clip tools/port-clip/main.cpp engine/Clip.cpp engine/LogFormats.cpp)
target_include_directories(flxport-clip PRIVATE engine)
target_link_libraries(flxport-clip PRIVATE flx_ffmpeg)
flx_tool(flxport-motion tools/port-motion/main.cpp)
target_sources(flxport-motion PRIVATE ${MOTION_SOURCES})
target_link_libraries(flxport-motion PRIVATE flx_ffmpeg)
flx_tool(flxport-export tools/port-export/main.cpp)
target_sources(flxport-export PRIVATE ${MOTION_SOURCES})
target_link_libraries(flxport-export PRIVATE flx_ffmpeg)

# The app: FLX Film Lab for Windows, both halves of the Mac app, Stills and Motion.
set(APP_SOURCES
    app/main.cpp
    app/AppSettings.cpp
    app/Theme.cpp
    app/Vocabulary.cpp
    app/Widgets.cpp
    app/CurvesView.cpp
    app/Inspector.cpp
    app/Canvas.cpp
    app/MainWindow.cpp
    app/Document.cpp
    app/EditNames.cpp
    app/Presets.cpp
    app/RenderEngine.cpp
    app/Exporter.cpp
    app/Exif.cpp
    app/MotionEngine.cpp
    app/MotionDocument.cpp
    app/MotionCanvas.cpp
    app/TransportBar.cpp
    app/MotionInspector.cpp
    app/AudioPlayback.cpp
)
qt_add_executable(FLXFilmLab ${APP_SOURCES} ${ENGINE_SOURCES} ${MOTION_SOURCES}
    engine/RawUnpackRawSpeed.cpp engine/RawUnpackLibRaw.cpp engine/RawDecoder.cpp engine/RawDetail.cpp
    engine/GpuDecoder.cpp engine/AppleColour.cpp engine/LensCorrection.cpp)
set_target_properties(FLXFilmLab PROPERTIES MACOSX_BUNDLE OFF WIN32_EXECUTABLE ON)
# the version, for the program, Explorer and the installer (tools/pc/package.cmd)
configure_file(app/version.h.in ${CMAKE_CURRENT_BINARY_DIR}/version.h)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/version.txt "${PROJECT_VERSION}")
target_include_directories(FLXFilmLab PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
# the icon and the version Explorer shows
if(WIN32)
    target_sources(FLXFilmLab PRIVATE app/FLXFilmLab.rc)
    # the GPU's memory budget (RenderEngine's gpuMemoryFree)
    target_link_libraries(FLXFilmLab PRIVATE dxgi)
endif()
target_include_directories(FLXFilmLab PRIVATE engine app)
target_link_libraries(FLXFilmLab PRIVATE Qt6::Widgets Qt6::Gui Qt6::GuiPrivate Qt6::Network Qt6::Multimedia
                                         flx_rawspeed flx_libraw flx_ffmpeg)
flx_rounding(FLXFilmLab)
qt_add_shaders(FLXFilmLab "FLXFilmLab_kernels"
    NOGLSL
    HLSL 50
    ${FLX_PRECOMPILE}
    MSL 12
    BASE engine/shaders
    PREFIX "/shaders"
    FILES ${ENGINE_KERNELS} ${RAW_KERNELS}
)
# The Mac app's tree, the release the port follows: mac/, the submodule, in
# the port's own repository; the repository's root, beside windows/, in the
# public one (as tools/mac-reference/mac-tree.sh finds it).
set(FLX_MAC ${CMAKE_CURRENT_SOURCE_DIR}/mac)
if(NOT EXISTS ${FLX_MAC}/build.sh)
    get_filename_component(FLX_MAC ${CMAKE_CURRENT_SOURCE_DIR}/.. ABSOLUTE)
endif()
# The film packs next to the program: the Mac app's, or on the PC the copy
# tools/pc/remote.sh keeps beside the sources.
set(FLX_PACKS ${FLX_MAC}/FLXFilmLab/Resources/packs)
if(NOT EXISTS ${FLX_PACKS}/index.json)
    set(FLX_PACKS ${CMAKE_CURRENT_SOURCE_DIR}/packs)
endif()
file(GLOB PACK_FILES CONFIGURE_DEPENDS ${FLX_PACKS}/*.json ${FLX_PACKS}/*.tclut ${FLX_PACKS}/*.txt)
foreach(f ${PACK_FILES})
    get_filename_component(name ${f} NAME)
    configure_file(${f} ${CMAKE_CURRENT_BINARY_DIR}/packs/${name} COPYONLY)
endforeach()
# On Windows, Qt's libraries and plugins beside the program, so it runs
# from Explorer as well as from a shell with Qt on its PATH.
if(WIN32)
    find_program(WINDEPLOYQT windeployqt HINTS "${QT6_INSTALL_PREFIX}/bin" "${Qt6_DIR}/../../../bin")
    if(WINDEPLOYQT)
        add_custom_command(TARGET FLXFilmLab POST_BUILD
            COMMAND "${WINDEPLOYQT}" --release --no-translations --no-system-d3d-compiler --no-opengl-sw
                    --no-compiler-runtime --skip-plugin-types multimedia --no-ffmpeg "$<TARGET_FILE:FLXFilmLab>" > "${CMAKE_CURRENT_BINARY_DIR}/windeployqt.log"
            VERBATIM)
    endif()
endif()

# The app as it is handed over (tools\pc\package.cmd): the program, Qt's
# libraries and plugins, the compiler's runtime, the data, and the licences.
if(WIN32)
    install(TARGETS FLXFilmLab RUNTIME DESTINATION .)
    install(DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/packs ${CMAKE_CURRENT_BINARY_DIR}/lensfun DESTINATION .)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/cameras.xml DESTINATION .)
    if(EXISTS ${CMAKE_CURRENT_BINARY_DIR}/apple-colour.json)
        install(FILES ${CMAKE_CURRENT_BINARY_DIR}/apple-colour.json DESTINATION .)
    endif()
    # The licences of what the app is made of: its own and its film data's,
    # the libraries it carries, Lensfun's data, Qt's and FFmpeg's.
    function(flx_licence source name)
        if(NOT EXISTS "${source}")
            message(FATAL_ERROR "no licence at ${source}")
        endif()
        install(FILES "${source}" DESTINATION licenses RENAME "${name}")
    endfunction()
    flx_licence(${FLX_MAC}/LICENSE "FLX Film Lab - GPL-3.0.txt")
    flx_licence(${FLX_MAC}/NOTICE.md "FLX Film Lab - notices.md")
    flx_licence(${FLX_PACKS}/LICENSE-CC-BY-SA-4.0.txt "Film packs - CC BY-SA 4.0.txt")
    flx_licence(${CMAKE_CURRENT_SOURCE_DIR}/third_party/LibRaw/LICENSE.LGPL "LibRaw - LGPL-2.1.txt")
    flx_licence(${CMAKE_CURRENT_SOURCE_DIR}/third_party/LibRaw/LICENSE.CDDL "LibRaw - CDDL-1.0.txt")
    flx_licence(${CMAKE_CURRENT_SOURCE_DIR}/third_party/LibRaw/COPYRIGHT "LibRaw - copyright.txt")
    flx_licence(${CMAKE_CURRENT_SOURCE_DIR}/third_party/rawspeed/LICENSE "RawSpeed - LGPL-2.1.txt")
    flx_licence(${CMAKE_CURRENT_SOURCE_DIR}/third_party/pugixml/LICENSE.md "pugixml - MIT.md")
    flx_licence(${CMAKE_CURRENT_SOURCE_DIR}/third_party/lensfun/docs/cc-by-sa-3.0.txt "Lensfun database - CC BY-SA 3.0.txt")
    flx_licence(${CMAKE_CURRENT_SOURCE_DIR}/third_party/lensfun/docs/lgpl-3.0.txt "Qt 6 - LGPL-3.0.txt")
    flx_licence(${FFMPEG_DIR}/LICENSE.txt "FFmpeg - LGPL-2.1.txt")
    flx_licence(${FFMPEG_DIR}/SOURCE.txt "FFmpeg - source.txt")
    flx_licence(${FFMPEG_DIR}/NOTICES.txt "FFmpeg - notices.txt")
    # FFmpeg's libraries Motion reads and writes movies with
    file(GLOB FFMPEG_RUNTIME "${FFMPEG_DIR}/bin/avcodec-*.dll" "${FFMPEG_DIR}/bin/avformat-*.dll"
         "${FFMPEG_DIR}/bin/avutil-*.dll" "${FFMPEG_DIR}/bin/swscale-*.dll" "${FFMPEG_DIR}/bin/swresample-*.dll")
    install(FILES ${FFMPEG_RUNTIME} DESTINATION .)
    if(WINDEPLOYQT)
        install(CODE "execute_process(COMMAND \"${WINDEPLOYQT}\" --release --no-translations --no-system-d3d-compiler
                      --no-opengl-sw --no-compiler-runtime --skip-plugin-types multimedia --no-ffmpeg \"\${CMAKE_INSTALL_PREFIX}/FLXFilmLab.exe\" OUTPUT_QUIET)")
    endif()
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION .)
    include(InstallRequiredSystemLibraries)
endif()

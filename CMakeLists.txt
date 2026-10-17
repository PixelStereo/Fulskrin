cmake_minimum_required(VERSION 3.21)
project(Pinol VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
if(MSVC)
  add_compile_options(/utf-8 /W3)   # UTF-8 sources (UI strings contain non-ASCII characters)
  add_compile_definitions(NOMINMAX _USE_MATH_DEFINES)
endif()
# Warnings on Pinol's own code (the libraries fetched below keep theirs)
set(PINOL_WARNINGS $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wno-unused-parameter>)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Qt6 6.2 REQUIRED COMPONENTS Core Gui Widgets OpenGL OpenGLWidgets Network)

# ---------------------------------------------------------------------------
# FFmpeg: pkg-config (macOS Homebrew, Linux) or FFMPEG_ROOT (Windows, "shared" build)
# ---------------------------------------------------------------------------
set(FFMPEG_ROOT "" CACHE PATH "FFmpeg folder containing include/ and lib/ (Windows)")
set(FFMPEG_LIBS avformat avcodec swscale swresample avutil)

if(FFMPEG_ROOT)
  add_library(ffmpeg INTERFACE)
  target_include_directories(ffmpeg INTERFACE "${FFMPEG_ROOT}/include")
  foreach(lib ${FFMPEG_LIBS})
    find_library(FF_${lib} NAMES ${lib} lib${lib} PATHS "${FFMPEG_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)
    target_link_libraries(ffmpeg INTERFACE ${FF_${lib}})
  endforeach()
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libswscale libswresample libavutil)
  add_library(ffmpeg INTERFACE)
  target_link_libraries(ffmpeg INTERFACE PkgConfig::FFMPEG)
endif()

# ---------------------------------------------------------------------------
# Engine: no dependency on widgets (reusable with another UI)
# ---------------------------------------------------------------------------
add_library(pinol_engine STATIC
  src/engine/Gl.cpp
  src/engine/Isf.cpp
  src/engine/IsfLibrary.cpp
  src/engine/Mapping.cpp
  src/engine/VideoDecoder.cpp
  src/engine/VideoDecoderHap.cpp
  src/engine/VideoDecoderDelivery.cpp
  src/engine/VideoDecoderLayout.cpp
  src/engine/VideoDecoderDemux.cpp
  src/engine/VideoDecoderSeek.cpp
  src/engine/VideoDecoderMetrics.cpp
  src/engine/ProcessMemory.cpp
  src/engine/VideoTexture.cpp
  src/engine/Hap.cpp
  src/engine/Snappy.cpp
  src/engine/WorkerPool.cpp
  src/engine/Engine.cpp
  src/engine/Layers.cpp
  src/engine/Render.cpp
  src/engine/Project.cpp
  src/engine/ProjectCollect.cpp
  src/engine/Media.cpp
  src/engine/Publish.cpp
  src/engine/DeckLink.cpp
  src/engine/Midi.cpp
  src/engine/LiveInput.cpp
  src/engine/Permissions.cpp
  src/engine/AudioStream.cpp
  src/engine/AudioOutput.cpp
  src/engine/MiniaudioImpl.cpp
  src/engine/Osc.cpp
  src/engine/EventLog.cpp
  src/engine/FrameTiming.cpp
  src/engine/OscCodec.cpp
  src/engine/SequenceFeedback.cpp
  src/engine/Zeroconf.cpp
  src/engine/Snapshots.cpp
  src/engine/Sequencer.cpp
  src/engine/Publishing.cpp
  src/engine/TestCard.cpp
  src/engine/Masks.cpp
  src/engine/Timecode.cpp
  src/engine/TimecodeClock.cpp
  src/engine/TimecodeIo.cpp
  src/engine/TimecodeOutputs.cpp
  src/engine/Interactions.cpp
  src/engine/InteractionsEngine.cpp
  src/engine/Presets.cpp
  src/engine/Animations.cpp
  src/engine/Parameter.cpp
  src/engine/LayerParams.cpp
)
target_include_directories(pinol_engine PUBLIC src/engine)
target_link_libraries(pinol_engine PUBLIC Qt6::Core Qt6::Gui Qt6::OpenGL Qt6::Network ffmpeg)
target_compile_definitions(pinol_engine PUBLIC GL_SILENCE_DEPRECATION)
if(WIN32)
  target_link_libraries(pinol_engine PRIVATE psapi) # process memory (ProcessMemory.cpp)
endif()
# Cameras (live inputs): Qt Multimedia when it is there
find_package(Qt6 QUIET COMPONENTS Multimedia)
if(Qt6Multimedia_FOUND)
  target_link_libraries(pinol_engine PRIVATE Qt6::Multimedia)
  target_compile_definitions(pinol_engine PRIVATE PINOL_HAS_CAMERA)
else()
  message(STATUS "Qt Multimedia not found: no camera inputs")
endif()

# ---------------------------------------------------------------------------
# Third-party sources: git submodules in third_party/ (git submodule update --init), built here
# ---------------------------------------------------------------------------
function(pinol_submodule name file)
  if(NOT EXISTS "${CMAKE_SOURCE_DIR}/third_party/${name}/${file}")
    message(FATAL_ERROR "third_party/${name} is empty: run  git submodule update --init")
  endif()
endfunction()

# ---------------------------------------------------------------------------
# Audio output: miniaudio (single header, public domain / MIT-0), compiled in src/engine/MiniaudioImpl.cpp
# ---------------------------------------------------------------------------
pinol_submodule(miniaudio miniaudio.h)
target_include_directories(pinol_engine PRIVATE "${CMAKE_SOURCE_DIR}/third_party/miniaudio")
target_compile_definitions(pinol_engine PRIVATE MA_NO_DECODING MA_NO_ENCODING MA_NO_GENERATION MA_NO_ENGINE
  MA_NO_RESOURCE_MANAGER MA_NO_NODE_GRAPH)
if(MSVC)
  set_source_files_properties(src/engine/MiniaudioImpl.cpp PROPERTIES COMPILE_OPTIONS "/W0")
else()
  set_source_files_properties(src/engine/MiniaudioImpl.cpp PROPERTIES COMPILE_OPTIONS "-w")
endif()
if(APPLE)
  # Link the Core Audio frameworks directly rather than loading them at runtime
  target_compile_definitions(pinol_engine PRIVATE MA_NO_RUNTIME_LINKING)
  target_link_libraries(pinol_engine PRIVATE "-framework CoreFoundation" "-framework CoreAudio"
    "-framework AudioToolbox" "-framework OpenGL")
elseif(UNIX)
  find_package(Threads REQUIRED)
  target_link_libraries(pinol_engine PRIVATE Threads::Threads ${CMAKE_DL_LIBS} m)
endif()

# ---------------------------------------------------------------------------
# MIDI (the timecode's MTC): RtMidi (MIT), a submodule. CoreMIDI, Windows MM, ALSA (Linux: libasound2-dev; without it,
# no MIDI port)
# ---------------------------------------------------------------------------
pinol_submodule(rtmidi RtMidi.cpp)
set(rtmidi_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/rtmidi")
set(PINOL_MIDI_API "")
if(APPLE)
  set(PINOL_MIDI_API __MACOSX_CORE__)
  set(PINOL_MIDI_LIBS "-framework CoreMIDI" "-framework CoreAudio" "-framework CoreFoundation")
elseif(WIN32)
  set(PINOL_MIDI_API __WINDOWS_MM__)
  set(PINOL_MIDI_LIBS winmm)
else()
  find_package(ALSA QUIET)
  if(ALSA_FOUND)
    set(PINOL_MIDI_API __LINUX_ALSA__)
    set(PINOL_MIDI_LIBS ALSA::ALSA Threads::Threads)
  else()
    message(STATUS "ALSA not found: no MIDI ports (sudo apt install libasound2-dev)")
  endif()
endif()
if(PINOL_MIDI_API)
  add_library(rtmidi_static STATIC "${rtmidi_SOURCE_DIR}/RtMidi.cpp")
  target_include_directories(rtmidi_static PUBLIC "${rtmidi_SOURCE_DIR}")
  target_compile_definitions(rtmidi_static PUBLIC ${PINOL_MIDI_API})
  target_link_libraries(rtmidi_static PUBLIC ${PINOL_MIDI_LIBS})
  if(MSVC)
    target_compile_options(rtmidi_static PRIVATE /W0)
  else()
    target_compile_options(rtmidi_static PRIVATE -w)
  endif()
  target_link_libraries(pinol_engine PRIVATE rtmidi_static)
  target_compile_definitions(pinol_engine PRIVATE PINOL_HAS_MIDI)
endif()

# ---------------------------------------------------------------------------
# HAP: texture blocks decoded on the CPU when the GPU cannot sample them (BC7 and BC6 on macOS).
# bcdec: single header, MIT or Unlicense, compiled in src/engine/Hap.cpp.
# ---------------------------------------------------------------------------
pinol_submodule(bcdec bcdec.h)
target_include_directories(pinol_engine PRIVATE "${CMAKE_SOURCE_DIR}/third_party/bcdec")

# ---------------------------------------------------------------------------
# Output publishing
#  - NDI and OMT: loaded at runtime (no build-time dependency)
#  - Syphon (macOS) and Spout (Windows): sources downloaded and built with Pinol
# ---------------------------------------------------------------------------
option(PINOL_SYPHON "Syphon publishing (macOS)" ON)
option(PINOL_SPOUT "Spout publishing (Windows)" ON)

# macOS: the output windows shown through Metal (OpenGL into a window only works on the main thread there)
if(APPLE)
  enable_language(OBJCXX)
  target_sources(pinol_engine PRIVATE src/engine/PresentMetal.mm src/engine/Permissions.mm src/engine/VideoSurfaceMac.cpp)
  set_source_files_properties(src/engine/PresentMetal.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
  target_compile_definitions(pinol_engine PRIVATE GL_SILENCE_DEPRECATION)
  target_link_libraries(pinol_engine PRIVATE "-framework Metal" "-framework QuartzCore" "-framework IOSurface"
    "-framework AppKit" "-framework Network" "-framework CoreVideo")
else()
  target_sources(pinol_engine PRIVATE src/engine/VideoSurfaceNone.cpp)
endif()

if(APPLE AND PINOL_SYPHON)
  enable_language(C OBJC OBJCXX) # Syphon: C and Objective-C sources
  pinol_submodule(syphon SyphonServer.h)
  set(syphon_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/syphon")
  # The sources import <Syphon/...>: expose the headers under this prefix.
  file(GLOB SYPHON_HEADERS "${syphon_SOURCE_DIR}/*.h")
  file(COPY ${SYPHON_HEADERS} DESTINATION "${CMAKE_BINARY_DIR}/syphon-include/Syphon")
  file(GLOB SYPHON_SOURCES "${syphon_SOURCE_DIR}/*.m" "${syphon_SOURCE_DIR}/*.c")
  # OpenGL version only: the Metal server would require compiling .metal shaders
  list(FILTER SYPHON_SOURCES EXCLUDE REGEX "Metal")
  add_library(syphon STATIC ${SYPHON_SOURCES})
  target_include_directories(syphon PUBLIC "${CMAKE_BINARY_DIR}/syphon-include" PRIVATE "${syphon_SOURCE_DIR}")
  target_compile_definitions(syphon PRIVATE SYPHON_CORE_SHARE GL_SILENCE_DEPRECATION)
  target_compile_options(syphon PRIVATE -w "SHELL:-include ${syphon_SOURCE_DIR}/Syphon_Prefix.pch"
    $<$<COMPILE_LANGUAGE:OBJC>:-fobjc-arc>)
  target_link_libraries(syphon PUBLIC "-framework Cocoa" "-framework OpenGL" "-framework IOSurface" "-framework Foundation")
  target_sources(pinol_engine PRIVATE src/engine/PublishSyphon.mm src/engine/LiveInputSyphon.mm)
  set_source_files_properties(src/engine/PublishSyphon.mm src/engine/LiveInputSyphon.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
  target_link_libraries(pinol_engine PRIVATE syphon)
  target_compile_definitions(pinol_engine PRIVATE PINOL_HAS_SYPHON)
endif()

# DeckLink (Blackmagic SDI / HDMI cards): the SDK's headers are in third_party/decklink (Boost license), the driver
# (Desktop Video) is loaded at run time. Windows: its interfaces come from the SDK's IDL, compiled by MIDL.
option(PINOL_DECKLINK "DeckLink outputs (Blackmagic Desktop Video)" ON)
if(PINOL_DECKLINK)
  set(DECKLINK_SDK "${CMAKE_SOURCE_DIR}/third_party/decklink")
  if(APPLE)
    target_include_directories(pinol_engine PRIVATE "${DECKLINK_SDK}/mac")
    target_sources(pinol_engine PRIVATE "${DECKLINK_SDK}/mac/DeckLinkAPIDispatch.cpp")
    set_source_files_properties("${DECKLINK_SDK}/mac/DeckLinkAPIDispatch.cpp" PROPERTIES COMPILE_OPTIONS "-w")
    target_link_libraries(pinol_engine PRIVATE "-framework CoreFoundation")
    target_compile_definitions(pinol_engine PRIVATE PINOL_HAS_DECKLINK)
  elseif(WIN32)
    find_program(MIDL_EXE midl)
    if(MIDL_EXE)
      set(DECKLINK_GEN "${CMAKE_BINARY_DIR}/decklink")
      file(MAKE_DIRECTORY "${DECKLINK_GEN}")
      file(GLOB DECKLINK_IDL "${DECKLINK_SDK}/win/*.idl")
      add_custom_command(
        OUTPUT "${DECKLINK_GEN}/DeckLinkAPI_h.h" "${DECKLINK_GEN}/DeckLinkAPI_i.c"
        COMMAND "${MIDL_EXE}" /nologo /env x64 /I "${DECKLINK_SDK}/win" /out "${DECKLINK_GEN}"
                /h DeckLinkAPI_h.h /iid DeckLinkAPI_i.c /tlb DeckLinkAPI.tlb /proxy DeckLinkAPI_p.c /dlldata dlldata.c
                "${DECKLINK_SDK}/win/DeckLinkAPI.idl"
        DEPENDS ${DECKLINK_IDL}
        COMMENT "DeckLink SDK interfaces (MIDL)")
      add_custom_target(decklink_idl DEPENDS "${DECKLINK_GEN}/DeckLinkAPI_h.h" "${DECKLINK_GEN}/DeckLinkAPI_i.c")
      add_dependencies(pinol_engine decklink_idl)
      target_include_directories(pinol_engine PRIVATE "${DECKLINK_GEN}")
      target_link_libraries(pinol_engine PRIVATE ole32 oleaut32)
      target_compile_definitions(pinol_engine PRIVATE PINOL_HAS_DECKLINK)
    else()
      message(WARNING "MIDL not found (run from a Visual Studio developer prompt): built without DeckLink outputs")
    endif()
  else()
    target_include_directories(pinol_engine PRIVATE "${DECKLINK_SDK}/linux")
    target_sources(pinol_engine PRIVATE "${DECKLINK_SDK}/linux/DeckLinkAPIDispatch.cpp")
    set_source_files_properties("${DECKLINK_SDK}/linux/DeckLinkAPIDispatch.cpp" PROPERTIES COMPILE_OPTIONS "-w")
    target_link_libraries(pinol_engine PRIVATE ${CMAKE_DL_LIBS} Threads::Threads)
    target_compile_definitions(pinol_engine PRIVATE PINOL_HAS_DECKLINK)
  endif()
endif()

if(WIN32 AND PINOL_SPOUT)
  pinol_submodule(spout2 SPOUTSDK/SpoutGL/Spout.cpp)
  set(SPOUT_DIR "${CMAKE_SOURCE_DIR}/third_party/spout2/SPOUTSDK/SpoutGL")
  add_library(spout_gl STATIC
    ${SPOUT_DIR}/Spout.cpp ${SPOUT_DIR}/SpoutCopy.cpp ${SPOUT_DIR}/SpoutDirectX.cpp
    ${SPOUT_DIR}/SpoutFrameCount.cpp ${SPOUT_DIR}/SpoutGL.cpp ${SPOUT_DIR}/SpoutGLextensions.cpp
    ${SPOUT_DIR}/SpoutReceiver.cpp ${SPOUT_DIR}/SpoutSender.cpp ${SPOUT_DIR}/SpoutSenderNames.cpp
    ${SPOUT_DIR}/SpoutSharedMemory.cpp ${SPOUT_DIR}/SpoutUtils.cpp)
  target_include_directories(spout_gl PUBLIC ${SPOUT_DIR})
  target_compile_definitions(spout_gl PUBLIC SPOUT_BUILD_STATIC)
  if(MSVC)
    target_compile_options(spout_gl PRIVATE /W0)
  endif()
  target_link_libraries(spout_gl PUBLIC opengl32 d3d9 d3d11 dxgi version winmm shlwapi)
  target_sources(pinol_engine PRIVATE src/engine/PublishSpout.cpp)
  target_link_libraries(pinol_engine PRIVATE spout_gl)
  target_compile_definitions(pinol_engine PRIVATE PINOL_HAS_SPOUT)
endif()

# ---------------------------------------------------------------------------
# Application
# ---------------------------------------------------------------------------
set(APP_SOURCES
  assets/branding.qrc
  src/main.cpp
  src/ui/MainWindow.cpp
  src/ui/LogPanel.cpp
  src/ui/LogModel.cpp
  src/ui/LayerInspector.cpp
  src/ui/ParamPanel.cpp
  src/ui/PresetEditor.cpp
  src/ui/MappingView.cpp
  src/ui/PreviewWorkspace.cpp
  src/ui/MaskPanel.cpp
  src/ui/OutputWindow.cpp
  src/ui/GlDraw.cpp
  src/ui/Commands.cpp
  src/ui/LayerTable.cpp
  src/ui/MediaBin.cpp
  src/ui/CompositionPanel.cpp
  src/ui/ViewportOutput.cpp
  src/ui/SettingsPanel.cpp
  src/ui/SnapshotPanel.cpp
  src/ui/SequencePanel.cpp
  src/ui/TimelineActivity.cpp
  src/ui/AnimEditor.cpp
  src/ui/ParamAnimPanel.cpp
  src/ui/TimelinePanel.cpp
  src/ui/Widgets.cpp
  src/ui/ColorPicker.cpp
  src/ui/ColorPicker.h
  src/ui/InteractionPanel.cpp
  src/ui/AudioSpectrum.cpp
)

if(APPLE)
  enable_language(OBJCXX)
  list(APPEND APP_SOURCES src/ui/MacPresentation.mm)
  set_source_files_properties(assets/Pinol.icns PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
  add_executable(Pinol MACOSX_BUNDLE ${APP_SOURCES} assets/Pinol.icns)
  set_target_properties(Pinol PROPERTIES MACOSX_BUNDLE_ICON_FILE Pinol.icns)
  target_link_libraries(Pinol PRIVATE "-framework AppKit")
  # The camera permission plugin (static, from Qt): without it Qt refuses the camera before macOS is asked
  find_package(Qt6QDarwinCameraPermissionPlugin QUIET)
  if(TARGET Qt6::QDarwinCameraPermissionPlugin)
    target_link_libraries(Pinol PRIVATE Qt6::QDarwinCameraPermissionPlugin Qt6::QDarwinCameraPermissionPlugin_init)
    # Qt's permission handler is an Objective-C class in that static library: -ObjC keeps it in the link
    target_link_options(Pinol PRIVATE "-ObjC")
  else()
    message(WARNING "Qt camera permission plugin not found: the camera will not be asked for on macOS")
  endif()
  # The microphone permission plugin, the same way
  find_package(Qt6QDarwinMicrophonePermissionPlugin QUIET)
  if(TARGET Qt6::QDarwinMicrophonePermissionPlugin)
    target_link_libraries(Pinol PRIVATE Qt6::QDarwinMicrophonePermissionPlugin Qt6::QDarwinMicrophonePermissionPlugin_init)
  else()
    message(WARNING "Qt microphone permission plugin not found: the microphone will not be asked for on macOS")
  endif()
  set_target_properties(Pinol PROPERTIES
    MACOSX_BUNDLE_BUNDLE_NAME "Pinol"
    MACOSX_BUNDLE_GUI_IDENTIFIER "fr.pinol.app"
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/cmake/Info.plist.in") # with the camera's usage text
  set(ISF_DEST "$<TARGET_FILE_DIR:Pinol>/../Resources/isf")
elseif(WIN32)
  add_executable(Pinol WIN32 ${APP_SOURCES} assets/Pinol.rc)
  target_include_directories(Pinol PRIVATE "${CMAKE_SOURCE_DIR}/assets")
  set(ISF_DEST "$<TARGET_FILE_DIR:Pinol>/isf")
else()
  add_executable(Pinol ${APP_SOURCES})
  set(ISF_DEST "$<TARGET_FILE_DIR:Pinol>/isf")
endif()

# The source revision, for endurance-session comparison: regenerated at every build (cmake/BuildSha.cmake), not
# frozen at configure time
set(PINOL_BUILD_SHA_HEADER "${CMAKE_BINARY_DIR}/generated/pinol_build_sha.h")
add_custom_target(pinol_build_sha
  COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_SOURCE_DIR} -DOUT=${PINOL_BUILD_SHA_HEADER} -P ${CMAKE_SOURCE_DIR}/cmake/BuildSha.cmake
  BYPRODUCTS ${PINOL_BUILD_SHA_HEADER}
  VERBATIM)
add_dependencies(Pinol pinol_build_sha)
target_include_directories(Pinol PRIVATE "${CMAKE_BINARY_DIR}/generated")
target_include_directories(Pinol PRIVATE src src/ui)
target_link_libraries(Pinol PRIVATE pinol_engine Qt6::Widgets Qt6::OpenGLWidgets)

# Copy the bundled ISF shaders next to the executable (or into the macOS bundle).
# The folder is emptied first so renamed or deleted shaders do not linger in the build.
add_custom_command(TARGET Pinol POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E rm -rf "${ISF_DEST}"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/isf" "${ISF_DEST}")

# Windows: the FFmpeg DLLs must be next to Pinol.exe
if(WIN32 AND FFMPEG_ROOT)
  file(GLOB FFMPEG_DLLS "${FFMPEG_ROOT}/bin/*.dll")
  if(FFMPEG_DLLS)
    add_custom_command(TARGET Pinol POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FFMPEG_DLLS} "$<TARGET_FILE_DIR:Pinol>")
  endif()
endif()

target_compile_options(pinol_engine PRIVATE ${PINOL_WARNINGS})
target_compile_options(Pinol PRIVATE ${PINOL_WARNINGS})

install(TARGETS Pinol BUNDLE DESTINATION . RUNTIME DESTINATION bin)

# ---------------------------------------------------------------------------
# Fuzzing (Clang, libFuzzer + ASan / UBSan): the HAP and Snappy readers against damaged files (test/fuzz_hap.cpp)
# ---------------------------------------------------------------------------
option(PINOL_FUZZ "Build the fuzz targets (requires Clang)" OFF)
if(PINOL_FUZZ)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "PINOL_FUZZ needs Clang (libFuzzer): -DCMAKE_CXX_COMPILER=clang++")
  endif()
  add_executable(fuzz_hap EXCLUDE_FROM_ALL test/fuzz_hap.cpp src/engine/Hap.cpp src/engine/Snappy.cpp
    src/engine/WorkerPool.cpp)
  target_include_directories(fuzz_hap PRIVATE src/engine "${CMAKE_SOURCE_DIR}/third_party/bcdec")
  target_link_libraries(fuzz_hap PRIVATE Qt6::Core)
  target_compile_options(fuzz_hap PRIVATE -g -O1 -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined)
  target_link_options(fuzz_hap PRIVATE -fsanitize=fuzzer,address,undefined)
  add_executable(fuzz_osc EXCLUDE_FROM_ALL test/fuzz_osc.cpp src/engine/OscCodec.cpp)
  target_include_directories(fuzz_osc PRIVATE src/engine)
  target_link_libraries(fuzz_osc PRIVATE Qt6::Core)
  target_compile_options(fuzz_osc PRIVATE -g -O1 -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined)
  target_link_options(fuzz_osc PRIVATE -fsanitize=fuzzer,address,undefined)
endif()

# ---------------------------------------------------------------------------
# Engine tests (headless): cmake --build build --target pinol_tests
# ---------------------------------------------------------------------------
option(PINOL_TESTS "Build the engine tests" ON)
if(PINOL_TESTS)
  add_executable(pinol_tests EXCLUDE_FROM_ALL test/engine_test.cpp test/operator_state.cpp test/video_test.cpp test/cue_capture.cpp test/cue_lifecycle.cpp test/gpu_targets.cpp test/golden_render.cpp test/timeline_repeat.cpp test/preview_sources.cpp test/recall_delay.cpp test/emergency_stop.cpp src/ui/Commands.cpp src/ui/LogModel.cpp)
  target_include_directories(pinol_tests PRIVATE src/ui)
  target_link_libraries(pinol_tests PRIVATE pinol_engine)
  target_compile_options(pinol_tests PRIVATE ${PINOL_WARNINGS})
  target_compile_definitions(pinol_tests PRIVATE TEST_DIR="${CMAKE_SOURCE_DIR}/test")
  # CTest: each area in a process of its own (a new engine), then every bundled shader compiled and rendered.
  # Test media first: sh test/make_media.sh
  enable_testing()
  # GNU-compatible Linux linkers inject failures at our FFmpeg call sites only.
  # No test hooks or extra branches are compiled into the production decoder.
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(pinol_decoder_allocations_test test/decoder_allocations.cpp)
    target_link_libraries(pinol_decoder_allocations_test PRIVATE pinol_engine)
    target_compile_options(pinol_decoder_allocations_test PRIVATE ${PINOL_WARNINGS})
    target_compile_definitions(pinol_decoder_allocations_test PRIVATE TEST_DIR="${CMAKE_SOURCE_DIR}/test")
    target_link_options(pinol_decoder_allocations_test PRIVATE
      -Wl,--wrap=avcodec_alloc_context3 -Wl,--wrap=avcodec_parameters_to_context
      -Wl,--wrap=av_frame_alloc -Wl,--wrap=av_packet_alloc
      -Wl,--wrap=av_seek_frame -Wl,--wrap=avcodec_flush_buffers)
    add_test(NAME decoder_allocations COMMAND pinol_decoder_allocations_test)
    set_tests_properties(decoder_allocations PROPERTIES TIMEOUT 20)
  endif()
  find_package(Python3 QUIET COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(NAME ci_scope COMMAND ${Python3_EXECUTABLE} -m unittest discover -s ${CMAKE_SOURCE_DIR}/.github/scripts -p test_ci_scope.py)
    add_test(NAME champ_score COMMAND ${Python3_EXECUTABLE} -m unittest discover -s ${CMAKE_SOURCE_DIR}/examples/champ-de-rupture -p test_show.py)
    add_test(NAME champ_fixture COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/examples/champ-de-rupture/build_show.py --out ${CMAKE_BINARY_DIR}/champ-fixture)
    set_tests_properties(champ_fixture PROPERTIES FIXTURES_SETUP champ)
    # Full-resolution procedural shaders also run on Apple Software Renderer in CI.
    # Keep a bounded smoke-test timeout without treating software rendering as a GPU benchmark.
    add_test(NAME champ_render COMMAND $<TARGET_FILE:Pinol> --render ${CMAKE_BINARY_DIR}/champ-fixture/preview.png --frames 3 --strict ${CMAKE_BINARY_DIR}/champ-fixture/Champ_de_rupture.pinol)
    set_tests_properties(champ_render PROPERTIES FIXTURES_REQUIRED champ TIMEOUT 120)
    add_test(NAME matieres_score COMMAND ${Python3_EXECUTABLE} -m unittest discover -s ${CMAKE_SOURCE_DIR}/examples/matieres-instables -p test_show.py)
    add_test(NAME matieres_fixture COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/examples/matieres-instables/build_show.py --out ${CMAKE_BINARY_DIR}/matieres-fixture)
    set_tests_properties(matieres_fixture PROPERTIES FIXTURES_SETUP matieres)
    add_test(NAME matieres_render COMMAND $<TARGET_FILE:Pinol> --render ${CMAKE_BINARY_DIR}/matieres-fixture/preview.png --frames 3 --strict ${CMAKE_BINARY_DIR}/matieres-fixture/Matieres_instables.pinol)
    set_tests_properties(matieres_render PROPERTIES FIXTURES_REQUIRED matieres TIMEOUT 120)
  endif()
  add_executable(pinol_mask_geometry_test test/mask_geometry.cpp src/engine/Masks.cpp)
  target_include_directories(pinol_mask_geometry_test PRIVATE src/engine)
  target_link_libraries(pinol_mask_geometry_test PRIVATE Qt6::Gui)
  add_test(NAME mask_geometry COMMAND pinol_mask_geometry_test)
  find_package(Threads REQUIRED)
  add_executable(pinol_realtime_timing_test test/realtime_timing.cpp)
  target_include_directories(pinol_realtime_timing_test PRIVATE src/engine)
  target_link_libraries(pinol_realtime_timing_test PRIVATE Threads::Threads)
  add_test(NAME realtime_timing COMMAND pinol_realtime_timing_test)
  set_tests_properties(realtime_timing PROPERTIES TIMEOUT 30)

  add_executable(pinol_audio_analysis_test test/audio_analysis.cpp)
  target_include_directories(pinol_audio_analysis_test PRIVATE src/engine)
  add_test(NAME audio_analysis COMMAND pinol_audio_analysis_test)
  find_package(Threads REQUIRED)
  add_executable(pinol_audio_analysis_concurrency_test test/audio_analysis_concurrency.cpp)
  target_include_directories(pinol_audio_analysis_concurrency_test PRIVATE src/engine)
  target_link_libraries(pinol_audio_analysis_concurrency_test PRIVATE Threads::Threads)
  add_test(NAME audio_analysis_concurrency COMMAND pinol_audio_analysis_concurrency_test)
  set_tests_properties(audio_analysis_concurrency PROPERTIES TIMEOUT 60)
  add_executable(pinol_audio_interaction_ui_test test/audio_interaction_ui.cpp src/ui/InteractionPanel.cpp src/ui/AudioSpectrum.cpp src/ui/PresetEditor.cpp src/ui/Commands.cpp src/ui/Widgets.cpp src/ui/ColorPicker.cpp src/ui/AnimEditor.cpp)
  target_include_directories(pinol_audio_interaction_ui_test PRIVATE src/ui)
  target_link_libraries(pinol_audio_interaction_ui_test PRIVATE pinol_engine Qt6::Widgets)
  add_test(NAME audio_interaction_ui COMMAND pinol_audio_interaction_ui_test)
  add_executable(pinol_snapshot_update_ui_test test/snapshot_update_ui.cpp src/ui/PresetEditor.cpp src/ui/Commands.cpp)
  target_include_directories(pinol_snapshot_update_ui_test PRIVATE src/ui)
  target_link_libraries(pinol_snapshot_update_ui_test PRIVATE pinol_engine Qt6::Widgets)
  add_test(NAME snapshot_update_ui COMMAND pinol_snapshot_update_ui_test)
  add_executable(pinol_timeline_activity_test test/timeline_activity.cpp src/ui/TimelineActivity.cpp src/ui/SequencePanel.cpp src/ui/Widgets.cpp src/ui/ColorPicker.cpp)
  target_include_directories(pinol_timeline_activity_test PRIVATE src/ui)
  target_link_libraries(pinol_timeline_activity_test PRIVATE pinol_engine Qt6::Widgets)
  add_test(NAME timeline_activity COMMAND pinol_timeline_activity_test)
  set_tests_properties(timeline_activity PROPERTIES TIMEOUT 30)
  add_executable(pinol_cue_dispatch_test test/cue_dispatch.cpp)
  target_link_libraries(pinol_cue_dispatch_test PRIVATE pinol_engine Qt6::Widgets)
  add_test(NAME cue_dispatch COMMAND pinol_cue_dispatch_test)
  set_tests_properties(cue_dispatch PROPERTIES TIMEOUT 15)
  foreach(area operator_state preview_sources emergency_stop cue_capture cue_lifecycle gpu_targets golden timeline_repeat recall_delay isf_safe video project playback undo structure compositing osc snapshots masks rendering thread publishing
          media sound layersource timecode interactions)
    add_test(NAME ${area} COMMAND pinol_tests --area ${area})
  endforeach()
  # Areas whose checks depend on real time (sound clocks, threads, waits, publishers): label "timing". The CI runs the
  # others once, these with retries (ctest -L timing --repeat until-pass:3) — their thresholds stay as they are; a CI
  # runner without a sound card or a GPU is late now and then, a real regression fails every attempt
  set_tests_properties(sound playback thread publishing timecode snapshots media PROPERTIES LABELS timing)
  add_executable(pinol_sequence_ui_test test/sequence_ui.cpp src/ui/SequencePanel.cpp src/ui/TimelineActivity.cpp src/ui/Widgets.cpp src/ui/ColorPicker.cpp)
  target_include_directories(pinol_sequence_ui_test PRIVATE src/ui)
  target_link_libraries(pinol_sequence_ui_test PRIVATE pinol_engine Qt6::Widgets)
  add_test(NAME sequence_ui COMMAND pinol_sequence_ui_test)
  add_executable(pinol_number_widgets_test test/number_widgets.cpp src/ui/Widgets.cpp src/ui/ColorPicker.cpp)
  target_include_directories(pinol_number_widgets_test PRIVATE src/ui)
  target_link_libraries(pinol_number_widgets_test PRIVATE pinol_engine Qt6::Widgets)
  add_test(NAME number_widgets COMMAND pinol_number_widgets_test)
  add_executable(pinol_mask_ui_test test/mask_ui.cpp src/ui/MappingView.cpp src/ui/GlDraw.cpp
                 src/ui/Commands.cpp src/ui/Widgets.cpp src/ui/ColorPicker.cpp)
  target_include_directories(pinol_mask_ui_test PRIVATE src/ui)
  target_link_libraries(pinol_mask_ui_test PRIVATE pinol_engine Qt6::Widgets Qt6::OpenGLWidgets)
  add_test(NAME mask_ui COMMAND pinol_mask_ui_test)
  set(WORKSPACE_TEST_SOURCES ${APP_SOURCES})
  list(REMOVE_ITEM WORKSPACE_TEST_SOURCES src/main.cpp)
  add_executable(pinol_workspace_ui_test test/workspace_ui.cpp ${WORKSPACE_TEST_SOURCES})
  target_include_directories(pinol_workspace_ui_test PRIVATE src src/ui "${CMAKE_BINARY_DIR}/generated")
  target_link_libraries(pinol_workspace_ui_test PRIVATE pinol_engine Qt6::Widgets Qt6::OpenGLWidgets)
  target_compile_definitions(pinol_workspace_ui_test PRIVATE TEST_DIR="${CMAKE_SOURCE_DIR}/test")
  if(APPLE)
    target_link_libraries(pinol_workspace_ui_test PRIVATE "-framework AppKit")
  endif()
  add_test(NAME workspace_ui COMMAND pinol_workspace_ui_test)
  add_test(NAME selection_ui COMMAND pinol_workspace_ui_test --selection) # not in the Valgrind job: see the test
  add_executable(pinol_preview_ui_test test/preview_ui.cpp src/ui/PreviewWorkspace.cpp src/ui/MappingView.cpp
                 src/ui/GlDraw.cpp src/ui/Commands.cpp src/ui/Widgets.cpp src/ui/ColorPicker.cpp)
  target_include_directories(pinol_preview_ui_test PRIVATE src/ui)
  target_link_libraries(pinol_preview_ui_test PRIVATE pinol_engine Qt6::Widgets Qt6::OpenGLWidgets)
  add_test(NAME preview_ui COMMAND pinol_preview_ui_test)
  add_executable(pinol_recall_delay_ui_test test/recall_delay_ui.cpp src/ui/SnapshotPanel.cpp src/ui/PresetEditor.cpp
                 src/ui/SequencePanel.cpp src/ui/TimelineActivity.cpp src/ui/Commands.cpp src/ui/Widgets.cpp src/ui/ColorPicker.cpp)
  target_include_directories(pinol_recall_delay_ui_test PRIVATE src/ui)
  target_link_libraries(pinol_recall_delay_ui_test PRIVATE pinol_engine Qt6::Widgets)
  add_test(NAME recall_delay_ui COMMAND pinol_recall_delay_ui_test)
  add_executable(pinol_color_picker_ui_test test/color_picker_ui.cpp src/ui/Widgets.cpp src/ui/ColorPicker.cpp)
  target_include_directories(pinol_color_picker_ui_test PRIVATE src/ui)
  target_link_libraries(pinol_color_picker_ui_test PRIVATE pinol_engine Qt6::Widgets)
  add_test(NAME color_picker_ui COMMAND pinol_color_picker_ui_test)
  set_tests_properties(color_picker_ui PROPERTIES TIMEOUT 30)
  add_test(NAME isf COMMAND pinol_tests --check-isf "${CMAKE_SOURCE_DIR}/isf")
  if(NOT WIN32) # the application's exit codes in --render mode (Windows: its Qt libraries are deployed separately)
    add_test(NAME render_cli COMMAND ${CMAKE_COMMAND} -DPINOL=$<TARGET_FILE:Pinol> -DOUT=${CMAKE_BINARY_DIR}/render_cli
             -P "${CMAKE_SOURCE_DIR}/test/render_cli.cmake")
  endif()
  # Several videos played together: cmake --build build --target pinol_bench
  add_executable(pinol_bench EXCLUDE_FROM_ALL test/bench.cpp)
  target_link_libraries(pinol_bench PRIVATE pinol_engine)
endif()

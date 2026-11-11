cmake_minimum_required(VERSION 3.21)
project(WavyFlo VERSION 0.1 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
find_package(Qt6 REQUIRED COMPONENTS Widgets Multimedia Test OpenGL Network Qml)
if(WIN32)
  enable_language(RC)
endif()
qt_add_executable(WavyFlo WIN32 src/main.cpp src/engine.cpp src/scene3d.cpp src/scene3d_checks.cpp src/gpu_renderer.cpp src/gpu_checks.cpp src/performance_monitor.cpp src/render_export.cpp src/native_media.cpp src/image_files.cpp src/ffmpeg_export.cpp)
if(WIN32)
  target_sources(WavyFlo PRIVATE resources/wavy-flo.rc)
endif()
qt_add_resources(WavyFlo ui_icons PREFIX "/" FILES resources/dropdown.svg resources/dropdown-ink.svg resources/check.svg resources/graph.svg resources/wavy-flo.png)
target_sources(WavyFlo PRIVATE src/precision.cpp)
target_sources(WavyFlo PRIVATE src/surface_effects.cpp)
target_sources(WavyFlo PRIVATE src/gpu_pixels.cpp)
target_sources(WavyFlo PRIVATE src/effect_graph.cpp)
target_sources(WavyFlo PRIVATE src/plugin_runtime.cpp src/plugin_runtime_checks.cpp)
target_sources(WavyFlo PRIVATE src/color_picker.cpp)
target_sources(WavyFlo PRIVATE src/shapes.cpp)
target_sources(WavyFlo PRIVATE src/expressions.cpp src/expressions_checks.cpp)
target_sources(WavyFlo PRIVATE src/waveform.cpp)
target_sources(WavyFlo PRIVATE src/lyric_motion.cpp)
target_sources(WavyFlo PRIVATE src/effect_packs.cpp src/effect_packs_checks.cpp src/effect_editor.cpp)
target_sources(WavyFlo PRIVATE src/shader_cpu.cpp)
target_sources(WavyFlo PRIVATE src/particles.cpp src/particles_checks.cpp)
target_sources(WavyFlo PRIVATE src/tracker.cpp src/tracker_checks.cpp)
target_sources(WavyFlo PRIVATE src/video_frames.cpp src/video_frames_checks.cpp src/remap_audio.cpp)
target_sources(WavyFlo PRIVATE src/lights.cpp src/lights_checks.cpp)
target_sources(WavyFlo PRIVATE src/render_queue.cpp src/render_queue_panel.cpp src/render_queue_checks.cpp)
target_sources(WavyFlo PRIVATE src/color_checks.cpp)
target_sources(WavyFlo PRIVATE src/prerender.cpp)
# OpenFX: image effect plug-ins made for other hosts, run as effects on a
# layer (src/ofx_host.h). The OpenFX headers and two of the project's example
# plug-ins are in third_party/openfx; the examples are built into ofx-test,
# beside the program, for --ofx-self-test, and go no further.
target_sources(WavyFlo PRIVATE src/ofx_host.cpp src/ofx_worker.cpp src/ofx_checks.cpp src/ofx_gl.cpp)
if(WIN32)
  # ofx_gl.cpp makes its own WGL context for plug-ins that draw on the card.
  target_link_libraries(WavyFlo PRIVATE opengl32)
endif()
target_include_directories(WavyFlo PRIVATE third_party/openfx/include)
function(wavy_ofx_example target name)
  add_library(${target} MODULE ${ARGN})
  target_include_directories(${target} PRIVATE third_party/openfx/include third_party/openfx/examples/include)
  set(bundle "${CMAKE_BINARY_DIR}/ofx-test/${name}.ofx.bundle/Contents/Win64")
  set_target_properties(${target} PROPERTIES PREFIX "" SUFFIX ".ofx" OUTPUT_NAME "${name}"
    LIBRARY_OUTPUT_DIRECTORY "${bundle}" RUNTIME_OUTPUT_DIRECTORY "${bundle}")
  add_dependencies(WavyFlo ${target})
endfunction()
wavy_ofx_example(OfxInvertExample invert third_party/openfx/examples/Invert/invert.cpp)
wavy_ofx_example(OfxGainExample basic third_party/openfx/examples/Basic/basic.cpp)
# A generator and nothing else (none of the examples is only that).
wavy_ofx_example(OfxGeneratorTest generator tests/ofx_generator_plugin.cpp)
# A filter that draws beyond the picture it is given.
wavy_ofx_example(OfxSpillTest spill tests/ofx_spill_plugin.cpp)
# Text, a button and a group among the controls.
wavy_ofx_example(OfxControlsTest controls tests/ofx_controls_plugin.cpp)
# A second picture, a matte, needed.
wavy_ofx_example(OfxMatteTest matte tests/ofx_matte_plugin.cpp)
# A retimer, which talks and reports progress.
wavy_ofx_example(OfxRetimeTest retime tests/ofx_retime_plugin.cpp)
# One that draws only on the graphics card, through OpenGL.
wavy_ofx_example(OfxCardTest card tests/ofx_card_plugin.cpp)
if(WIN32)
  target_link_libraries(OfxCardTest PRIVATE opengl32)
endif()
# The OpenFX project's own test and example plug-ins, built into
# ofx-conformance for --ofx-self-test, and no further. Most plug-ins on the
# market are written on its Support library; built with DEBUG it checks every
# property set this host hands it against the standard and logs what is wrong
# (to OFX_PLUGIN_LOGFILE). Generators and transitions are among them on
# purpose: they must be turned away cleanly.
set(WAVY_OFX_SUPPORT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/openfx/support")
file(GLOB WAVY_OFX_SUPPORT_SOURCES "${WAVY_OFX_SUPPORT}/Library/*.cpp")
add_library(WavyOfxSupport STATIC ${WAVY_OFX_SUPPORT_SOURCES})
target_include_directories(WavyOfxSupport PUBLIC third_party/openfx/include "${WAVY_OFX_SUPPORT}/include"
  "${WAVY_OFX_SUPPORT}/Plugins/include")
target_compile_definitions(WavyOfxSupport PUBLIC DEBUG _USE_MATH_DEFINES)
function(wavy_ofx_conformance target name)
  add_library(${target} MODULE ${ARGN})
  target_include_directories(${target} PRIVATE third_party/openfx/include third_party/openfx/examples/include
    "${WAVY_OFX_SUPPORT}/include" "${WAVY_OFX_SUPPORT}/Plugins/include")
  target_compile_definitions(${target} PRIVATE DEBUG _USE_MATH_DEFINES)
  if(WIN32)
    target_link_libraries(${target} PRIVATE opengl32)
  endif()
  set(bundle "${CMAKE_BINARY_DIR}/ofx-conformance/${name}.ofx.bundle/Contents/Win64")
  set_target_properties(${target} PROPERTIES PREFIX "" SUFFIX ".ofx" OUTPUT_NAME "${name}"
    LIBRARY_OUTPUT_DIRECTORY "${bundle}" RUNTIME_OUTPUT_DIRECTORY "${bundle}")
  add_dependencies(WavyFlo ${target})
endfunction()
function(wavy_ofx_support_conformance target name)
  wavy_ofx_conformance(${target} ${name} ${ARGN})
  target_link_libraries(${target} PRIVATE WavyOfxSupport)
endfunction()
set(WAVY_OFX_PLUGS "${WAVY_OFX_SUPPORT}/Plugins")
set(WAVY_OFX_EXAMPLES "${CMAKE_CURRENT_SOURCE_DIR}/third_party/openfx/examples")
wavy_ofx_support_conformance(OfxConfPropTester proptester "${WAVY_OFX_SUPPORT}/PropTester/propTester.cpp")
wavy_ofx_support_conformance(OfxConfBasic support-basic "${WAVY_OFX_PLUGS}/Basic/basic.cpp")
wavy_ofx_support_conformance(OfxConfInvert support-invert "${WAVY_OFX_PLUGS}/Invert/invert.cpp")
wavy_ofx_support_conformance(OfxConfChoice support-choiceparams "${WAVY_OFX_PLUGS}/ChoiceParams/choiceparams.cpp")
wavy_ofx_support_conformance(OfxConfTester support-tester "${WAVY_OFX_PLUGS}/Tester/Tester.cpp")
wavy_ofx_support_conformance(OfxConfGenerator support-generator "${WAVY_OFX_PLUGS}/Generator/noise.cpp")
wavy_ofx_support_conformance(OfxConfTransition support-transition "${WAVY_OFX_PLUGS}/Transition/crossFade.cpp")
wavy_ofx_support_conformance(OfxConfRetimer support-retimer "${WAVY_OFX_PLUGS}/Retimer/retimer.cpp")
wavy_ofx_support_conformance(OfxConfField support-field "${WAVY_OFX_PLUGS}/Field/field.cpp")
wavy_ofx_support_conformance(OfxConfMultiBundle support-multibundle "${WAVY_OFX_PLUGS}/MultiBundle/PluginRegistration.cpp"
  "${WAVY_OFX_PLUGS}/MultiBundle/multibundle1.cpp" "${WAVY_OFX_PLUGS}/MultiBundle/multibundle2.cpp")
wavy_ofx_conformance(OfxConfCustom example-custom "${WAVY_OFX_EXAMPLES}/Custom/custom.cpp")
wavy_ofx_conformance(OfxConfRectangle example-rectangle "${WAVY_OFX_EXAMPLES}/Rectangle/rectangle.cpp")
wavy_ofx_conformance(OfxConfDepth example-depthconverter "${WAVY_OFX_EXAMPLES}/DepthConverter/depthConverter.cpp")
wavy_ofx_conformance(OfxConfChoiceC example-choiceparams "${WAVY_OFX_EXAMPLES}/ChoiceParams/choiceparams.cpp")
wavy_ofx_conformance(OfxConfTest example-test "${WAVY_OFX_EXAMPLES}/Test/testProperties.cpp" "${WAVY_OFX_EXAMPLES}/Test/ofxLog.cpp")
wavy_ofx_conformance(OfxConfOverlay example-overlay "${WAVY_OFX_EXAMPLES}/Overlay/overlay.cpp")
wavy_ofx_conformance(OfxConfDrawSuite example-drawsuite "${WAVY_OFX_EXAMPLES}/DrawSuite/drawsuite.cpp")
# Plug-ins that misbehave on purpose (tests/ofx_hazard_plugin.cpp), built into
# ofx-hazards for --ofx-self-test: the plug-in host process must take each of
# them -- a crash, a std::terminate while loading or rendering, a render that
# never returns -- and Wavy-Flo carry on.
function(wavy_ofx_hazard target name code)
  add_library(${target} MODULE tests/ofx_hazard_plugin.cpp)
  target_include_directories(${target} PRIVATE third_party/openfx/include)
  target_compile_definitions(${target} PRIVATE WAVY_HAZARD=${code})
  set(bundle "${CMAKE_BINARY_DIR}/ofx-hazards/${name}.ofx.bundle/Contents/Win64")
  set_target_properties(${target} PROPERTIES PREFIX "" SUFFIX ".ofx" OUTPUT_NAME "${name}"
    LIBRARY_OUTPUT_DIRECTORY "${bundle}" RUNTIME_OUTPUT_DIRECTORY "${bundle}")
  add_dependencies(WavyFlo ${target})
endfunction()
wavy_ofx_hazard(OfxHazardLoad hazard-load 1)
wavy_ofx_hazard(OfxHazardCrash hazard-crash 2)
wavy_ofx_hazard(OfxHazardTerminate hazard-terminate 3)
wavy_ofx_hazard(OfxHazardHang hazard-hang 4)
# Help > Report a Problem posts to the report channel's webhook, read here from
# report-webhook.txt beside this file -- kept out of the repository, so a
# build made elsewhere has no channel to send to and says so.
target_sources(WavyFlo PRIVATE src/report_dialog.cpp)
set(WAVY_REPORT_WEBHOOK "")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/report-webhook.txt")
  file(READ "${CMAKE_CURRENT_SOURCE_DIR}/report-webhook.txt" WAVY_REPORT_WEBHOOK)
  string(STRIP "${WAVY_REPORT_WEBHOOK}" WAVY_REPORT_WEBHOOK)
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/report-webhook.txt")
set_source_files_properties(src/report_dialog.cpp PROPERTIES COMPILE_DEFINITIONS "WAVY_REPORT_WEBHOOK=\"${WAVY_REPORT_WEBHOOK}\"")
target_sources(WavyFlo PRIVATE src/static_layers_checks.cpp)
# Scripts: Wavy-Flo's own API (wavy) and After Effects' (ExtendScript .jsx,
# ScriptUI), on Qt's JavaScript engine (src/script_host.h).
target_sources(WavyFlo PRIVATE src/script_host.cpp src/script_checks.cpp src/jsxbin.cpp)
# The bridge scripts call is a QObject (Q_INVOKABLE), so moc runs for it.
set_property(TARGET WavyFlo PROPERTY AUTOMOC ON)
qt_add_resources(WavyFlo scripting PREFIX "/" BASE resources FILES
  resources/scripting/wavy.js resources/scripting/scriptui.js resources/scripting/wavyui.js resources/scripting/ae.js)
target_sources(WavyFlo PRIVATE src/crash_report.cpp)
target_sources(WavyFlo PRIVATE src/updater.cpp)
if(WIN32)
  target_link_libraries(WavyFlo PRIVATE dbghelp)
endif()
if(MSVC)
  # main.cpp contains most of the editor UI and now legitimately produces more
  # COFF sections than MSVC's historical object-file limit.  This changes only
  # the object-file index width; it does not affect the executable or runtime.
  target_compile_options(WavyFlo PRIVATE /bigobj)
  # Symbols in an optimised build. /Zi only records what the code compiled to;
  # /OPT:REF and /OPT:ICF put back the folding that /DEBUG would otherwise turn
  # off, so the executable stays the size and the speed it was. Without this a
  # crash report can name a module and an offset but never a function.
  target_compile_options(WavyFlo PRIVATE $<$<CONFIG:Release>:/Zi>)
  target_link_options(WavyFlo PRIVATE $<$<CONFIG:Release>:/DEBUG /OPT:REF /OPT:ICF>)
  # 16 MB of stack for each thread instead of Windows' 1 MB. main() carries
  # every self-test inline and its frame alone measured 710 KB, which left the
  # whole running editor about 300 KB; the surface-effects test needs 334 KB
  # and died on entry (0xC00000FD) once each layer grew by ten properties.
  # Only address space is reserved: memory is committed as the stack is used.
  target_link_options(WavyFlo PRIVATE /STACK:16777216)
endif()
qt_add_resources(WavyFlo third_party_notices PREFIX "/" FILES third_party/FFMPEG.md CREDITS.md)
target_include_directories(WavyFlo PRIVATE sdk)
target_link_libraries(WavyFlo PRIVATE Qt6::Widgets Qt6::Multimedia Qt6::Test Qt6::OpenGL Qt6::Network Qt6::Qml Qt6::GuiPrivate)
set_target_properties(WavyFlo PROPERTIES OUTPUT_NAME "Wavy-Flo")
# Builder installs only the application executable. Stage the additional Qt
# runtime beside the project executable as part of a successful user build --
# where the source sits inside an installed Wavy-Flo (a Wavy-Flo.exe in the
# folder above). A copy of the source anywhere else keeps its build to itself.
set(WAVY_INSIDE_APP OFF)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../Wavy-Flo.exe")
  set(WAVY_INSIDE_APP ON)
endif()
if(WIN32)
  target_link_libraries(WavyFlo PRIVATE pdh psapi mfplat mfreadwrite mfuuid ole32 propsys d3d11 windowscodecs)
  if(WAVY_INSIDE_APP)
    set(WAVY_QT_OPENGL_STAGE "$<IF:$<CONFIG:Release>,${CMAKE_CURRENT_SOURCE_DIR}/../Qt6OpenGL.dll,$<TARGET_FILE_DIR:WavyFlo>/$<TARGET_FILE_NAME:Qt6::OpenGL>>")
  else()
    set(WAVY_QT_OPENGL_STAGE "$<TARGET_FILE_DIR:WavyFlo>/$<TARGET_FILE_NAME:Qt6::OpenGL>")
  endif()
  add_custom_command(TARGET WavyFlo POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "$<TARGET_FILE:Qt6::OpenGL>" "${WAVY_QT_OPENGL_STAGE}"
    VERBATIM)
  # And the JavaScript engine scripts run on, the same way.
  if(WAVY_INSIDE_APP)
    set(WAVY_QT_QML_STAGE "$<IF:$<CONFIG:Release>,${CMAKE_CURRENT_SOURCE_DIR}/../Qt6Qml.dll,$<TARGET_FILE_DIR:WavyFlo>/$<TARGET_FILE_NAME:Qt6::Qml>>")
  else()
    set(WAVY_QT_QML_STAGE "$<TARGET_FILE_DIR:WavyFlo>/$<TARGET_FILE_NAME:Qt6::Qml>")
  endif()
  add_custom_command(TARGET WavyFlo POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "$<TARGET_FILE:Qt6::Qml>" "${WAVY_QT_QML_STAGE}"
    VERBATIM)
endif()
# Effects are shared libraries with the extension .wfx, in effects/ beside the
# executable (sdk/wavy_effect.h). Wavy-Flo's own colour, stylize, noise and
# transition effects are packs like any other; the samples are too.
get_filename_component(WAVY_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
string(FIND "${CMAKE_BINARY_DIR}/" "${WAVY_ROOT}/" WAVY_BUILD_IN_ROOT)
# A Release build inside the project folder -- the Builder's own -- also puts
# the effects in the effects folder beside the installed Wavy-Flo.exe, the way
# Qt6OpenGL.dll is staged above. A build anywhere else leaves the installed
# application alone.
set(WAVY_STAGE_EFFECTS OFF)
if(WAVY_INSIDE_APP AND WAVY_BUILD_IN_ROOT EQUAL 0 AND CMAKE_BUILD_TYPE STREQUAL "Release")
  set(WAVY_STAGE_EFFECTS ON)
endif()
function(wavy_effect target output)
  add_library(${target} SHARED ${ARGN})
  target_include_directories(${target} PRIVATE sdk plugins)
  set_target_properties(${target} PROPERTIES PREFIX "" SUFFIX ".wfx" OUTPUT_NAME "${output}"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/effects" LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/effects")
  add_dependencies(WavyFlo ${target})
  if(WAVY_STAGE_EFFECTS)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory "${WAVY_ROOT}/effects"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:${target}>" "${WAVY_ROOT}/effects/$<TARGET_FILE_NAME:${target}>"
      VERBATIM)
  endif()
endfunction()
# OpenFX's licence asks that it travel with the program: in licenses/ beside
# the executable, which every release carries -- and beside the installed one
# when the Builder's build stages the effects there.
set(WAVY_OFX_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/openfx/LICENSE.md")
add_custom_command(TARGET WavyFlo POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:WavyFlo>/licenses"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${WAVY_OFX_LICENSE}" "$<TARGET_FILE_DIR:WavyFlo>/licenses/OpenFX-BSD-3-Clause.txt"
  VERBATIM)
if(WAVY_STAGE_EFFECTS)
  add_custom_command(TARGET WavyFlo POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${WAVY_ROOT}/licenses"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${WAVY_OFX_LICENSE}" "${WAVY_ROOT}/licenses/OpenFX-BSD-3-Clause.txt"
    VERBATIM)
endif()
# The example scripts (File > Scripts, Window > Script Panels) in Scripts/
# beside the executable, the same way. Only these files are written: a
# person's own scripts in that folder are left as they are.
add_custom_command(TARGET WavyFlo POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different "${CMAKE_CURRENT_SOURCE_DIR}/scripts" "$<TARGET_FILE_DIR:WavyFlo>/Scripts"
  VERBATIM)
if(WAVY_STAGE_EFFECTS)
  add_custom_command(TARGET WavyFlo POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different "${CMAKE_CURRENT_SOURCE_DIR}/scripts" "${WAVY_ROOT}/Scripts"
    VERBATIM)
endif()
wavy_effect(WavyColor "Wavy-Color" plugins/pack_color.cpp)
wavy_effect(WavyStylize "Wavy-Stylize" plugins/pack_stylize.cpp)
wavy_effect(WavyNoise "Wavy-Noise" plugins/pack_noise.cpp)
wavy_effect(WavyTransition "Wavy-Transition" plugins/pack_transition.cpp)
wavy_effect(WavyMotion "Wavy-Motion" plugins/pack_motion.cpp)
wavy_effect(WavyPop "Wavy-Pop" plugins/pack_pop.cpp)
wavy_effect(WavyInvert "WavyInvert" plugins/invert.cpp)
wavy_effect(WavyTint "WavyTint" plugins/tint.cpp)
wavy_effect(WavyVerticalBlur "WavyVerticalBlur" plugins/vertical_blur.cpp)
wavy_effect(WavyHostHalo "WavyHostHalo" plugins/host_halo.cpp)
# SDK 10's examples: a pack whose effect is a shader and nothing else, and
# effects written as text (.wfxs), which need no building at all -- they are
# copied beside the packs, and staged with them, the same way.
wavy_effect(WavyGlassWobble "WavyGlassWobble" sdk/examples/glass_wobble.cpp)
file(GLOB WAVY_TEXT_EFFECTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/sdk/examples/*.wfxs")
add_custom_target(WavyTextEffects ALL
  COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/effects"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${WAVY_TEXT_EFFECTS} "${CMAKE_BINARY_DIR}/effects"
  VERBATIM)
add_dependencies(WavyFlo WavyTextEffects)
if(WAVY_STAGE_EFFECTS)
  add_custom_command(TARGET WavyTextEffects POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${WAVY_ROOT}/effects"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${WAVY_TEXT_EFFECTS} "${WAVY_ROOT}/effects"
    VERBATIM)
endif()
# Packs built on this machine only, beside Wavy-Flo's own: local-packs.cmake,
# when it is there, adds them (and the checks for them, as WAVY_LOCAL_CHECKS).
include("${CMAKE_CURRENT_SOURCE_DIR}/local-packs.cmake" OPTIONAL)
if(APPLE)
 enable_language(OBJCXX)
 add_library(WavyMetal SHARED plugins/metal.mm)
 target_include_directories(WavyMetal PRIVATE sdk)
 target_link_libraries(WavyMetal PRIVATE "-framework Metal" "-framework Foundation")
 set_target_properties(WavyMetal PROPERTIES PREFIX "")
endif()
enable_testing()
add_test(NAME effect_graph COMMAND WavyFlo --effect-graph-self-test)
add_test(NAME effect_graph_gpu COMMAND WavyFlo --effect-graph-gpu-self-test)
set_tests_properties(effect_graph_gpu PROPERTIES SKIP_RETURN_CODE 77)
set_tests_properties(effect_graph PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_test(NAME gpu_pixels COMMAND WavyFlo --gpu-pixels-self-test)
add_test(NAME plugin_runtime COMMAND WavyFlo --plugin-runtime-self-test)
set_tests_properties(plugin_runtime PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_test(NAME plugin_shader COMMAND WavyFlo --plugin-shader-self-test)
set_tests_properties(plugin_shader PROPERTIES SKIP_RETURN_CODE 77)
set_tests_properties(gpu_pixels PROPERTIES SKIP_RETURN_CODE 77)
add_test(NAME prerender COMMAND WavyFlo --prerender-self-test)
add_test(NAME static_layers COMMAND WavyFlo --static-layers-self-test)
add_test(NAME preview_playback COMMAND WavyFlo --preview-playback-self-test)
set_tests_properties(preview_playback PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
set_tests_properties(prerender PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_test(NAME shapes COMMAND WavyFlo --shapes-self-test)
set_tests_properties(shapes PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_test(NAME color_picker COMMAND WavyFlo --color-picker-self-test)
set_tests_properties(color_picker PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_test(NAME surface_effects COMMAND WavyFlo --surface-effects-self-test)
set_tests_properties(surface_effects PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_test(NAME precision COMMAND WavyFlo --precision-self-test)
set_tests_properties(precision PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_test(NAME ffmpeg_export_arguments COMMAND WavyFlo --ffmpeg-export-self-test)
set_tests_properties(ffmpeg_export_arguments PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_test(NAME native_media COMMAND WavyFlo --native-media-self-test)
set_tests_properties(native_media PROPERTIES SKIP_RETURN_CODE 77)
add_test(NAME scene_3d COMMAND WavyFlo --3d-self-test)
add_test(NAME native_engine COMMAND WavyFlo --self-test)
set_tests_properties(native_engine PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_test(NAME gpu_renderer COMMAND WavyFlo --gpu-self-test)
set_tests_properties(gpu_renderer PROPERTIES SKIP_RETURN_CODE 77)
add_test(NAME ofx_host COMMAND WavyFlo --ofx-self-test)
set_tests_properties(ofx_host PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Fetches the reports from the report channel into a folder (tools/reports.cpp).
qt_add_executable(WavyFloReports tools/reports.cpp)
target_link_libraries(WavyFloReports PRIVATE Qt6::Core Qt6::Network)
qt_add_executable(WavyFloBuilder WIN32 tools/builder.cpp)
target_link_libraries(WavyFloBuilder PRIVATE Qt6::Widgets)
set_target_properties(WavyFloBuilder PROPERTIES OUTPUT_NAME "Wavy-Flo Builder")

if(WIN32)
  add_executable(WavyFloBuilderLauncher WIN32 tools/builder_launcher.cpp)
  target_link_libraries(WavyFloBuilderLauncher PRIVATE shell32 user32)
  set_property(TARGET WavyFloBuilderLauncher PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  set_target_properties(WavyFloBuilderLauncher PROPERTIES OUTPUT_NAME "Wavy-Flo Builder Launcher")
endif()

# Wavy-Flo Setup: the installer. Plain Win32 with the C++ runtime linked in, so
# the one exe runs on a machine with nothing of Wavy-Flo on it yet; the program
# is packed onto its end by tools/publish-release.ps1 (--pack).
if(WIN32)
  add_executable(WavyFloSetup WIN32 tools/setup.cpp tools/setup.rc)
  target_compile_definitions(WavyFloSetup PRIVATE UNICODE _UNICODE)
  target_link_libraries(WavyFloSetup PRIVATE cabinet comctl32 shell32 ole32 uuid advapi32 user32 gdi32 winhttp bcrypt)
  set_property(TARGET WavyFloSetup PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  # Its own manifest is in setup.rc.
  target_link_options(WavyFloSetup PRIVATE /MANIFEST:NO)
  set_target_properties(WavyFloSetup PROPERTIES OUTPUT_NAME "Wavy-Flo Setup")
  # Carrying no program it is the Wavy-Flo Installer, which always fetches the
  # newest Wavy-Flo: published once (tools/publish-installer.ps1), never again.
  add_custom_command(TARGET WavyFloSetup POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:WavyFloSetup>" "${CMAKE_BINARY_DIR}/Wavy-Flo-Installer.exe"
    VERBATIM)
endif()

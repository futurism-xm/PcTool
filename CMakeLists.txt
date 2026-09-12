cmake_minimum_required(VERSION 3.24)

project(PcTool VERSION 0.1.0 LANGUAGES CXX RC)

add_executable(PcToolUninstallHelper src/installer/uninstall_helper.cpp)
target_compile_features(PcToolUninstallHelper PRIVATE cxx_std_17)
target_link_libraries(PcToolUninstallHelper PRIVATE Advapi32 Userenv User32)
if(MSVC)
    target_compile_options(PcToolUninstallHelper PRIVATE /W4 /utf-8 /EHsc)
    set_property(TARGET PcToolUninstallHelper PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_executable(PcToolStorageTests tests/storage_tests.cpp src/app/app_settings.cpp)
add_executable(PcToolArchiveStorageTests tests/archive_storage_tests.cpp)
target_compile_features(PcToolArchiveStorageTests PRIVATE cxx_std_17)
target_compile_definitions(PcToolArchiveStorageTests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(PcToolArchiveStorageTests PRIVATE Advapi32 User32)
target_include_directories(PcToolStorageTests PRIVATE src)
target_compile_features(PcToolStorageTests PRIVATE cxx_std_17)
target_compile_definitions(PcToolStorageTests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(PcToolStorageTests PRIVATE Advapi32 Ole32 Shell32 User32)
if(MSVC)
    target_compile_options(PcToolStorageTests PRIVATE /utf-8 /EHsc)
endif()

include(cmake/PcToolModules.cmake)

add_executable(PcTool WIN32
    ${PCTOOL_CPP_SOURCES}
    ${PCTOOL_LOCAL_HEADERS}
    src/app/app.rc
)

# This Chinese MSVC + Ninja setup does not record localized /showIncludes
# output, so Ninja otherwise reports #deps 0 for every object. Explicit local
# header dependencies prevent stale object layouts after a header change.
if(MSVC AND CMAKE_GENERATOR MATCHES "Ninja")
    set(PCTOOL_ABSOLUTE_HEADERS)
    foreach(header IN LISTS PCTOOL_LOCAL_HEADERS)
        list(APPEND PCTOOL_ABSOLUTE_HEADERS
            "${CMAKE_CURRENT_SOURCE_DIR}/${header}")
    endforeach()
    set_property(SOURCE ${PCTOOL_CPP_SOURCES} APPEND PROPERTY
        OBJECT_DEPENDS "${PCTOOL_ABSOLUTE_HEADERS}")
endif()

target_include_directories(PcTool PRIVATE src src/app)

target_compile_features(PcTool PRIVATE cxx_std_17)
target_compile_definitions(PcTool PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    # SDK C++/WinRT includes the legacy coroutine header even without coroutine use.
    _SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS
)

target_link_libraries(PcTool PRIVATE
    Winhttp
    Crypt32
    Uxtheme
    Advapi32
    Comctl32
    Dwmapi
    Gdiplus
    Imm32
    Iphlpapi
    Msimg32
    Ole32
    Pdh
    PowrProf
    Windowscodecs
    comdlg32
    RuntimeObject
    WindowsApp
    D3D11
    Dxgi
    Mfplat
    Mfreadwrite
    Mfuuid
    Mf
    Mfplay
    Propsys
)

if(MSVC)
    target_compile_options(PcTool PRIVATE
        /W4
        /permissive-
        /utf-8
        /EHsc
        $<$<CONFIG:Release>:/Gy>
        $<$<CONFIG:Release>:/Gw>
        $<$<CONFIG:Release>:/GF>
        $<$<CONFIG:Release>:/Zc:inline>
    )
    # app.RC already embeds the DPI/common-controls manifest as resource ID 1.
    target_link_options(PcTool PRIVATE
        /MANIFEST:NO
        $<$<CONFIG:Release>:/OPT:REF>
        $<$<CONFIG:Release>:/OPT:ICF>
    )
endif()

set_property(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" PROPERTY VS_STARTUP_PROJECT PcTool)

include(CTest)
if(BUILD_TESTING)
    add_executable(PcToolUninstallFixture tests/uninstall_process_fixture.cpp)
    target_compile_features(PcToolUninstallFixture PRIVATE cxx_std_17)
    target_compile_definitions(PcToolUninstallFixture PRIVATE UNICODE _UNICODE)
    target_link_libraries(PcToolUninstallFixture PRIVATE Advapi32 User32)
    if(MSVC)
        target_compile_options(PcToolUninstallFixture PRIVATE /utf-8 /EHsc)
    endif()
    add_executable(PcToolCaptureTests tests/capture_core_tests.cpp src/shared/image/image_document.cpp src/long_capture/scroll_stitcher.cpp src/ocr/ocr_text_merge.cpp)
    target_include_directories(PcToolCaptureTests PRIVATE src)
    target_compile_features(PcToolCaptureTests PRIVATE cxx_std_17)
    target_compile_definitions(PcToolCaptureTests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    if(MSVC)
        target_compile_options(PcToolCaptureTests PRIVATE /W4 /utf-8 /EHsc)
        # The localized Ninja /showIncludes limitation above applies to this
        # test translation unit too. Keep Annotation/Image ABI in sync with the
        # shared sources when local image or annotation headers change.
        set_property(SOURCE tests/capture_core_tests.cpp APPEND PROPERTY OBJECT_DEPENDS "${PCTOOL_ABSOLUTE_HEADERS}")
    endif()
    add_test(NAME capture_core COMMAND PcToolCaptureTests)
    add_executable(PcToolScrollAnnotationTests tests/scroll_annotation_tests.cpp src/shared/annotation/annotation_painter.cpp)
    target_include_directories(PcToolScrollAnnotationTests PRIVATE src)
    target_compile_features(PcToolScrollAnnotationTests PRIVATE cxx_std_17)
    target_compile_definitions(PcToolScrollAnnotationTests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    if(MSVC)
        target_compile_options(PcToolScrollAnnotationTests PRIVATE /W4 /utf-8 /EHsc)
        set_property(SOURCE tests/scroll_annotation_tests.cpp APPEND PROPERTY OBJECT_DEPENDS "${PCTOOL_ABSOLUTE_HEADERS}")
    endif()
    target_link_libraries(PcToolScrollAnnotationTests PRIVATE gdiplus gdi32 user32 msimg32)
    add_test(NAME capture_scroll_annotations COMMAND PcToolScrollAnnotationTests)
    add_executable(PcToolTaskbarTests tests/taskbar_layout_tests.cpp src/monitor/taskbar_monitor_window.cpp src/app/app_settings.cpp src/monitor/system_monitor.cpp)
    target_include_directories(PcToolTaskbarTests PRIVATE src)
    target_compile_features(PcToolTaskbarTests PRIVATE cxx_std_17)
    get_target_property(PCTOOL_TEST_DEFINITIONS PcTool COMPILE_DEFINITIONS)
    get_target_property(PCTOOL_TEST_LIBRARIES PcTool LINK_LIBRARIES)
    target_compile_definitions(PcToolTaskbarTests PRIVATE ${PCTOOL_TEST_DEFINITIONS})
    target_link_libraries(PcToolTaskbarTests PRIVATE ${PCTOOL_TEST_LIBRARIES})
    if(MSVC)
        target_compile_options(PcToolTaskbarTests PRIVATE /W4 /utf-8 /EHsc)
        set_property(SOURCE tests/taskbar_layout_tests.cpp APPEND PROPERTY OBJECT_DEPENDS "${PCTOOL_ABSOLUTE_HEADERS}")
    endif()
    add_executable(PcToolClipboardTests tests/clipboard_history_tests.cpp src/system_tools/paint.cpp src/clipboard/clipboard_history.cpp src/shared/ui/popup_controls.cpp)
    target_include_directories(PcToolClipboardTests PRIVATE src)
    target_compile_features(PcToolClipboardTests PRIVATE cxx_std_17)
    target_compile_definitions(PcToolClipboardTests PRIVATE ${PCTOOL_TEST_DEFINITIONS})
    target_link_libraries(PcToolClipboardTests PRIVATE ${PCTOOL_TEST_LIBRARIES})
    if(MSVC)
        target_compile_options(PcToolClipboardTests PRIVATE /W4 /utf-8 /EHsc)
        set_property(SOURCE tests/clipboard_history_tests.cpp APPEND PROPERTY OBJECT_DEPENDS "${PCTOOL_ABSOLUTE_HEADERS}")
    endif()
    add_test(NAME clipboard_history_core COMMAND PcToolClipboardTests)
    add_executable(PcToolOcrTests tests/ocr_layout_tests.cpp src/ocr/ocr_layout.cpp)
    target_include_directories(PcToolOcrTests PRIVATE src)
    target_compile_features(PcToolOcrTests PRIVATE cxx_std_17)
    target_compile_definitions(PcToolOcrTests PRIVATE ${PCTOOL_TEST_DEFINITIONS})
    if(MSVC)
        target_compile_options(PcToolOcrTests PRIVATE /W4 /utf-8 /EHsc)
    endif()
    add_test(NAME ocr_layout COMMAND PcToolOcrTests)

    add_test(NAME taskbar_layout COMMAND PcToolTaskbarTests)
    add_executable(PcToolTrayTests tests/tray_icon_tests.cpp)
    target_include_directories(PcToolTrayTests PRIVATE src)
    target_compile_features(PcToolTrayTests PRIVATE cxx_std_17)
    target_compile_definitions(PcToolTrayTests PRIVATE ${PCTOOL_TEST_DEFINITIONS})
    target_link_libraries(PcToolTrayTests PRIVATE ${PCTOOL_TEST_LIBRARIES})
    if(MSVC)
        target_compile_options(PcToolTrayTests PRIVATE /W4 /utf-8 /EHsc)
    endif()
    add_test(NAME tray_icon_lifecycle COMMAND PcToolTrayTests)
    add_executable(PcToolHotkeyTests tests/hotkey_settings_tests.cpp src/hotkeys/hotkey_settings.cpp src/shared/ui/settings_frame.cpp src/shared/ui/confirmation.cpp src/shared/ui/popup_controls.cpp src/shared/ui/toolbar_icons.cpp)
    target_include_directories(PcToolHotkeyTests PRIVATE src)
    target_compile_features(PcToolHotkeyTests PRIVATE cxx_std_17)
    target_compile_definitions(PcToolHotkeyTests PRIVATE ${PCTOOL_TEST_DEFINITIONS})
    target_link_libraries(PcToolHotkeyTests PRIVATE ${PCTOOL_TEST_LIBRARIES})
    if(MSVC)
        target_compile_options(PcToolHotkeyTests PRIVATE /W4 /utf-8 /EHsc)
        set_property(SOURCE tests/hotkey_settings_tests.cpp APPEND PROPERTY OBJECT_DEPENDS "${PCTOOL_ABSOLUTE_HEADERS}")
    endif()
    add_executable(PcToolCaptureIntegration tests/capture_integration.cpp
        tests/screenshot_interaction_tests.cpp
        tests/ocr_comparison.cpp
        src/shared/image/image_document.cpp src/long_capture/scroll_stitcher.cpp src/ocr/ocr_text_merge.cpp src/shared/platform/capture_platform.cpp src/shared/ui/capture_ui.cpp
        src/long_capture/long_capture_session.cpp src/app/capture_tools.cpp src/ocr/ocr_layout.cpp src/ocr/ocr_window.cpp src/ocr/ocr_service.cpp src/ocr/paddle_ocr.cpp src/recording/screen_recorder.cpp src/recording/gif_recording.cpp src/recording/gif_preview.cpp
        src/shared/annotation/annotation_style.cpp
    src/shared/ui/toolbar_icons.cpp src/recording/recording_session.cpp src/recording/recording_preview.cpp src/shared/annotation/annotation_painter.cpp src/shared/annotation/annotation_renderer.cpp
        src/shared/ui/popup_controls.cpp src/capture/image_editor.cpp src/capture/screenshot_overlay.cpp src/app/app.rc)
    set_property(SOURCE tests/capture_integration.cpp tests/screenshot_interaction_tests.cpp tests/ocr_comparison.cpp APPEND PROPERTY OBJECT_DEPENDS "${PCTOOL_ABSOLUTE_HEADERS}")
    target_include_directories(PcToolCaptureIntegration PRIVATE src src/app)
    target_compile_features(PcToolCaptureIntegration PRIVATE cxx_std_17)
    get_target_property(PCTOOL_DEFINITIONS PcTool COMPILE_DEFINITIONS)
    get_target_property(PCTOOL_LIBRARIES PcTool LINK_LIBRARIES)
    target_compile_definitions(PcToolCaptureIntegration PRIVATE ${PCTOOL_DEFINITIONS})
    target_link_libraries(PcToolCaptureIntegration PRIVATE ${PCTOOL_LIBRARIES} Winmm)
    if(MSVC)
        target_compile_options(PcToolCaptureIntegration PRIVATE /W4 /utf-8 /EHsc)
        target_link_options(PcToolCaptureIntegration PRIVATE /MANIFEST:NO)
    endif()
    add_test(NAME recording_annotation_alpha COMMAND PcToolCaptureIntegration annotation-test "${CMAKE_CURRENT_BINARY_DIR}/annotation")
    add_test(NAME toolbar_icons COMMAND PcToolCaptureIntegration icon-test "${CMAKE_CURRENT_BINARY_DIR}/toolbar-icons")
    add_test(NAME recording_discard_files COMMAND PcToolCaptureIntegration discard-files-test "${CMAKE_CURRENT_BINARY_DIR}/discard-files")
    # Optional, test-only Electron runtime; never installed or used by PcTool.
    set(PCTOOL_ELECTRON_TEST_EXECUTABLE "" CACHE FILEPATH "Electron executable for real Chromium wheel-routing tests")
    if(PCTOOL_ELECTRON_TEST_EXECUTABLE)
        add_test(NAME capture_electron_scroll COMMAND "${PCTOOL_ELECTRON_TEST_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/electron_scroll_fixture.cjs"
            "$<TARGET_FILE:PcToolCaptureIntegration>" "${CMAKE_CURRENT_BINARY_DIR}/electron-scroll")
        set_tests_properties(capture_electron_scroll PROPERTIES RUN_SERIAL TRUE TIMEOUT 120 LABELS desktop)
        add_test(NAME capture_electron_scroll_large_first COMMAND "${PCTOOL_ELECTRON_TEST_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/electron_scroll_fixture.cjs"
            "$<TARGET_FILE:PcToolCaptureIntegration>" "${CMAKE_CURRENT_BINARY_DIR}/electron-scroll-large" --large-first-wheel)
        set_tests_properties(capture_electron_scroll_large_first PROPERTIES RUN_SERIAL TRUE TIMEOUT 120 LABELS desktop)
    endif()
    # Interactive desktop/audio integration is opt-in; core tests work unattended.
    option(PCTOOL_ENABLE_DESKTOP_TESTS "Enable tests requiring an interactive desktop, Chinese OCR and audio devices" OFF)
    if(PCTOOL_ENABLE_DESKTOP_TESTS)
        add_test(NAME ocr_local_comparison COMMAND PcToolCaptureIntegration ocr-compare "${CMAKE_CURRENT_BINARY_DIR}/ocr-comparison")
        set_tests_properties(ocr_local_comparison PROPERTIES RUN_SERIAL TRUE TIMEOUT 120 LABELS desktop)
        add_test(NAME capture_pin_interactions COMMAND PcToolCaptureIntegration pin-interaction-test "${CMAKE_CURRENT_BINARY_DIR}/pin-interactions")
        set_tests_properties(capture_pin_interactions PROPERTIES RUN_SERIAL TRUE TIMEOUT 120 LABELS desktop)
        add_test(NAME clipboard_history_ui COMMAND PcToolClipboardTests ui-test)
        set_tests_properties(clipboard_history_ui PROPERTIES RUN_SERIAL TRUE TIMEOUT 30 LABELS desktop)
        add_test(NAME taskbar_menu_input COMMAND PcToolTaskbarTests menu-test)
        set_tests_properties(taskbar_menu_input PROPERTIES RUN_SERIAL TRUE TIMEOUT 20 LABELS desktop)
        add_test(NAME capture_gif COMMAND PcToolCaptureIntegration gif-test "${CMAKE_CURRENT_BINARY_DIR}/gif-test")
        set_tests_properties(capture_gif PROPERTIES RUN_SERIAL TRUE TIMEOUT 90 LABELS desktop)
        add_test(NAME capture_recording_visual COMMAND PcToolCaptureIntegration record-visual-test "${CMAKE_CURRENT_BINARY_DIR}/record-visual")
        add_test(NAME capture_recording_exit COMMAND PcToolCaptureIntegration record-exit-test)
        add_test(NAME capture_recording_styles COMMAND PcToolCaptureIntegration style-test "${CMAKE_CURRENT_BINARY_DIR}/record-style")
        add_test(NAME capture_recording_cursor COMMAND PcToolCaptureIntegration record-cursor-test "${CMAKE_CURRENT_BINARY_DIR}/record-cursor")
        add_test(NAME capture_recording_prepare COMMAND PcToolCaptureIntegration prepare-test)
        add_test(NAME capture_ocr COMMAND PcToolCaptureIntegration ocr)
        add_test(NAME capture_ocr_ui COMMAND PcToolCaptureIntegration ocr-ui-test "${CMAKE_CURRENT_BINARY_DIR}/ocr-ui")
        add_test(NAME capture_ocr_source COMMAND PcToolCaptureIntegration ocr-SOURCE-test "${CMAKE_CURRENT_BINARY_DIR}/ocr-SOURCE")
        set_tests_properties(capture_ocr_source PROPERTIES RUN_SERIAL TRUE TIMEOUT 30 LABELS desktop)
        set_tests_properties(capture_ocr_ui PROPERTIES RUN_SERIAL TRUE TIMEOUT 120 LABELS desktop)
        add_test(NAME capture_editor COMMAND PcToolCaptureIntegration editor-test "${CMAKE_CURRENT_BINARY_DIR}/editor-test")
        add_test(NAME capture_partial_scroll COMMAND PcToolCaptureIntegration partial-scroll-test "${CMAKE_CURRENT_BINARY_DIR}/partial-scroll")
        set_tests_properties(capture_partial_scroll PROPERTIES RUN_SERIAL TRUE TIMEOUT 180 LABELS desktop)
        add_test(NAME capture_scroll_annotation_editor COMMAND PcToolCaptureIntegration scroll-annotation-editor-test "${CMAKE_CURRENT_BINARY_DIR}/scroll-annotations")
        set_tests_properties(capture_scroll_annotation_editor PROPERTIES RUN_SERIAL TRUE TIMEOUT 45 LABELS desktop)
        add_test(NAME capture_qq_long COMMAND PcToolCaptureIntegration qq-long-test "${CMAKE_CURRENT_BINARY_DIR}/qq-long")
        add_test(NAME capture_live_scroll COMMAND PcToolCaptureIntegration long-test "${CMAKE_CURRENT_BINARY_DIR}/live-scroll.png")
        add_test(NAME capture_sessions COMMAND PcToolCaptureIntegration session-test)
        add_test(NAME capture_recording_ui COMMAND PcToolCaptureIntegration record-ui-test "${CMAKE_CURRENT_BINARY_DIR}/record-ui")
        add_test(NAME capture_recording_preview COMMAND PcToolCaptureIntegration preview-test "${CMAKE_CURRENT_BINARY_DIR}/record-preview")
        add_test(NAME capture_discard_ui COMMAND PcToolCaptureIntegration discard-ui-test "${CMAKE_CURRENT_BINARY_DIR}/discard-ui")
        add_test(NAME capture_live_audio COMMAND PcToolCaptureIntegration live-audio-test "${CMAKE_CURRENT_BINARY_DIR}/live-audio.mp4")
        set(PCTOOL_DESKTOP_TEST_NAMES capture_recording_visual capture_recording_exit capture_recording_styles capture_recording_cursor capture_recording_prepare capture_ocr capture_editor capture_qq_long capture_live_scroll capture_sessions capture_recording_ui capture_live_audio capture_discard_ui)
        foreach(audio_mode silent system mic both)
            add_test(NAME capture_record_${audio_mode} COMMAND PcToolCaptureIntegration ${audio_mode} 4 "${CMAKE_CURRENT_BINARY_DIR}/record-${audio_mode}.mp4")
            list(APPEND PCTOOL_DESKTOP_TEST_NAMES capture_record_${audio_mode})
        endforeach()
        set_tests_properties(${PCTOOL_DESKTOP_TEST_NAMES} PROPERTIES RUN_SERIAL TRUE TIMEOUT 45 LABELS desktop)
        set_tests_properties(capture_recording_preview PROPERTIES RUN_SERIAL TRUE TIMEOUT 60 LABELS desktop)
    endif()
endif()

if(BUILD_TESTING)
    add_executable(PcToolTranslationTests tests/translation_tests.cpp tests/translation_source_tests.cpp
        ${PCTOOL_TRANSLATION_SOURCES} ${PCTOOL_SHARED_SOURCES}
        src/ocr/paddle_ocr.cpp src/ocr/ocr_layout.cpp
        src/clipboard/clipboard_history.cpp src/app/app.rc
        src/monitor/taskbar_monitor_window.cpp src/monitor/system_monitor.cpp src/app/app_settings.cpp)
    target_include_directories(PcToolTranslationTests PRIVATE src src/app third_party/ocr/include)
    target_compile_features(PcToolTranslationTests PRIVATE cxx_std_17)
    target_compile_definitions(PcToolTranslationTests PRIVATE ${PCTOOL_DEFINITIONS})
    target_link_libraries(PcToolTranslationTests PRIVATE ${PCTOOL_LIBRARIES} Ws2_32)
    if(MSVC)
        target_compile_options(PcToolTranslationTests PRIVATE /W4 /utf-8 /EHsc)
        target_link_options(PcToolTranslationTests PRIVATE /MANIFEST:NO)
    endif()
    set_property(SOURCE tests/translation_tests.cpp APPEND PROPERTY OBJECT_DEPENDS "${PCTOOL_ABSOLUTE_HEADERS}")
    add_test(NAME translation_core COMMAND PcToolTranslationTests)
    add_test(NAME translation_sources_core COMMAND PcToolTranslationTests sources "${CMAKE_CURRENT_BINARY_DIR}/translation-sources")
    if(PCTOOL_ENABLE_DESKTOP_TESTS)
        add_test(NAME installation_storage COMMAND powershell -NoProfile -ExecutionPolicy Bypass -file "${CMAKE_SOURCE_DIR}/tests/run_storage_tests.ps1" -Binaries "$<TARGET_FILE_DIR:PcToolStorageTests>" -Output "${CMAKE_CURRENT_BINARY_DIR}/storage-tests")
        set_tests_properties(installation_storage PROPERTIES RUN_SERIAL TRUE TIMEOUT 60 LABELS desktop)
        add_test(NAME hotkey_settings_ui COMMAND PcToolHotkeyTests)
        set_tests_properties(hotkey_settings_ui PROPERTIES RUN_SERIAL TRUE TIMEOUT 20 LABELS desktop)
        set(PCTOOL_TEST_NOTEPADPP "" CACHE FILEPATH "Optional Notepad++ executable for isolated Alt+E regression")
        if(EXISTS "${PCTOOL_TEST_NOTEPADPP}")
            add_test(NAME translation_notepadpp COMMAND PcToolTranslationTests npp "${CMAKE_CURRENT_BINARY_DIR}/translation-notepadpp" "${PCTOOL_TEST_NOTEPADPP}")
            set_tests_properties(translation_notepadpp PROPERTIES RUN_SERIAL TRUE TIMEOUT 120 LABELS desktop)
        endif()
        add_test(NAME translation_ui COMMAND PcToolTranslationTests ui "${CMAKE_CURRENT_BINARY_DIR}/translation-ui")
        add_test(NAME translation_sources_ui COMMAND PcToolTranslationTests sources-ui "${CMAKE_CURRENT_BINARY_DIR}/translation-sources-ui")
        set_tests_properties(translation_sources_ui PROPERTIES RUN_SERIAL TRUE TIMEOUT 120 LABELS desktop)
        set_tests_properties(translation_ui PROPERTIES RUN_SERIAL TRUE TIMEOUT 120 LABELS desktop)
    endif()
endif()

# Local CPU OCR; model/runtime downloads are an explicit developer setup step.
option(PCTOOL_BUILD_OCR_BENCHMARK "Build isolated CPU OCR evaluation tool" OFF)
if(PCTOOL_BUILD_OCR_BENCHMARK)
    set_property(SOURCE src/ocr/paddle_ocr.cpp tests/ocr_benchmark.cpp tests/capture_integration.cpp APPEND PROPERTY OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/ocr/ocr_evaluation.h")
    if(TARGET PcToolCaptureIntegration)
        target_compile_definitions(PcToolCaptureIntegration PRIVATE PCTOOL_OCR_EVALUATION)
        target_link_libraries(PcToolCaptureIntegration PRIVATE Psapi)
    endif()
    add_executable(PcToolOcrBenchmark tests/ocr_benchmark.cpp src/ocr/paddle_ocr.cpp src/ocr/ocr_layout.cpp src/shared/image/image_document.cpp src/long_capture/scroll_stitcher.cpp src/ocr/ocr_text_merge.cpp)
    target_include_directories(PcToolOcrBenchmark PRIVATE src third_party/ocr/include)
    target_compile_features(PcToolOcrBenchmark PRIVATE cxx_std_17)
    target_compile_definitions(PcToolOcrBenchmark PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN PCTOOL_OCR_EVALUATION)
    target_link_libraries(PcToolOcrBenchmark PRIVATE Psapi)
    if(MSVC)
        target_compile_options(PcToolOcrBenchmark PRIVATE /W4 /utf-8 /EHsc)
    endif()
    add_custom_command(TARGET PcToolOcrBenchmark POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ocr/onnxruntime.dll" "$<TARGET_FILE_DIR:PcToolOcrBenchmark>/onnxruntime.dll")
    if(BUILD_TESTING)
        find_package(Python3 COMPONENTS Interpreter REQUIRED)
        add_test(NAME ocr_evaluation_failures COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/ocr_evaluation_tests.py" "$<TARGET_FILE:PcToolOcrBenchmark>")
        set_tests_properties(ocr_evaluation_failures PROPERTIES TIMEOUT 30)
    endif()
endif()
foreach(ocr_dependency Include/onnxruntime_cxx_api.h onnxruntime.dll models/det.onnx models/rec.onnx models/characters.txt models/MODEL_VERSION.txt)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ocr/${ocr_dependency}")
        message(FATAL_ERROR "Offline ocr dependency missing: ${ocr_dependency}. run powershell -ExecutionPolicy Bypass -file scripts/setup-ocr.ps1 before configuring.")
    endif()
endforeach()
foreach(ocr_target PcTool PcToolCaptureIntegration PcToolTranslationTests)
    if(TARGET ${ocr_target})
        target_include_directories(${ocr_target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ocr/include")
        add_custom_command(TARGET ${ocr_target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ocr/onnxruntime.dll" "$<TARGET_FILE_DIR:${ocr_target}>/onnxruntime.dll"
            COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ocr/models" "$<TARGET_FILE_DIR:${ocr_target}>/ocr_models"
            COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ocr/licenses" "$<TARGET_FILE_DIR:${ocr_target}>/ocr_licenses")
    endif()
endforeach()







include(cmake/PcToolInstall.cmake)
if(BUILD_TESTING)
    add_test(NAME module_boundaries COMMAND powershell -NoProfile -ExecutionPolicy Bypass -file "${CMAKE_SOURCE_DIR}/tests/module_boundaries.ps1")
    add_test(NAME archive_integration COMMAND powershell -NoProfile -ExecutionPolicy Bypass -file "${CMAKE_SOURCE_DIR}/tests/archive_integration.ps1" -runtime "${PCTOOL_ARCHIVE_RUNTIME}")
    add_executable(PcToolArchiveShellTests tests/archive_shell_tests.cpp)
    target_compile_features(PcToolArchiveShellTests PRIVATE cxx_std_17)
    target_compile_definitions(PcToolArchiveShellTests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(PcToolArchiveShellTests PRIVATE Ole32 Shell32 uuid)
    if(MSVC)
        target_compile_options(PcToolArchiveShellTests PRIVATE /W4 /utf-8 /EHsc)
    endif()
    add_test(NAME archive_shell COMMAND PcToolArchiveShellTests "${PCTOOL_ARCHIVE_RUNTIME}")
endif()

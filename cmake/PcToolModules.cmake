# Feature source inventories. App coordinates modules; shared contains no feature windows.
set(PCTOOL_CPP_SOURCES)
set(PCTOOL_LOCAL_HEADERS)
list(APPEND PCTOOL_LOCAL_HEADERS src/shared/platform/app_storage.h)
list(APPEND PCTOOL_CPP_SOURCES src/hotkeys/hotkey_settings.cpp)
list(APPEND PCTOOL_LOCAL_HEADERS src/hotkeys/hotkey_settings.h src/shared/platform/hotkey_binding.h)
set(PCTOOL_TRANSLATION_SOURCES
    src/translation/source_config.cpp
    src/translation/source_provider.cpp
    src/translation/source_settings.cpp
    src/translation/source_confirmation.cpp
    src/translation/source_icons.cpp
    src/translation/translation_input_watch.cpp
    src/translation/translation_controller.cpp
    src/translation/translation_capture.cpp
)
list(APPEND PCTOOL_CPP_SOURCES ${PCTOOL_TRANSLATION_SOURCES})
list(APPEND PCTOOL_LOCAL_HEADERS
    src/translation/source_config.h
    src/translation/source_provider.h
    src/translation/source_settings.h
    src/translation/source_confirmation.h
    src/translation/source_icons.h
    src/translation/translation_input_watch.h
    src/translation/translation_controller.h
    src/translation/translation_capture.h
    src/translation/translation_view.h
    src/translation/translation_submission.h
    src/translation/translation_provider.h
)
set(PCTOOL_APP_SOURCES
    src/app/app_settings.cpp
    src/app/capture_tools.cpp
    src/app/main.cpp
)
list(APPEND PCTOOL_CPP_SOURCES ${PCTOOL_APP_SOURCES})
list(APPEND PCTOOL_LOCAL_HEADERS
    src/app/app_messages.h
    src/app/app_settings.h
    src/app/capture_tools.h
)

set(PCTOOL_ARCHIVE_SOURCES
    src/archive/archive_service.cpp
)
list(APPEND PCTOOL_CPP_SOURCES ${PCTOOL_ARCHIVE_SOURCES})
list(APPEND PCTOOL_LOCAL_HEADERS
    src/archive/archive_service.h
)

set(PCTOOL_CAPTURE_SOURCES
    src/capture/image_editor.cpp
    src/capture/screenshot_overlay.cpp
)
list(APPEND PCTOOL_CPP_SOURCES ${PCTOOL_CAPTURE_SOURCES})
list(APPEND PCTOOL_LOCAL_HEADERS
    src/capture/image_editor.h
    src/capture/screenshot_overlay.h
)

set(PCTOOL_CLIPBOARD_SOURCES
    src/clipboard/clipboard_history.cpp
)
list(APPEND PCTOOL_CPP_SOURCES ${PCTOOL_CLIPBOARD_SOURCES})
list(APPEND PCTOOL_LOCAL_HEADERS
    src/clipboard/clipboard_history.h
)

set(PCTOOL_LONG_CAPTURE_SOURCES
    src/long_capture/long_capture_session.cpp
    src/long_capture/scroll_stitcher.cpp
)
list(APPEND PCTOOL_CPP_SOURCES ${PCTOOL_LONG_CAPTURE_SOURCES})
list(APPEND PCTOOL_LOCAL_HEADERS
    src/long_capture/long_capture_session.h
    src/long_capture/scroll_stitcher.h
    src/long_capture/scroll_annotation_mapping.h
)

set(PCTOOL_MONITOR_SOURCES
    src/monitor/system_monitor.cpp
    src/monitor/taskbar_monitor_window.cpp
)
list(APPEND PCTOOL_CPP_SOURCES ${PCTOOL_MONITOR_SOURCES})
list(APPEND PCTOOL_LOCAL_HEADERS
    src/monitor/system_monitor.h
    src/monitor/monitor_trace.h
    src/monitor/taskbar_monitor_window.h
)

set(PCTOOL_OCR_SOURCES
    src/ocr/ocr_layout.cpp
    src/ocr/ocr_service.cpp
    src/ocr/ocr_text_merge.cpp
    src/ocr/ocr_window.cpp
    src/ocr/paddle_ocr.cpp
)
list(APPEND PCTOOL_CPP_SOURCES ${PCTOOL_OCR_SOURCES})
list(APPEND PCTOOL_LOCAL_HEADERS
    src/ocr/ocr_evaluation.h
    src/ocr/ocr_service.h
    src/ocr/ocr_text_merge.h
    src/ocr/ocr_window.h
)

set(PCTOOL_RECORDING_SOURCES
    src/recording/recording_preview.cpp
    src/recording/recording_session.cpp
    src/recording/screen_recorder.cpp
    src/recording/gif_recording.cpp
    src/recording/gif_preview.cpp
)
list(APPEND PCTOOL_CPP_SOURCES ${PCTOOL_RECORDING_SOURCES})
list(APPEND PCTOOL_LOCAL_HEADERS
    src/recording/recording_preview.h
    src/recording/recording_session.h
    src/recording/recording_timeline.h
    src/recording/screen_recorder.h
    src/recording/gif_recording.h
)

set(PCTOOL_SHARED_SOURCES
    src/shared/ui/settings_frame.cpp
    src/shared/ui/confirmation.cpp
    src/shared/ui/popup_controls.cpp
    src/shared/annotation/annotation_painter.cpp
    src/shared/annotation/annotation_renderer.cpp
    src/shared/annotation/annotation_style.cpp
    src/shared/image/image_document.cpp
    src/shared/platform/capture_platform.cpp
    src/shared/ui/capture_ui.cpp
    src/shared/ui/toolbar_icons.cpp
)
list(APPEND PCTOOL_CPP_SOURCES ${PCTOOL_SHARED_SOURCES})
list(APPEND PCTOOL_LOCAL_HEADERS
    src/shared/platform/menu_source.h
    src/shared/ui/settings_frame.h
    src/shared/ui/confirmation.h
    src/shared/annotation/annotation_painter.h
    src/shared/annotation/annotation_fragments.h
    src/shared/annotation/annotation_renderer.h
    src/shared/annotation/annotation_style.h
    src/shared/annotation/text_layout.h
    src/shared/async/cancellation.h
    src/shared/image/image_document.h
    src/shared/platform/capture_platform.h
    src/shared/ui/capture_ui.h
    src/shared/ui/resource.h
    src/shared/ui/tray_icon.h
    src/shared/ui/popup_controls.h
    src/shared/ui/toolbar_icons.h
)

set(PCTOOL_SYSTEM_TOOLS_SOURCES
    src/system_tools/paint.cpp
)
list(APPEND PCTOOL_CPP_SOURCES ${PCTOOL_SYSTEM_TOOLS_SOURCES})
list(APPEND PCTOOL_LOCAL_HEADERS
    src/system_tools/paint.h
    src/system_tools/task_manager.h
)


list(APPEND PCTOOL_LOCAL_HEADERS src/shared/ui/capture_overlay_ui.h)

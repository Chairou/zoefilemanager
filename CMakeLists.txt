cmake_minimum_required(VERSION 3.22)
project(ZoeFileManager VERSION 0.8.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets DBus)

# libssh2: powers the real SSH/SFTP login in RemoteDialog / SftpClient.
# Homebrew on Apple Silicon ships headers/libs under /opt/homebrew.
find_path(LIBSSH2_INCLUDE_DIR NAMES libssh2.h
    HINTS /opt/homebrew/include /usr/local/include /usr/include)
find_library(LIBSSH2_LIBRARY NAMES ssh2 libssh2
    HINTS /opt/homebrew/lib /usr/local/lib /usr/lib)
if(NOT LIBSSH2_INCLUDE_DIR OR NOT LIBSSH2_LIBRARY)
    message(FATAL_ERROR
        "libssh2 not found. Install it first, e.g. `brew install libssh2` on macOS.")
endif()
message(STATUS "libssh2: ${LIBSSH2_LIBRARY} (headers: ${LIBSSH2_INCLUDE_DIR})")

# libsmbclient: drives the SMB/Samba browsing in RemoteDialog / SmbClient.
# On Apple Silicon Homebrew it lives under /opt/homebrew/{include,lib}.
find_path(LIBSMBCLIENT_INCLUDE_DIR NAMES libsmbclient.h
    HINTS /opt/homebrew/include /usr/local/include /usr/include)
find_library(LIBSMBCLIENT_LIBRARY NAMES smbclient libsmbclient
    HINTS /opt/homebrew/lib /usr/local/lib /usr/lib)
if(NOT LIBSMBCLIENT_INCLUDE_DIR OR NOT LIBSMBCLIENT_LIBRARY)
    message(FATAL_ERROR
        "libsmbclient not found. Install it first, e.g. `brew install samba` on macOS.")
endif()
message(STATUS "libsmbclient: ${LIBSMBCLIENT_LIBRARY} (headers: ${LIBSMBCLIENT_INCLUDE_DIR})")

set(SOURCES
    src/main.cpp
    src/MainWindow.cpp
    src/FilePanel.cpp
    src/DirectoryTree.cpp
    src/FileListView.cpp
    src/PathBar.cpp
    src/SearchDialog.cpp
    src/RemoteDialog.cpp
    src/ScriptDialog.cpp
    src/RealFileSystem.cpp
    src/ShortcutBar.cpp
    src/SftpClient.cpp
    src/FileSystemRouter.cpp
    src/AboutDialog.cpp
    src/SettingsDialog.cpp
    src/I18n.cpp
    src/AppFonts.cpp
    src/SmbClient.cpp
)

set(HEADERS
    src/MainWindow.h
    src/FilePanel.h
    src/DirectoryTree.h
    src/FileListView.h
    src/PathBar.h
    src/SearchDialog.h
    src/RemoteDialog.h
    src/ScriptDialog.h
    src/RealFileSystem.h
    src/ShortcutBar.h
    src/SftpClient.h
    src/FileSystemRouter.h
    src/AboutDialog.h
    src/SettingsDialog.h
    src/I18n.h
    src/AppFonts.h
    src/SmbClient.h
    src/Types.h
)

# ---- macOS: build as a proper .app bundle so launching from Finder /
# Spotlight / Dock does NOT spawn a Terminal window. WIN32 makes the
# Windows build a GUI subsystem app (no console window).

# macOS app icon
set(MACOSX_ICON "${CMAKE_CURRENT_SOURCE_DIR}/icon/ZoeFileManager.icns")

add_executable(${PROJECT_NAME} MACOSX_BUNDLE WIN32 ${SOURCES} ${HEADERS} ${MACOSX_ICON})

if(APPLE)
    set_source_files_properties(${MACOSX_ICON} PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources"
    )
    set_target_properties(${PROJECT_NAME} PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Zoe File Manager"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.workbuddy.ZoeFileManager"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_COPYRIGHT "Copyright (c) 2026 WorkBuddy"
        MACOSX_BUNDLE_ICON_FILE "ZoeFileManager.icns"
    )

    # Teach the runtime linker to look inside the bundle in ADDITION to the
    # Qt build-time rpath. We APPEND rather than replace so macdeployqt can
    # still resolve @rpath/Qt*.framework references during its own rewrite
    # pass (it reads the binary's existing rpaths to find Qt).
    set_property(TARGET ${PROJECT_NAME} APPEND PROPERTY
        INSTALL_RPATH "@executable_path/../Frameworks")

    # ALSO emit a flat copy of the binary at <build>/ZoeFileManager —
    # convenient for command-line invocation, scripts, packaging, etc.
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
            "$<TARGET_FILE:${PROJECT_NAME}>"
            "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}"
        COMMENT "Copying flat binary to ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}"
    )

    # ---- Bundle the third-party license texts under Contents/Resources/licenses/
    # The About dialog (Help → About) reads this directory so it can show
    # full LGPL / libssh2 / OpenSSL license text inline. Required by LGPLv3
    # to ship the license alongside dynamically-linked Qt frameworks.
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/licenses"
            "$<TARGET_BUNDLE_CONTENT_DIR:${PROJECT_NAME}>/Resources/licenses"
        COMMENT "Bundling third-party license texts into the .app"
        VERBATIM
    )

    # ---- Self-contained .app bundle (no Homebrew required at runtime) ----
    # 1) macdeployqt copies Qt frameworks, platform plugins, image-format
    #    plugins and rewrites install_names to @executable_path/../Frameworks.
    # 2) A small cmake -P script then vendor-copies the non-Qt dylibs Qt
    #    does not know about (libssh2 + its libssl/libcrypto dependencies)
    #    and rewrites their install names to the same @rpath scheme.
    # Toggle off with -DDEPLOY_QT=OFF if you want a "thin" build for dev.
    option(DEPLOY_QT "Embed Qt frameworks and deps into the .app bundle" ON)

    if(DEPLOY_QT)
        # macdeployqt lives next to qmake in the Qt install.
        get_target_property(_qt_qmake_exe Qt6::qmake IMPORTED_LOCATION)
        get_filename_component(_qt_bin_dir "${_qt_qmake_exe}" DIRECTORY)
        find_program(MACDEPLOYQT_EXECUTABLE macdeployqt
            HINTS "${_qt_bin_dir}" /opt/homebrew/bin /usr/local/bin)

        if(NOT MACDEPLOYQT_EXECUTABLE)
            message(WARNING
                "macdeployqt not found — the .app will still depend on Homebrew Qt at runtime. "
                "Install full Qt (brew install qt) to enable self-contained bundling.")
        else()
            message(STATUS "macdeployqt: ${MACDEPLOYQT_EXECUTABLE}")
            set(_deploy_script "${CMAKE_CURRENT_SOURCE_DIR}/cmake/deploy_macos.cmake")

            # Run macdeployqt first, then the vendor-copy script.
            #
            # Notes on flags:
            #   -no-strip   : keep symbols (faster, and avoids extra mutation
            #                 that would invalidate signatures we redo later).
            #   -codesign=- : we DO NOT let macdeployqt sign anything. It
            #                 self-invalidates signatures by patching dylibs
            #                 after signing them (e.g. libbrotlicommon), and
            #                 it then fails its own --verify check. Our
            #                 deploy_macos.cmake script does the final
            #                 ad-hoc signing once everything is in place.
            #
            # We invoke macdeployqt via `sh -c "... ; true"` so that benign
            # ERROR lines about optional Qt modules NOT used by this app
            # (QtPdf, QtVirtualKeyboard, etc. dragged in by plugin scans)
            # never abort the whole build.
            #
            # macdeployqt in brew-Qt 6.x doesn't expose -exclude-plugins, so we:
            #   (a) filter known-benign rpath ERRORs from its stderr to keep
            #       the build log clean, and
            #   (b) right after macdeployqt finishes, delete the unused
            #       plugins it copied in (libqpdf, libqtvirtualkeyboardplugin)
            #       so the bundle doesn't carry dead-link dylibs at runtime.
            #
            # On big bundles (SMB pulls in ~100 samba private dylibs) this
            # step can run for 2-5 minutes. Without live output users
            # often think the build has hung. We therefore:
            #   - run macdeployqt with stdbuf -oL -eL (line-buffered) so
            #     no chunk gets stuck inside libc's stdio buffer,
            #   - tee the full output to build-dir/macdeployqt.log
            #     (always available for `tail -f`),
            #   - print a 1-line heartbeat every 5s while it runs, so the
            #     terminal NEVER goes silent for more than 5s,
            #   - filter only the terminal display, never the log file,
            #   - skip macdeployqt entirely on incremental rebuilds via
            #     a sentinel file inside the bundle (delete the bundle
            #     or the sentinel to force a full re-deploy).
            set(_macdeploy_log "${CMAKE_BINARY_DIR}/macdeployqt.log")
            add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
                COMMAND sh -c
                    "BUNDLE='$<TARGET_BUNDLE_DIR:${PROJECT_NAME}>'; SENTINEL=\"$BUNDLE/Contents/.macdeployqt.done\"; if [ -f \"$SENTINEL\" ]; then echo \"macdeployqt: bundle already deployed (sentinel: $SENTINEL) — skipping. Delete the bundle or that file to force a full re-deploy.\"; exit 0; fi; LOG='${_macdeploy_log}'; FW=\"$BUNDLE/Contents/Frameworks\"; SB=$(command -v stdbuf || command -v gstdbuf || true); SBARGS=''; [ -n \"$SB\" ] && SBARGS='-oL -eL'; echo \"Running macdeployqt (first time can take 2-5 min on a big bundle; full log: $LOG)\"; : > \"$LOG\"; ( $SB $SBARGS '${MACDEPLOYQT_EXECUTABLE}' \"$BUNDLE\" -verbose=1 -no-strip '-codesign=-' 2>&1 || echo \"macdeployqt reported errors; continuing — final sign happens in deploy_macos.cmake\" ) | tee \"$LOG\" | grep --line-buffered -v -E 'Cannot resolve rpath \"@rpath/Qt[A-Za-z]+\\.framework' | grep --line-buffered -v -E 'Cannot resolve rpath \"@rpath/lib[^\"]+\\.dylib\"' | grep --line-buffered -v -E '^ERROR:  using QList\\(\"' | grep --line-buffered -v -E '^ERROR: \"error: .*otool-classic: can'\\''t open file' | grep --line-buffered -v -E '^ERROR: Codesign signing error:' | grep --line-buffered -v -E '^ERROR: \"/.*\\.dylib: No such file or directory' | grep --line-buffered -v -E 'install_name_tool: warning: changes being made to the file will invalidate the code signature' & MDPID=$!; while kill -0 $MDPID 2>/dev/null; do sleep 5; n=$(ls \"$FW\" 2>/dev/null | wc -l | tr -d ' '); printf '  ... macdeployqt working: %s frameworks/dylibs in bundle so far\\n' \"$n\"; done; wait $MDPID; rm -f \"$BUNDLE/Contents/PlugIns/imageformats/libqpdf.dylib\" \"$BUNDLE/Contents/PlugIns/platforminputcontexts/libqtvirtualkeyboardplugin.dylib\"; rmdir \"$BUNDLE/Contents/PlugIns/platforminputcontexts\" 2>/dev/null; echo 'Pruned unused Qt plugins (libqpdf, libqtvirtualkeyboardplugin)'; true"
                COMMAND ${CMAKE_COMMAND}
                    -DBUNDLE=$<TARGET_BUNDLE_DIR:${PROJECT_NAME}>
                    -DLIBSSH2_SRC=${LIBSSH2_LIBRARY}
                    -P "${_deploy_script}"
                COMMENT "Embedding Qt frameworks + libssh2 into the .app bundle"
                VERBATIM
            )
        endif()
    endif()
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE
    Qt6::Widgets
    ${LIBSSH2_LIBRARY}
    ${LIBSMBCLIENT_LIBRARY}
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${LIBSSH2_INCLUDE_DIR}
    ${LIBSMBCLIENT_INCLUDE_DIR}
)

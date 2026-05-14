# deploy_macos.cmake  - invoked via `cmake -P` from POST_BUILD
#
# Inputs (via -D):
#   BUNDLE        : absolute path to the .app bundle (ending in .app)
#   LIBSSH2_SRC   : source path of libssh2 dylib that was linked against
#
# What it does:
#   1. Ensures Contents/Frameworks exists.
#   2. Copies libssh2 + its non-system dependencies (libssl.3, libcrypto.3)
#      into the bundle, dereferencing symlinks so we store the real dylib.
#   3. Rewrites install_names so the bundle is fully self-contained:
#      - each vendored dylib gets id "@rpath/<soname>"
#      - every reference from the main binary and from each vendored dylib
#        to an absolute Homebrew path is rewritten to "@rpath/<soname>"
#   4. Leaves /usr/lib/* and /System/Library/* references alone (those are
#      always available on macOS).
#
# macdeployqt has already been run by this point, so Qt frameworks are
# already embedded and their install_names already point at
# @rpath/Qt*.framework/... — we only need to handle the non-Qt libs.

cmake_minimum_required(VERSION 3.22)

if(NOT BUNDLE OR NOT LIBSSH2_SRC)
    message(FATAL_ERROR "deploy_macos.cmake: BUNDLE and LIBSSH2_SRC must be set via -D")
endif()

set(FRAMEWORKS_DIR "${BUNDLE}/Contents/Frameworks")
file(MAKE_DIRECTORY "${FRAMEWORKS_DIR}")

# ---- Homebrew dylib search paths --------------------------------------
# Samba ships many "private" dylibs under <prefix>/Cellar/samba/<ver>/lib/private/
# that are NOT on the standard linker search path, but libsmbclient
# hard-codes references to them via @executable_path/../Frameworks/...
# We need to be able to locate them on disk so we can vendor them in.
#
# Krb5 / tdb / etc. live under their own Cellar dirs too. We GLOB the
# version directory so a Homebrew upgrade (samba 4.21 -> 4.22, etc.)
# doesn't silently break the deploy.
set(_HB_SEARCH_PATHS
    "/opt/homebrew/lib"
    "/opt/homebrew/opt/samba/lib"
    "/opt/homebrew/opt/samba/lib/private"
    "/opt/homebrew/opt/krb5/lib"
    "/opt/homebrew/opt/tdb/lib"
    "/opt/homebrew/opt/talloc/lib"
    "/opt/homebrew/opt/tevent/lib"
    "/opt/homebrew/opt/ldb/lib"
    "/opt/homebrew/opt/openssl@3/lib"
    "/opt/homebrew/opt/gnutls/lib"
    "/opt/homebrew/opt/icu4c@76/lib"
    "/opt/homebrew/opt/icu4c/lib"
    "/usr/local/lib"
)
# Add versioned Cellar dirs (samba/lib + samba/lib/private, krb5/lib, tdb/lib).
file(GLOB _hb_samba_dirs    "/opt/homebrew/Cellar/samba/*/lib")
file(GLOB _hb_samba_priv    "/opt/homebrew/Cellar/samba/*/lib/private")
file(GLOB _hb_krb5_dirs     "/opt/homebrew/Cellar/krb5/*/lib")
file(GLOB _hb_tdb_dirs      "/opt/homebrew/Cellar/tdb/*/lib")
file(GLOB _hb_talloc_dirs   "/opt/homebrew/Cellar/talloc/*/lib")
file(GLOB _hb_tevent_dirs   "/opt/homebrew/Cellar/tevent/*/lib")
file(GLOB _hb_ldb_dirs      "/opt/homebrew/Cellar/ldb/*/lib")
file(GLOB _hb_icu_dirs      "/opt/homebrew/Cellar/icu4c@*/*/lib" "/opt/homebrew/Cellar/icu4c/*/lib")
# Final fallback: every Cellar/*/lib  (covers any keg-only formula such as
# icu4c@76, openssl@3, sqlite, etc., even on a different Homebrew prefix)
file(GLOB _hb_cellar_all    "/opt/homebrew/Cellar/*/*/lib")
list(APPEND _HB_SEARCH_PATHS
    ${_hb_samba_dirs} ${_hb_samba_priv}
    ${_hb_krb5_dirs} ${_hb_tdb_dirs}
    ${_hb_talloc_dirs} ${_hb_tevent_dirs} ${_hb_ldb_dirs}
    ${_hb_icu_dirs}
    ${_hb_cellar_all})

# Helper: find <name> on disk in our Homebrew search paths.
function(_find_dylib_in_homebrew name out_path)
    set(${out_path} "" PARENT_SCOPE)
    foreach(_d IN LISTS _HB_SEARCH_PATHS)
        if(EXISTS "${_d}/${name}")
            set(${out_path} "${_d}/${name}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
endfunction()

# Pick an executable inside the bundle — it's the only "main" Mach-O we
# need to rewrite references on.
get_filename_component(_app_name "${BUNDLE}" NAME_WE)
set(APP_BINARY "${BUNDLE}/Contents/MacOS/${_app_name}")
if(NOT EXISTS "${APP_BINARY}")
    message(FATAL_ERROR "deploy_macos.cmake: app binary not found: ${APP_BINARY}")
endif()

# Helper: given an input dylib path, resolve symlinks and pick a stable
# soname we'll store inside the bundle (e.g. libssh2.1.dylib).
function(_resolve_soname in_path out_real out_soname)
    get_filename_component(_real "${in_path}" REALPATH)
    set(${out_real} "${_real}" PARENT_SCOPE)
    # Ask otool what the *ID* install_name is - that's the canonical name
    # other libs will refer to it by.
    execute_process(COMMAND otool -D "${_real}"
        OUTPUT_VARIABLE _out OUTPUT_STRIP_TRAILING_WHITESPACE)
    # otool -D prints two lines: "<path>:" then "<id>". Take line 2.
    string(REPLACE "\n" ";" _lines "${_out}")
    list(LENGTH _lines _n)
    if(_n GREATER_EQUAL 2)
        list(GET _lines 1 _id)
    else()
        set(_id "${_real}")
    endif()
    get_filename_component(_soname "${_id}" NAME)
    set(${out_soname} "${_soname}" PARENT_SCOPE)
endfunction()

# Helper: copy a dylib (dereferencing symlinks) into Contents/Frameworks
# and return the destination path + its soname.
function(_vendor_dylib in_path out_dest out_soname)
    _resolve_soname("${in_path}" _real _soname)
    set(_dest "${FRAMEWORKS_DIR}/${_soname}")
    if(NOT EXISTS "${_dest}")
        message(STATUS "  vendor: ${_real}  ->  Contents/Frameworks/${_soname}")
        file(COPY "${_real}" DESTINATION "${FRAMEWORKS_DIR}")
        # file(COPY) preserves the source filename - rename to the soname
        # if they differ (e.g. libssh2.1.1.0.dylib -> libssh2.1.dylib).
        get_filename_component(_copied_name "${_real}" NAME)
        if(NOT _copied_name STREQUAL _soname)
            file(RENAME "${FRAMEWORKS_DIR}/${_copied_name}" "${_dest}")
        endif()
        execute_process(COMMAND chmod u+w "${_dest}")
        execute_process(COMMAND install_name_tool -id "@rpath/${_soname}" "${_dest}")
    endif()
    set(${out_dest} "${_dest}" PARENT_SCOPE)
    set(${out_soname} "${_soname}" PARENT_SCOPE)
endfunction()

# Helper: for every non-system dependency of <dylib>, make sure the
# dependency is vendored too and rewrite <dylib>'s reference to @rpath/...
function(_rewrite_non_system_deps dylib)
    # Guard against cycles in the dep graph (samba private libs do form
    # cycles, e.g. libreplace <-> libsamba-util). We track real, fully-
    # resolved paths in a GLOBAL property so recursion only walks each
    # dylib once per cmake -P invocation.
    get_filename_component(_real_dylib "${dylib}" REALPATH)
    get_property(_visited GLOBAL PROPERTY _DEPLOY_VISITED_DYLIBS)
    if(_visited)
        list(FIND _visited "${_real_dylib}" _idx)
        if(NOT _idx EQUAL -1)
            return()
        endif()
    endif()
    set_property(GLOBAL APPEND PROPERTY _DEPLOY_VISITED_DYLIBS "${_real_dylib}")

    execute_process(COMMAND otool -L "${dylib}"
        OUTPUT_VARIABLE _out OUTPUT_STRIP_TRAILING_WHITESPACE)
    string(REPLACE "\n" ";" _lines "${_out}")
    foreach(_line IN LISTS _lines)
        string(STRIP "${_line}" _line)
        # Lines of interest look like: "<path> (compatibility version ...)"
        string(REGEX MATCH "^([^ \t][^ \t]*)[ \t]+\\(compatibility version" _m "${_line}")
        if(NOT CMAKE_MATCH_1)
            continue()
        endif()
        set(_dep "${CMAKE_MATCH_1}")
        # Skip self (first entry for a dylib is its own id).
        get_filename_component(_dep_name "${_dep}" NAME)
        get_filename_component(_self_name "${dylib}" NAME)
        if(_dep_name STREQUAL _self_name AND _dep MATCHES "^@")
            continue()
        endif()
        # Leave system libs alone.
        if(_dep MATCHES "^/usr/lib/" OR _dep MATCHES "^/System/")
            continue()
        endif()
        # If the ref is already in @-form, the standard assumption is "it's
        # already in the bundle", but Samba's libsmbclient hard-codes
        # @executable_path/../Frameworks/<libfoo>.dylib for many private
        # dylibs that live under Cellar/samba/<ver>/lib/private/ and are
        # NOT picked up automatically. So: verify the target actually
        # exists in Contents/Frameworks; if it does, skip; if not, hunt
        # it down in Homebrew, vendor it, and rewrite this ref to
        # @rpath/<soname> (canonical form) so everything lines up.
        if(_dep MATCHES "^@rpath/" OR _dep MATCHES "^@executable_path/" OR
           _dep MATCHES "^@loader_path/")
            # Special-case framework references like
            # @rpath/QtCore.framework/Versions/A/QtCore: extract the
            # framework path from the @-prefix and check if the framework
            # is in the bundle. If yes, skip. If no, also skip (we cannot
            # easily vendor a framework from Homebrew here — macdeployqt
            # owns that, and missing Qt frameworks are a separate bug).
            string(REGEX MATCH "([^/]+\\.framework/.+)$" _fwmatch "${_dep}")
            if(CMAKE_MATCH_1 AND EXISTS "${FRAMEWORKS_DIR}/${CMAKE_MATCH_1}")
                # Framework binary is in the bundle — nothing to do.
                continue()
            endif()
            if(EXISTS "${FRAMEWORKS_DIR}/${_dep_name}")
                # Already vendored — but make sure the ref is on the
                # canonical @rpath/<soname> form to keep things uniform.
                if(NOT _dep STREQUAL "@rpath/${_dep_name}")
                    execute_process(COMMAND install_name_tool -change
                        "${_dep}" "@rpath/${_dep_name}" "${dylib}")
                endif()
                continue()
            endif()
            # If this is a framework binary ref but the framework is NOT
            # in the bundle, we cannot rescue it from a flat dylib search
            # — just warn and move on.
            if(CMAKE_MATCH_1)
                message(WARNING
                    "deploy_macos.cmake: framework dependency '${CMAKE_MATCH_1}' "
                    "referenced by '${_self_name}' is NOT in the bundle. "
                    "macdeployqt should have copied it; please re-run with a "
                    "clean bundle.")
                continue()
            endif()
            # Missing — try to find it in Homebrew.
            _find_dylib_in_homebrew("${_dep_name}" _hb_path)
            if(NOT _hb_path)
                message(WARNING
                    "deploy_macos.cmake: dependency '${_dep_name}' referenced by "
                    "'${_self_name}' as '${_dep}' is NOT in the bundle and was "
                    "NOT found in Homebrew search paths. The .app may fail to "
                    "launch.")
                continue()
            endif()
            message(STATUS "    rescue: ${_self_name} needs ${_dep_name} (was: ${_dep}) -> vendor from ${_hb_path}")
            _vendor_dylib("${_hb_path}" _dep_dest _dep_soname)
            execute_process(COMMAND install_name_tool -change
                "${_dep}" "@rpath/${_dep_soname}" "${dylib}")
            _rewrite_non_system_deps("${_dep_dest}")
            continue()
        endif()
        # Non-system dep - vendor it (recursively) and rewrite the ref.
        _vendor_dylib("${_dep}" _dep_dest _dep_soname)
        message(STATUS "    patch: ${_self_name}  ${_dep}  ->  @rpath/${_dep_soname}")
        execute_process(COMMAND install_name_tool -change
            "${_dep}" "@rpath/${_dep_soname}" "${dylib}")
        # Recurse - but only after the copy exists.
        _rewrite_non_system_deps("${_dep_dest}")
    endforeach()
endfunction()

message(STATUS "Deploy (vendor non-Qt dylibs) -> ${BUNDLE}")

# ---- 1) Vendor libssh2 itself. ----
_vendor_dylib("${LIBSSH2_SRC}" SSH2_DEST SSH2_SONAME)

# ---- 1.5) Vendor LLVM libc++ dependencies if they exist in the bundle ----
# Check if libc++.1.dylib was already copied by macdeployqt
set(LLVM_CXX_DIR "/opt/homebrew/opt/llvm/lib/c++")
if(EXISTS "${LLVM_CXX_DIR}")
    file(GLOB _cxx_dylibs "${FRAMEWORKS_DIR}/libc++*.dylib")
    if(_cxx_dylibs)
        message(STATUS "Found LLVM libc++ in bundle, vendoring its dependencies...")
        
        # Vendor libunwind.1.dylib
        set(UNWIND_PATH "/opt/homebrew/Cellar/llvm/19.1.7_1/lib/unwind/libunwind.1.dylib")
        if(EXISTS "${UNWIND_PATH}")
            _vendor_dylib("${UNWIND_PATH}" UNWIND_DEST UNWIND_SONAME)
        endif()
        
        # Vendor libc++abi.1.dylib
        set(CXXABI_PATH "/opt/homebrew/Cellar/llvm/19.1.7_1/lib/c++/libc++abi.1.dylib")
        if(EXISTS "${CXXABI_PATH}")
            _vendor_dylib("${CXXABI_PATH}" CXXABI_DEST CXXABI_SONAME)
        endif()
    endif()
endif()

# ---- 2) Rewrite the app binary's reference to libssh2 -> @rpath/... ----
# We need to know *what string* the app currently uses to refer to libssh2;
# that's whatever the linker baked in (often /opt/homebrew/opt/libssh2/...).
execute_process(COMMAND otool -L "${APP_BINARY}"
    OUTPUT_VARIABLE _app_deps OUTPUT_STRIP_TRAILING_WHITESPACE)
string(REPLACE "\n" ";" _app_lines "${_app_deps}")
foreach(_line IN LISTS _app_lines)
    string(STRIP "${_line}" _line)
    string(REGEX MATCH "^([^ \t][^ \t]*)[ \t]+\\(compatibility version" _m "${_line}")
    if(NOT CMAKE_MATCH_1)
        continue()
    endif()
    set(_dep "${CMAKE_MATCH_1}")
    get_filename_component(_dep_name "${_dep}" NAME)
    if(_dep_name STREQUAL SSH2_SONAME AND NOT _dep MATCHES "^@")
        message(STATUS "  patch app: ${_dep}  ->  @rpath/${SSH2_SONAME}")
        execute_process(COMMAND install_name_tool -change
            "${_dep}" "@rpath/${SSH2_SONAME}" "${APP_BINARY}")
    endif()
endforeach()

# ---- 3) Recursively vendor + patch libssh2's own non-system deps
# (libssl.3, libcrypto.3) and keep chasing. ----
_rewrite_non_system_deps("${SSH2_DEST}")

# ---- 4) Belt-and-suspenders: ensure the app has an @rpath
# "@executable_path/../Frameworks" so @rpath/libfoo resolves. cmake's
# INSTALL_RPATH usually does this already, but add_rpath errors if
# duplicate — ignore failure. ----
execute_process(COMMAND install_name_tool -add_rpath
    "@executable_path/../Frameworks" "${APP_BINARY}"
    RESULT_VARIABLE _rc OUTPUT_QUIET ERROR_QUIET)

# ---- 4.5) Vendor QtDBus framework that macdeployqt misses ----
# QtGui links against @rpath/QtDBus.framework/Versions/A/QtDBus but
# macdeployqt does not always pull it in automatically.
set(_qtdbus_fw "/opt/homebrew/lib/QtDBus.framework")
if(EXISTS "${_qtdbus_fw}" AND NOT EXISTS "${FRAMEWORKS_DIR}/QtDBus.framework/Versions/A/QtDBus")
    message(STATUS "  vendor: QtDBus.framework -> Contents/Frameworks/QtDBus.framework")
    # Copy the framework structure (binary + Resources for Info.plist)
    file(MAKE_DIRECTORY "${FRAMEWORKS_DIR}/QtDBus.framework/Versions/A")
    file(COPY "${_qtdbus_fw}/Versions/A/QtDBus"
         DESTINATION "${FRAMEWORKS_DIR}/QtDBus.framework/Versions/A")
    if(EXISTS "${_qtdbus_fw}/Versions/A/Resources")
        file(COPY "${_qtdbus_fw}/Versions/A/Resources"
             DESTINATION "${FRAMEWORKS_DIR}/QtDBus.framework/Versions/A")
    endif()
    # Create standard framework symlinks
    execute_process(COMMAND ${CMAKE_COMMAND} -E create_symlink "A"
        "${FRAMEWORKS_DIR}/QtDBus.framework/Versions/Current")
    execute_process(COMMAND ${CMAKE_COMMAND} -E create_symlink "Versions/Current/QtDBus"
        "${FRAMEWORKS_DIR}/QtDBus.framework/QtDBus")
    execute_process(COMMAND ${CMAKE_COMMAND} -E create_symlink "Versions/Current/Resources"
        "${FRAMEWORKS_DIR}/QtDBus.framework/Resources")

    set(_qtdbus_bin "${FRAMEWORKS_DIR}/QtDBus.framework/Versions/A/QtDBus")
    execute_process(COMMAND chmod u+w "${_qtdbus_bin}")

    # Rewrite its install id
    execute_process(COMMAND install_name_tool -id
        "@rpath/QtDBus.framework/Versions/A/QtDBus" "${_qtdbus_bin}")

    # Rewrite its reference to QtCore (already in bundle)
    execute_process(COMMAND install_name_tool -change
        "@rpath/QtCore.framework/Versions/A/QtCore"
        "@executable_path/../Frameworks/QtCore.framework/Versions/A/QtCore"
        "${_qtdbus_bin}")

    # Rewrite its reference to the absolute Homebrew id to @rpath form
    execute_process(COMMAND install_name_tool -change
        "/opt/homebrew/opt/qt/lib/QtDBus.framework/Versions/A/QtDBus"
        "@rpath/QtDBus.framework/Versions/A/QtDBus"
        "${_qtdbus_bin}")

    # Vendor libdbus-1 if QtDBus depends on it
    set(_dbus_lib "/opt/homebrew/opt/dbus/lib/libdbus-1.3.dylib")
    if(EXISTS "${_dbus_lib}")
        _vendor_dylib("${_dbus_lib}" _dbus_dest _dbus_soname)
        execute_process(COMMAND install_name_tool -change
            "/opt/homebrew/opt/dbus/lib/libdbus-1.3.dylib"
            "@rpath/${_dbus_soname}"
            "${_qtdbus_bin}")
        _rewrite_non_system_deps("${_dbus_dest}")
    endif()

    # Add rpath to QtDBus binary so it can find sibling frameworks
    execute_process(COMMAND install_name_tool -add_rpath
        "@executable_path/../Frameworks" "${_qtdbus_bin}"
        RESULT_VARIABLE _rc OUTPUT_QUIET ERROR_QUIET)
endif()

# ---- 4.6) Sweep ALL vendored dylibs for non-system deps. -----------------
# macdeployqt copies third-party dylibs (e.g. libsmbclient + its samba
# private dylibs) into the bundle, but it does NOT recursively chase
# dependencies that are already expressed as @executable_path/...
# (Samba hard-codes those at link time). Some of those targets live
# under Cellar/samba/<ver>/lib/private/ and never made it into the
# bundle, causing dyld "Library not loaded" crashes at launch.
#
# Re-walk every dylib we have so far; for each missing dep we now know
# how to vendor from Homebrew (see _rewrite_non_system_deps + _HB_SEARCH_PATHS).
# Loop until a full pass adds no new file (handles deep dep chains).
message(STATUS "Sweeping all vendored dylibs for missing deps (samba private, krb5, tdb, ...)")
set(_sweep_pass 0)
while(_sweep_pass LESS 6)
    math(EXPR _sweep_pass "${_sweep_pass} + 1")
    # Reset the per-walk "visited" set so this pass re-examines every
    # dylib (the previous pass may have added new files we still need
    # to chase deps for).
    set_property(GLOBAL PROPERTY _DEPLOY_VISITED_DYLIBS "")
    file(GLOB _all_dylibs "${FRAMEWORKS_DIR}/*.dylib")
    # ALSO walk every Qt framework binary (e.g. QtCore.framework/Versions/A/QtCore).
    # Without this, dylibs hard-coded as @executable_path/../Frameworks/libfoo
    # by Qt itself (libglib, libicudata, libharfbuzz, libpng, libfreetype,
    # libmd4c, libb2, libdouble-conversion, libpcre2-16, libgthread, ...)
    # would never be rescued, since the sweep would only see standalone
    # dylibs and not the frameworks that pull them in.
    file(GLOB _fw_dirs LIST_DIRECTORIES TRUE "${FRAMEWORKS_DIR}/*.framework")
    set(_all_macho "${_all_dylibs}")
    foreach(_fw IN LISTS _fw_dirs)
        file(GLOB _fw_bins "${_fw}/Versions/*/Qt*")
        foreach(_b IN LISTS _fw_bins)
            if(NOT IS_SYMLINK "${_b}" AND NOT IS_DIRECTORY "${_b}")
                list(APPEND _all_macho "${_b}")
            endif()
        endforeach()
    endforeach()
    list(LENGTH _all_dylibs _before_count)
    foreach(_f IN LISTS _all_macho)
        if(NOT IS_SYMLINK "${_f}")
            execute_process(COMMAND chmod u+w "${_f}" OUTPUT_QUIET ERROR_QUIET)
            _rewrite_non_system_deps("${_f}")
        endif()
    endforeach()
    file(GLOB _all_dylibs_after "${FRAMEWORKS_DIR}/*.dylib")
    list(LENGTH _all_dylibs_after _after_count)
    message(STATUS "  sweep pass ${_sweep_pass}: ${_before_count} -> ${_after_count} dylibs in Frameworks/ (also walked Qt framework binaries)")
    if(_after_count EQUAL _before_count)
        break()
    endif()
endwhile()

# ---- 5) Re-sign everything we touched.  install_name_tool invalidates
# Mach-O code signatures (macdeployqt ad-hoc signs, we rewrite, now the
# hash no longer matches and the loader refuses to run on arm64).
# An ad-hoc signature ("-s -") is enough for local use; users who want
# to distribute the bundle will Developer-ID sign on top. ----
function(_resign path)
    execute_process(COMMAND codesign --force --sign - --timestamp=none
        --preserve-metadata=entitlements,requirements,flags "${path}"
        RESULT_VARIABLE _rc ERROR_VARIABLE _err)
    if(NOT _rc EQUAL 0)
        message(WARNING "codesign failed on ${path}: ${_err}")
    endif()
endfunction()

# Re-sign every dylib we touched, then all Qt frameworks macdeployqt
# embedded, then the platform plugins, and finally the app binary itself.
file(GLOB _vendored_dylibs "${FRAMEWORKS_DIR}/*.dylib")
foreach(_f IN LISTS _vendored_dylibs)
    _resign("${_f}")
endforeach()

# Sign Qt framework binaries (inside-out: binary first, then framework is valid)
file(GLOB _qt_fw_dirs LIST_DIRECTORIES TRUE "${FRAMEWORKS_DIR}/*.framework")
foreach(_fw IN LISTS _qt_fw_dirs)
    file(GLOB _fw_bins "${_fw}/Versions/*/Qt*")
    foreach(_b IN LISTS _fw_bins)
        if(NOT IS_SYMLINK "${_b}" AND NOT IS_DIRECTORY "${_b}")
            _resign("${_b}")
        endif()
    endforeach()
endforeach()

set(PLUGINS_DIR "${BUNDLE}/Contents/PlugIns")
if(EXISTS "${PLUGINS_DIR}")
    file(GLOB_RECURSE _plugin_dylibs "${PLUGINS_DIR}/*.dylib")
    foreach(_f IN LISTS _plugin_dylibs)
        _resign("${_f}")
    endforeach()
endif()

# The main binary MUST be signed last — after all its deps are final.
# Use --deep to ensure the entire bundle (including nested frameworks) is valid.
execute_process(COMMAND codesign --force --deep --sign - --timestamp=none
    --preserve-metadata=entitlements,requirements,flags "${BUNDLE}"
    RESULT_VARIABLE _rc ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
    message(WARNING "codesign --deep failed on ${BUNDLE}: ${_err}")
endif()

message(STATUS "Deploy finished.")
# SPDX-FileCopyrightText: (C) 2024 Chris Rizzitello <sithlord48@gmail.com>
# SPDX-License-Identifier: MIT

# HACK This is set when the files is included so its the real path
# calling CMAKE_CURRENT_LIST_DIR after include would return the wrong scope var
set(MY_DIR ${CMAKE_CURRENT_LIST_DIR})
set(OSX_BUNDLE ${BUILD_OSX_BUNDLE})

set(OS_STRING "macos-${BUILD_ARCHITECTURE}")

if (OSX_BUNDLE)
  # QtSvg is optional. Only framework targets need the plugin-only dependency
  # workaround; never infer a framework directory from a dylib or static archive.
  find_package(Qt6 ${REQUIRED_QT_VERSION} QUIET OPTIONAL_COMPONENTS Svg)
  set(_qt_svg_lib_dir "")
  set(_qt_svg_installed_binary "")
  if(TARGET Qt6::Svg)
    get_target_property(_qt_svg_binary Qt6::Svg LOCATION)
    file(REAL_PATH "${_qt_svg_binary}" _qt_svg_binary)
    get_filename_component(_qt_svg_lib_dir "${_qt_svg_binary}" DIRECTORY)
    get_target_property(_qt_svg_is_framework Qt6::Svg FRAMEWORK)
    if(_qt_svg_is_framework)
      if(NOT _qt_svg_binary MATCHES "^(.+\\.framework)/")
        message(FATAL_ERROR "Unrecognized QtSvg framework binary: ${_qt_svg_binary}")
      endif()
      set(_qt_svg_framework_dir "${CMAKE_MATCH_1}")
      get_filename_component(_qt_svg_framework_name "${_qt_svg_framework_dir}" NAME)
      get_filename_component(_qt_svg_lib_dir "${_qt_svg_framework_dir}" DIRECTORY)
      file(RELATIVE_PATH _qt_svg_framework_binary "${_qt_svg_framework_dir}" "${_qt_svg_binary}")
      set(_qt_svg_installed_binary
        "${CMAKE_PROJECT_PROPER_NAME}.app/Contents/Frameworks/${_qt_svg_framework_name}/${_qt_svg_framework_binary}")
      install(DIRECTORY "${_qt_svg_framework_dir}"
        DESTINATION "${CMAKE_PROJECT_PROPER_NAME}.app/Contents/Frameworks"
        PATTERN "Headers" EXCLUDE
        PATTERN "*.prl" EXCLUDE
      )
    endif()
  endif()
  set(_codesign_identity "-")
  set(_codesign_options "")
  if (APPLE_CODESIGN_DEV)
    set(_codesign_identity "${APPLE_CODESIGN_DEV}")
    set(_codesign_options "-hardened-runtime -timestamp")
  endif()
  install(CODE "
    set(_qt_deploy_args
      \"\${CMAKE_INSTALL_PREFIX}/${CMAKE_PROJECT_PROPER_NAME}.app\"
      \"-executable=\${CMAKE_INSTALL_PREFIX}/${CMAKE_PROJECT_PROPER_NAME}.app/Contents/MacOS/deskflow-core\")
    if(NOT \"${_qt_svg_lib_dir}\" STREQUAL \"\")
      list(APPEND _qt_deploy_args \"-libpath=${_qt_svg_lib_dir}\")
    endif()
    if(NOT \"${_qt_svg_installed_binary}\" STREQUAL \"\")
      list(APPEND _qt_deploy_args \"-executable=\${CMAKE_INSTALL_PREFIX}/${_qt_svg_installed_binary}\")
    endif()
    list(APPEND _qt_deploy_args ${_codesign_options} \"-codesign=${_codesign_identity}\")
    execute_process(COMMAND \"${DEPLOYQT}\" \${_qt_deploy_args} RESULT_VARIABLE _qt_deploy_result)
    if(NOT _qt_deploy_result EQUAL 0)
      message(FATAL_ERROR \"macdeployqt failed: \${_qt_deploy_result}\")
    endif()
  ")
  set(CPACK_PACKAGE_ICON "${MY_DIR}/dmg-volume.icns")
  set(CPACK_DMG_BACKGROUND_IMAGE "${MY_DIR}/dmg-background.tiff")
  set(CPACK_DMG_DS_STORE_SETUP_SCRIPT "${MY_DIR}/generate_ds_store.applescript")
  set(CPACK_DMG_VOLUME_NAME "${CMAKE_PROJECT_PROPER_NAME}")
  set(CPACK_DMG_SLA_USE_RESOURCE_FILE_LICENSE ON)
  set(CPACK_GENERATOR "DragNDrop")
endif()

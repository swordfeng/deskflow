# SPDX-FileCopyrightText: (C) 2024 Chris Rizzitello <sithlord48@gmail.com>
# SPDX-License-Identifier: MIT

# HACK This is set when the files is included so its the real path
# calling CMAKE_CURRENT_LIST_DIR after include would return the wrong scope var
set(MY_DIR ${CMAKE_CURRENT_LIST_DIR})
set(OSX_BUNDLE ${BUILD_OSX_BUNDLE})

set(OS_STRING "macos-${BUILD_ARCHITECTURE}")

if (OSX_BUNDLE)
  # Homebrew installs QtSvg separately; plugins may refer to it through @rpath.
  find_package(Qt6 ${REQUIRED_QT_VERSION} REQUIRED COMPONENTS Svg)
  file(REAL_PATH "${Qt6Svg_DIR}" _qt_svg_cmake_dir)
  get_filename_component(_qt_svg_lib_dir "${_qt_svg_cmake_dir}/../.." ABSOLUTE)
  # macdeployqt does not consistently use -libpath while resolving plugin-only
  # dependencies. Seed the framework and process its dependencies explicitly.
  get_target_property(_qt_svg_binary Qt6::Svg LOCATION)
  get_filename_component(_qt_svg_version_dir "${_qt_svg_binary}" DIRECTORY)
  get_filename_component(_qt_svg_framework_dir "${_qt_svg_version_dir}/../.." ABSOLUTE)
  file(REAL_PATH "${_qt_svg_framework_dir}" _qt_svg_framework_dir)
  install(DIRECTORY "${_qt_svg_framework_dir}"
    DESTINATION "${CMAKE_PROJECT_PROPER_NAME}.app/Contents/Frameworks"
    PATTERN "Headers" EXCLUDE
    PATTERN "*.prl" EXCLUDE
  )
  set(_codesign_identity "-")
  set(_codesign_options "")
  if (APPLE_CODESIGN_DEV)
    set(_codesign_identity "${APPLE_CODESIGN_DEV}")
    set(_codesign_options "-hardened-runtime -timestamp")
  endif()
  install(CODE "execute_process(COMMAND
    ${DEPLOYQT}
    \"\${CMAKE_INSTALL_PREFIX}/${CMAKE_PROJECT_PROPER_NAME}.app\"
    \"-executable=\${CMAKE_INSTALL_PREFIX}/${CMAKE_PROJECT_PROPER_NAME}.app/Contents/MacOS/deskflow-core\"
    \"-executable=\${CMAKE_INSTALL_PREFIX}/${CMAKE_PROJECT_PROPER_NAME}.app/Contents/Frameworks/QtSvg.framework/Versions/A/QtSvg\"
    \"-libpath=${_qt_svg_lib_dir}\"
    ${_codesign_options} \"-codesign=${_codesign_identity}\"
  )")
  set(CPACK_PACKAGE_ICON "${MY_DIR}/dmg-volume.icns")
  set(CPACK_DMG_BACKGROUND_IMAGE "${MY_DIR}/dmg-background.tiff")
  set(CPACK_DMG_DS_STORE_SETUP_SCRIPT "${MY_DIR}/generate_ds_store.applescript")
  set(CPACK_DMG_VOLUME_NAME "${CMAKE_PROJECT_PROPER_NAME}")
  set(CPACK_DMG_SLA_USE_RESOURCE_FILE_LICENSE ON)
  set(CPACK_GENERATOR "DragNDrop")
endif()

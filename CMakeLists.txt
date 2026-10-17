cmake_minimum_required(VERSION 3.16)
project(frame-mic-tuner VERSION 0.3.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# SteamVR に同梱の OpenVR ライブラリ（Frame の SteamOS 上のパス）
set(OPENVR_LIB_DIR "/opt/steamvr/bin/linuxarm64" CACHE PATH "libopenvr_api.so のあるディレクトリ")

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(CAIRO REQUIRED IMPORTED_TARGET cairo)
pkg_check_modules(FREETYPE REQUIRED IMPORTED_TARGET freetype2)
pkg_check_modules(VULKAN REQUIRED IMPORTED_TARGET vulkan)
# 声のチェックの録音・再生（pw_stream）
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)

# 言語のメニューの旗（assets/flags/*.png）を C++ の配列にして焼き込む。PNG を変えると設定し直す
file(GLOB FLAG_PNGS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/flags/*.png")
list(SORT FLAG_PNGS)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${FLAG_PNGS})
set(FLAG_ARRAYS "")
set(FLAG_ROWS "")
foreach(png IN LISTS FLAG_PNGS)
  get_filename_component(flag_name "${png}" NAME_WE)
  file(READ "${png}" flag_hex HEX)
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," flag_bytes "${flag_hex}")
  string(APPEND FLAG_ARRAYS "const unsigned char kFlag_${flag_name}[] = {${flag_bytes}};\n")
  string(APPEND FLAG_ROWS "    {\"${flag_name}\", kFlag_${flag_name}, sizeof(kFlag_${flag_name})},\n")
endforeach()
list(LENGTH FLAG_PNGS FLAG_COUNT)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/flag_icons_data.cpp.in"
  "// CMakeLists.txt が assets/flags/*.png から作るファイル（手で書き換えない）\n"
  "#include \"flag_icons.h\"\n\nnamespace {\n${FLAG_ARRAYS}}  // namespace\n\n"
  "const FlagPng kFlagPngs[] = {\n${FLAG_ROWS}};\nconst std::size_t kFlagPngCount = ${FLAG_COUNT};\n")
configure_file("${CMAKE_CURRENT_BINARY_DIR}/flag_icons_data.cpp.in" "${CMAKE_CURRENT_BINARY_DIR}/flag_icons_data.cpp" COPYONLY)

add_executable(frame-mic-tuner
  src/main.cpp
  src/config.cpp
  src/json.cpp
  src/command.cpp
  src/mic_state.cpp
  src/mic_worker.cpp
  src/outputs.cpp
  src/output_sync.cpp
  src/self_test_outputs.cpp
  src/mic_panel.cpp
  src/theme.cpp
  src/voice_check.cpp
  src/vr_overlay.cpp
  src/vk_texture.cpp
  src/draw.cpp
  src/i18n.cpp
  src/flag_icons.cpp
  "${CMAKE_CURRENT_BINARY_DIR}/flag_icons_data.cpp"
  vendor/frame-updater/cpp/update_check.cpp
  # 共通の UI 部品（色・寸法・切り替え・ボタン・カード・一番下の行）
  vendor/frame-ui/cpp/frame_ui.cpp
  # ささけん＠のほかの Frame アプリの一覧と、インストーラーを Konsole で開く部品
  vendor/frame-apps/cpp/frame_apps.cpp
  vendor/frame-apps/cpp/frame_apps_data.cpp
  vendor/frame-apps/cpp/frame_apps_catalog.cpp
  vendor/frame-apps/cpp/frame_apps_icons.cpp
  vendor/frame-apps/cpp/frame_apps_cairo.cpp
)
target_include_directories(frame-mic-tuner PRIVATE src third_party/openvr vendor/frame-updater/cpp vendor/frame-ui/cpp
                           vendor/frame-apps/cpp)
target_compile_options(frame-mic-tuner PRIVATE -Wall -Wextra)
# Keep Release's normal speed optimization while dropping unused code/data and
# allowing cross-file optimization. Do not change audio floating-point semantics.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(frame-mic-tuner PRIVATE
    $<$<CONFIG:Release>:-ffunction-sections;-fdata-sections>)
  target_link_options(frame-mic-tuner PRIVATE $<$<CONFIG:Release>:-Wl,--gc-sections>)
endif()
include(CheckIPOSupported)
check_ipo_supported(RESULT FRAME_MIC_IPO_SUPPORTED OUTPUT FRAME_MIC_IPO_ERROR)
if(FRAME_MIC_IPO_SUPPORTED)
  set_property(TARGET frame-mic-tuner PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
endif()
# --version で出す版（CHANGELOG.md と合わせる）
target_compile_definitions(frame-mic-tuner PRIVATE FRAME_MIC_TUNER_VERSION="${PROJECT_VERSION}")
target_link_libraries(frame-mic-tuner PRIVATE
  PkgConfig::CAIRO
  PkgConfig::FREETYPE
  PkgConfig::VULKAN
  PkgConfig::PIPEWIRE
  Threads::Threads
  "${OPENVR_LIB_DIR}/libopenvr_api.so"
)
# 実行時も SteamVR 同梱のライブラリを使う
set_target_properties(frame-mic-tuner PROPERTIES
  BUILD_RPATH "${OPENVR_LIB_DIR}"
  INSTALL_RPATH "${OPENVR_LIB_DIR}"
)

install(TARGETS frame-mic-tuner RUNTIME DESTINATION bin)

include(CTest)
if(BUILD_TESTING)
  add_test(NAME self-test COMMAND frame-mic-tuner --self-test)
  add_executable(rgba-test tests/rgba_test.cpp src/draw.cpp)
  target_include_directories(rgba-test PRIVATE src)
  target_link_libraries(rgba-test PRIVATE PkgConfig::CAIRO PkgConfig::FREETYPE)
  target_compile_options(rgba-test PRIVATE -Wall -Wextra)
  if(FRAME_MIC_IPO_SUPPORTED)
    set_property(TARGET rgba-test PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
  endif()
  add_test(NAME rgba COMMAND rgba-test)
endif()

cmake_minimum_required(VERSION 3.20)
project(FmEngineApi VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD_REQUIRED ON)

# -------------------------------------------------------
#  ymfm サブモジュールのパス
#  git submodule add https://github.com/aaronsgiles/ymfm extern/ymfm
# -------------------------------------------------------
set(YMFM_DIR "${CMAKE_SOURCE_DIR}/extern/ymfm/src" CACHE PATH "ymfm src dir")

# -------------------------------------------------------
#  外部チップライブラリのパス
#  git submodule add https://github.com/digital-sound-antiques/emu2149  extern/emu2149
#  git submodule add https://github.com/digital-sound-antiques/emu76489 extern/emu76489
#  git submodule add https://github.com/digital-sound-antiques/emu2212  extern/emu2212
#
#  SAASound は独立 DLL としてビルドし実行時に LoadLibrary でロードする。
#  FmEngineApi には含めない。SAASound.dll を出力ディレクトリに配置すること。
# -------------------------------------------------------
set(EMU2149_DIR  "${CMAKE_SOURCE_DIR}/extern/emu2149"  CACHE PATH "emu2149 dir")
set(EMU76489_DIR "${CMAKE_SOURCE_DIR}/extern/emu76489" CACHE PATH "emu76489 dir")
set(EMU2212_DIR  "${CMAKE_SOURCE_DIR}/extern/emu2212"  CACHE PATH "emu2212 dir")
set(NLOHMANN_DIR "${CMAKE_SOURCE_DIR}/extern/nlohmann" CACHE PATH "nlohmann/json dir")

# =======================================================
#  ターゲット 1: FmEngineApi.dll
# =======================================================
add_library(FmEngineApi SHARED
    src/FmEngineApi.cpp
    # ymfm ソース
    ${YMFM_DIR}/ymfm_opl.cpp
    ${YMFM_DIR}/ymfm_opn.cpp
    ${YMFM_DIR}/ymfm_opm.cpp
    ${YMFM_DIR}/ymfm_ssg.cpp
    ${YMFM_DIR}/ymfm_pcm.cpp
    ${YMFM_DIR}/ymfm_opz.cpp
    ${YMFM_DIR}/ymfm_misc.cpp
    ${YMFM_DIR}/ymfm_adpcm.cpp
    # 外部チップ ソース (SAASound は除く)
    ${EMU2149_DIR}/emu2149.c
    ${EMU76489_DIR}/emu76489.c
    ${EMU2212_DIR}/emu2212.c
)

target_include_directories(FmEngineApi PRIVATE
    src/
    ${YMFM_DIR}
    ${EMU2149_DIR}
    ${EMU76489_DIR}
    ${EMU2212_DIR}
)

# FMENGINE_EXPORTS → FmEngineApi.cpp 内で dllexport になる
target_compile_definitions(FmEngineApi PRIVATE FMENGINE_EXPORTS)

# .def ファイルでエクスポートシンボルを明示 (MSVC)
if (MSVC)
    set_target_properties(FmEngineApi PROPERTIES
        LINK_FLAGS "/DEF:\"${CMAKE_SOURCE_DIR}/src/FmEngineApi.def\""
    )
endif()

# WASAPI / COM リンク
target_link_libraries(FmEngineApi PRIVATE
    ole32
    uuid
    avrt
)

# バージョン情報を DLL に埋め込む (Windows リソース)
if (MSVC)
    target_sources(FmEngineApi PRIVATE src/FmEngineApi.rc)
endif()

# MSVC 最適化・警告設定
if (MSVC)
    target_compile_options(FmEngineApi PRIVATE
        /Oi /GL /W4 /utf-8
        /wd4100   # unreferenced formal parameter (ymfm 内)
        /wd4244   # narrowing conversion (ymfm 内)
        /wd4251   # dll-interface warning (内部クラスに不要)
    )
    target_link_options(FmEngineApi PRIVATE /LTCG)

    # .lib (インポートライブラリ) と .dll を build/bin に揃える
    set_target_properties(FmEngineApi PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
        PDB_OUTPUT_DIRECTORY     "${CMAKE_BINARY_DIR}/bin"
    )
endif()

# MinGW / Clang
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(FmEngineApi PRIVATE -O3 -Wall -Wextra)
    # シンボルの可視性をデフォルト hidden にして FMENGINE_API のみ公開
    target_compile_options(FmEngineApi PRIVATE -fvisibility=hidden)
endif()

# =======================================================
#  ターゲット 2: sample_app.exe  (使用例)
#  DLL の .lib にリンクする形でビルド
# =======================================================
add_executable(sample_app src/main.cpp)

target_include_directories(sample_app PRIVATE
    src/
    ${NLOHMANN_DIR}
)

target_link_libraries(sample_app PRIVATE FmEngineApi)

if (MSVC)
    target_compile_options(sample_app PRIVATE /utf8)
    set_target_properties(sample_app PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
    target_compile_options(sample_app PRIVATE /W4 /wd4100)
endif()

# test_patches.json をビルド出力ディレクトリにコピー
add_custom_command(TARGET sample_app POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/src/test_patches.json"
        "$<TARGET_FILE_DIR:sample_app>/test_patches.json"
    COMMENT "Copying test_patches.json"
)

# =======================================================
#  インストール規則 (任意)
#  cmake --install build --prefix install_dir
# =======================================================
install(TARGETS FmEngineApi
    RUNTIME DESTINATION bin          # .dll
    LIBRARY DESTINATION lib          # .so
    ARCHIVE DESTINATION lib          # .lib (import library)
)
install(FILES src/FmEngineApi.h DESTINATION include)

cmake_minimum_required(VERSION 3.20)
project(FmEngineApi VERSION 2.0.0 LANGUAGES C CXX)

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
#  git submodule add https://github.com/stripwax/SAASound               extern/SAASound
#  git submodule add https://github.com/nlohmann/json                   extern/nlohmann_json
#
#  SAASound は独立 DLL としてビルドし実行時に LoadLibrary でロードする。
#  FmEngineApi とは別ターゲットとして add_subdirectory で追加する。
# -------------------------------------------------------
set(EMU2149_DIR  "${CMAKE_SOURCE_DIR}/extern/emu2149"  CACHE PATH "emu2149 dir")
set(EMU76489_DIR "${CMAKE_SOURCE_DIR}/extern/emu76489" CACHE PATH "emu76489 dir")
set(EMU2212_DIR  "${CMAKE_SOURCE_DIR}/extern/emu2212"  CACHE PATH "emu2212 dir")
set(NLOHMANN_DIR "${CMAKE_SOURCE_DIR}/extern/nlohmann_json/single_include" CACHE PATH "nlohmann/json include dir")
set(SAASOUND_DIR "${CMAKE_SOURCE_DIR}/extern/SAASound" CACHE PATH "SAASound dir")

# -------------------------------------------------------
#  SAASound サブプロジェクト
#  EXCLUDE_FROM_ALL で cmake --build . 時は除外し、
#  SAASound ターゲットを明示的にビルドする必要がある場合のみビルドされる。
#  Visual Studio では .sln に SAASound プロジェクトが追加される。
# -------------------------------------------------------
if (EXISTS "${SAASOUND_DIR}/CMakeLists.txt")
    # SAASound の CMake 変数を事前設定
    set(SAMPLE_RATE_HZ  "44100"   CACHE STRING "SAASound output sample rate" FORCE)
    set(EXTERNAL_CLK_HZ "8000000" CACHE STRING "SAASound external clock Hz"  FORCE)
    # USE_CONFIG_FILE はデフォルト ON のまま使う。
    # OFF にすると SAAImpl.h の m_output_bitmask 宣言が除外され
    # SAAImpl.cpp がコンパイルエラーになる (SAASound リポジトリの既知の問題)。
    add_subdirectory(${SAASOUND_DIR} ${CMAKE_BINARY_DIR}/SAASound)

    if (TARGET SAASound)
        if (MSVC)
            set_target_properties(SAASound PROPERTIES
                RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
                PDB_OUTPUT_DIRECTORY     "${CMAKE_BINARY_DIR}/bin"
            )
        endif()
        message(STATUS "SAASound subproject added from ${SAASOUND_DIR}")
    endif()
else()
    message(STATUS "SAASound not found at ${SAASOUND_DIR} -- SAA1099 disabled.")
    message(STATUS "  Run: git submodule add https://github.com/stripwax/SAASound extern/SAASound")
endif()

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

# .c ファイルを C として明示コンパイル (MSVC は混在プロジェクトでも C++ 扱いになる場合がある)
set_source_files_properties(
    ${EMU2149_DIR}/emu2149.c
    ${EMU76489_DIR}/emu76489.c
    ${EMU2212_DIR}/emu2212.c
    PROPERTIES LANGUAGE C
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
    set_target_properties(sample_app PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
    target_compile_options(sample_app PRIVATE /W4 /wd4100 /utf-8)
endif()

# patches/ ディレクトリの JSON ファイルをビルド出力ディレクトリにコピー
set(PATCHES_SRC_DIR "${CMAKE_SOURCE_DIR}/src/patches")
if (EXISTS "${PATCHES_SRC_DIR}")
    file(GLOB PATCH_FILES "${PATCHES_SRC_DIR}/*.json")
    foreach(PATCH_FILE ${PATCH_FILES})
        add_custom_command(TARGET sample_app POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_FILE_DIR:sample_app>/patches"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${PATCH_FILE}"
                "$<TARGET_FILE_DIR:sample_app>/patches/"
            COMMENT "Copying ${PATCH_FILE}"
        )
    endforeach()
    # cmake 構成時にもコピー
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/bin/patches")
    foreach(PATCH_FILE ${PATCH_FILES})
        configure_file("${PATCH_FILE}" "${CMAKE_BINARY_DIR}/bin/patches/" COPYONLY)
    endforeach()
else()
    message(WARNING "patches/ directory not found at ${PATCHES_SRC_DIR}")
endif()

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

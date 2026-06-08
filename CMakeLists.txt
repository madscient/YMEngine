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

# =======================================================
#  ターゲット 1: FmEngineApi.dll  (または .so / .a)
# =======================================================
add_library(FmEngineApi SHARED
    src/FmEngineApi.cpp
    # ymfm ソース (DLL 内に静的に取り込む)
    ${YMFM_DIR}/ymfm_opl.cpp
    ${YMFM_DIR}/ymfm_opn.cpp
    ${YMFM_DIR}/ymfm_opm.cpp
    ${YMFM_DIR}/ymfm_ssg.cpp
    ${YMFM_DIR}/ymfm_pcm.cpp
    ${YMFM_DIR}/ymfm_opz.cpp
    ${YMFM_DIR}/ymfm_misc.cpp
    ${YMFM_DIR}/ymfm_adpcm.cpp
)

target_include_directories(FmEngineApi PRIVATE
    src/
    ${YMFM_DIR}
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
        /Oi /GL /W4
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

target_include_directories(sample_app PRIVATE src/)

# FmEngineApi.h だけで使えるようにするため、
# PRIVATE でインポートライブラリにリンク
target_link_libraries(sample_app PRIVATE FmEngineApi)

if (MSVC)
    set_target_properties(sample_app PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
    target_compile_options(sample_app PRIVATE /W4 /wd4100)
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

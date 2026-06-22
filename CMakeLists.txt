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

# =======================================================
#  ターゲット: FmEngineApi.dll / libFmEngineApi.so
# =======================================================
add_library(FmEngineApi SHARED
    src/FmEngineApi.cpp
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

target_compile_definitions(FmEngineApi PRIVATE FMENGINE_EXPORTS)

if (MSVC)
    set_target_properties(FmEngineApi PROPERTIES
        LINK_FLAGS               "/DEF:\"${CMAKE_SOURCE_DIR}/src/FmEngineApi.def\""
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
        PDB_OUTPUT_DIRECTORY     "${CMAKE_BINARY_DIR}/bin"
        DEBUG_POSTFIX            ""
    )
    target_compile_options(FmEngineApi PRIVATE
        /Oi /GL /W4 /utf-8
        /wd4100   # unreferenced formal parameter (ymfm 内)
        /wd4244   # narrowing conversion (ymfm 内)
        /wd4251   # dll-interface warning
    )
    target_link_options(FmEngineApi PRIVATE /LTCG)
    target_sources(FmEngineApi PRIVATE src/FmEngineApi.rc)
endif()

if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(FmEngineApi PRIVATE -O3 -Wall -Wextra -fvisibility=hidden)
endif()

# =======================================================
#  インストール規則
#  cmake --install build --prefix <dir>
# =======================================================
install(TARGETS FmEngineApi
    RUNTIME DESTINATION bin    # .dll
    LIBRARY DESTINATION lib    # .so
    ARCHIVE DESTINATION lib    # .lib (import library)
)
install(FILES src/FmEngineApi.h DESTINATION include)

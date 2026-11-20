cmake_minimum_required(VERSION 3.10)

## ---------------------------------------------------------------------------
## Battleship - PS Vita homebrew (VitaSDK / vita2d)
## ---------------------------------------------------------------------------
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

project(Battleship C)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "Battleship")
set(VITA_TITLEID  "BTLSHP001")      # 9 chars
set(VITA_VERSION  "01.01")

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -O2 -ffast-math")
set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d PARENTAL_LEVEL=1")

include_directories(include)

add_executable(${PROJECT_NAME}
  src/main.c
  src/game_logic.c
  src/ai.c
  src/adhoc_net.c
  src/audio.c
  src/render.c
)

target_link_libraries(${PROJECT_NAME}
  vita2d
  freetype
  png
  jpeg
  z
  m
  c
  SceDisplay_stub
  SceGxm_stub
  SceSysmodule_stub
  SceCtrl_stub
  SceTouch_stub
  SceAudio_stub
  ScePgf_stub
  ScePvf_stub
  SceCommonDialog_stub
  SceAppMgr_stub
  SceAppUtil_stub
  ScePower_stub
  SceNet_stub
  SceNetCtl_stub
  SceNetAdhocMatching_stub
  SceKernelThreadMgr_stub
  SceProcessmgr_stub
  SceLibKernel_stub
)

vita_create_self(eboot.bin ${PROJECT_NAME})

vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} eboot.bin
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE sce_sys/icon0.png sce_sys/icon0.png
  FILE sce_sys/pic0.png sce_sys/pic0.png
  FILE sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
  FILE sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
  FILE sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
)

## Optional: push to a Vita running VitaShell's FTP server (make send VITA_IP=...)
if(DEFINED VITA_IP)
  add_custom_target(send
    COMMAND curl -T ${CMAKE_BINARY_DIR}/${PROJECT_NAME}.vpk ftp://${VITA_IP}:1337/ux0:/data/
    DEPENDS ${PROJECT_NAME}.vpk
  )
endif()

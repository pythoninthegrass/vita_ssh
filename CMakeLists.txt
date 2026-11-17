cmake_minimum_required(VERSION 3.16)

# --- Toolchain Vita ---
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

project(ssh_client_vita)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

# --- Metadata ---
set(VITA_APP_NAME "VitaSSH")
set(VITA_TITLEID "ROM000069")
set(VITA_VERSION  "01.00")

# The server is its own app rather than a mode of the client: it installs next to
# ROM000069 as a separate VPK, so a device can have both. Its own target name,
# tile name and titleid follow from that.
set(VITA_SERVER_NAME      "ssh_server_vita")
set(VITA_SERVER_APP_NAME  "VitaSSHd")
set(VITA_SERVER_TITLEID   "ROM000070")
set(VITA_UPDATER_NAME "ssh_updater_vita")
# ROM000071 was used for a temporary hardware test; keep this title distinct.
set(VITA_UPDATER_TITLEID "ROM000072")

# --- Flags ---
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=gnu11 -Wall -O2")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")

# --- Includes ---
include_directories(
  ./include
  ./src
  $ENV{VITASDK}/arm-vita-eabi/include
)

# --- Create include directory if it doesn't exist ---
file(MAKE_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include)

# vita.cmake's macros append to cache-backed flag variables and never clear them, so a
# second app in the same project inherits the first's flags: vita-pack-vpk then sees
# sce_sys/icon0.png listed twice and dies with "File already exists", and the param.sfo
# is handed both apps' -s TITLE_ID. Dropping the shadowing normal variables makes each
# call re-seed from the (untouched) cache entry, so per-app flags stay per-app — and a
# flag passed in with -DVITA_PACK_VPK_FLAGS=… still reaches every call. Must be a macro:
# a function would unset in its own scope and change nothing.
macro(vita_reset_flags)
  unset(VITA_ELF_CREATE_FLAGS)
  unset(VITA_MAKE_FSELF_FLAGS)
  unset(VITA_MKSFOEX_FLAGS)
  unset(VITA_PACK_VPK_FLAGS)
endmacro()

# --- Sources: client app (libssh2) ---
add_executable(${PROJECT_NAME}
  src/main.c
  src/input.c
  src/network.c
  src/ssh_client.c
  src/shell.c
  src/text.c
  src/menu.c
)

target_link_libraries(${PROJECT_NAME}
  vita2d
  ssh2
  ssl
  crypto
  z
  SceDisplay_stub
  SceCtrl_stub
  SceSysmodule_stub
  SceNet_stub
  SceNetCtl_stub
  SceLibKernel_stub
  SceGxm_stub
  SceIme_stub
  SceAppUtil_stub
  ScePower_stub
  SceSysmem_stub
  SceCommonDialog_stub
  SceAppMgr_stub
  ScePgf_stub
  png
  bz2
  SceTouch_stub
  m
)

vita_reset_flags()
vita_create_self(${PROJECT_NAME}.self ${PROJECT_NAME})

vita_reset_flags()
vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} ${PROJECT_NAME}.self
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE ./sce_sys/icon0.png sce_sys/icon0.png
  FILE ./sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
  FILE ./sce_sys/livearea/contents/bg0.png sce_sys/livearea/contents/bg0.png
)

# vita_create_vpk only wires its own add_dependencies when the eboot argument is a
# target name, and we (like every vita project) hand it the .self file — whose target is
# <name>.self-self. So the VPK gets no edge on the SELF: it is packed on a clean tree and
# never repacked again, however many times a source changes. The explicit edge makes the
# VPK rebuild after the SELF within the same pass, which is what lets `task build`
# fingerprints be trusted at all.
add_dependencies(${PROJECT_NAME}.vpk-vpk ${PROJECT_NAME}.self-self)

# --- Sources: sshd app (libssh server + builtin shell) ---
# Shares only src/network.c (the sceNet bring-up) with the client; the client never
# links libssh and the server never links libssh2. Like the client, nothing globs —
# a new server .c file has to be listed here or no compiler ever sees it.
# vpk_head_template.c is generated here from third_party/vitashell/head.bin (VitaShell's
# GPLv3 package-header template, see third_party/vitashell/README.md) so the template is
# compiled into the app without committing a second copy as a C array.
set(VPK_HEAD_TEMPLATE_SRC ${CMAKE_BINARY_DIR}/vpk_head_template.c)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/third_party/vitashell/head.bin VPK_HEAD_HEX HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," VPK_HEAD_ARRAY "${VPK_HEAD_HEX}")
file(WRITE ${VPK_HEAD_TEMPLATE_SRC}
  "const unsigned char vpk_head_template[] = {${VPK_HEAD_ARRAY}};\n"
  "const unsigned long vpk_head_template_len = sizeof(vpk_head_template);\n")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/vitashell/head.bin)

add_executable(${VITA_UPDATER_NAME} src/sshd_updater.c src/vpk_promote.c)
target_compile_definitions(${VITA_UPDATER_NAME} PRIVATE
  SSHD_TITLEID="${VITA_SERVER_TITLEID}" SSHD_UPDATER_TITLEID="${VITA_UPDATER_TITLEID}")
target_link_libraries(${VITA_UPDATER_NAME}
  SceAppMgr_stub SceSysmodule_stub ScePromoterUtil_stub SceLibKernel_stub)
vita_reset_flags()
vita_create_self(${VITA_UPDATER_NAME}.self ${VITA_UPDATER_NAME} UNSAFE)
vita_reset_flags()
vita_create_vpk(${VITA_UPDATER_NAME}.vpk ${VITA_UPDATER_TITLEID} ${VITA_UPDATER_NAME}.self
  VERSION ${VITA_VERSION}
  NAME "VitaSSHd Updater"
)
add_dependencies(${VITA_UPDATER_NAME}.vpk-vpk ${VITA_UPDATER_NAME}.self-self)

add_executable(${VITA_SERVER_NAME}
  src/sshd.c
  src/sshd_session.c
  src/sshd_builtins.c
  src/sshd_vpk.c
  src/vpk_promote.c
  src/vpk_zip.c
  src/vpk_head.c
  ${VPK_HEAD_TEMPLATE_SRC}
  src/sshd_hostkey.c
  src/sshd_sysinfo.c
  src/sftp_server.c
  src/sh_path.c
  src/sh_grep.c
  src/sh_headtail.c
  src/sh_textops.c
  src/sh_stdin.c
  src/vita_shims.c
  src/network.c
)

# The server sits at the ceiling vita-elf-create enforces: segment 0 has no room left for
# its SCE data tail, and going over dies with "Cannot allocate N bytes for SCE data at end
# of segment 0; segment 1 overlaps" -- an abort, not a linker error, and not a message that
# points at size. head and tail cost 3404 bytes of text and the slack measured 3284, so the
# file holding the builtins -- the part of the server that keeps growing -- is compiled for
# size. Only code layout changes, and it buys back more than a new command costs, leaving
# room again for the next one.
set_source_files_properties(src/sshd_builtins.c PROPERTIES COMPILE_OPTIONS "-Os")

# libssh compiles its server and SFTP declarations out of the public headers unless
# WITH_SERVER is defined, and it defines that only in its own private config.h. A
# consumer that does not define it too gets no ssh_bind_* and no sftp_server_*.
target_compile_definitions(${VITA_SERVER_NAME} PRIVATE WITH_SERVER
  SSHD_TITLEID="${VITA_SERVER_TITLEID}" SSHD_UPDATER_TITLEID="${VITA_UPDATER_TITLEID}")

# libssh's stock SFTP handlers call these libc functions with the client's POSIX path.
# --wrap routes every reference, inside libssh.a too, to __wrap_<name> in
# src/sftp_server.c, which translates through sh_path.c while an SFTP packet is being
# handled and passes straight through otherwise. Server target only: the client app
# never links these wrappers.
target_link_options(${VITA_SERVER_NAME} PRIVATE
  -Wl,--wrap=open
  -Wl,--wrap=close
  -Wl,--wrap=opendir
  -Wl,--wrap=readdir
  -Wl,--wrap=closedir
  -Wl,--wrap=stat
  -Wl,--wrap=lstat
  -Wl,--wrap=mkdir
  -Wl,--wrap=rmdir
  -Wl,--wrap=unlink
  -Wl,--wrap=chmod
  -Wl,--wrap=utimes
  -Wl,--wrap=chown
  -Wl,--wrap=truncate
  -Wl,--wrap=statvfs
  -Wl,--wrap=ssh_channel_write
  -Wl,--wrap=realpath
)

# ssh is the SDK's static libssh. z is libssh's compression path and pthread is what
# its pthread.c wants; the SDK ships libpthread.a. png and bz2 are deliberately
# absent: nothing in the server pulls vita2d's image loaders, so those archives are
# never searched. SceAppMgr_stub and SceCommonDialog_stub are the non-obvious ones —
# vita2d's sharedFB path references them, so omitting either breaks the link.
# ScePower_stub is the idle-timer tick that keeps the device awake while the
# server runs. ScePromoterUtil_stub is the system package installer behind `vpk install`.
# SceKernelModulemgr_stub is where sceKernelGetSystemSwVersion lives — it is not in the
# SceLibKernel_stub already listed, and without it `uname -a` has no firmware to report.
target_link_libraries(${VITA_SERVER_NAME}
  vita2d
  ssh
  ssl
  crypto
  z
  pthread
  SceDisplay_stub
  SceCtrl_stub
  SceSysmodule_stub
  SceNet_stub
  SceNetCtl_stub
  SceLibKernel_stub
  SceKernelModulemgr_stub
  SceGxm_stub
  SceSysmem_stub
  SceAppMgr_stub
  SceCommonDialog_stub
  ScePgf_stub
  ScePower_stub
  ScePromoterUtil_stub
  m
)

vita_reset_flags()
# UNSAFE: system promotion and the updater handoff need elevated app permissions.
vita_create_self(${VITA_SERVER_NAME}.self ${VITA_SERVER_NAME} UNSAFE)

# Its own tile: the client's blue padlock and the server's teal prompt are distinct apps on
# the shelf, so they do not share art. The packed paths stay sce_sys/... — each VPK has its
# own sce_sys, so only the source directories differ.
vita_reset_flags()
vita_create_vpk(${VITA_SERVER_NAME}.vpk ${VITA_SERVER_TITLEID} ${VITA_SERVER_NAME}.self
  VERSION ${VITA_VERSION}
  NAME ${VITA_SERVER_APP_NAME}
  FILE ${CMAKE_CURRENT_BINARY_DIR}/${VITA_UPDATER_NAME}.vpk updater.vpk
  FILE ./sce_sys_sshd/icon0.png sce_sys/icon0.png
  FILE ./sce_sys_sshd/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
  FILE ./sce_sys_sshd/livearea/contents/bg0.png sce_sys/livearea/contents/bg0.png
)

# Same edge as the client, for the same reason.
add_dependencies(${VITA_SERVER_NAME}.vpk-vpk ${VITA_SERVER_NAME}.self-self)
add_dependencies(${VITA_SERVER_NAME}.vpk-vpk ${VITA_UPDATER_NAME}.vpk-vpk)

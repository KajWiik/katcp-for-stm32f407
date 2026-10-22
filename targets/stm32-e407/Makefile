##############################################################################
# Build global options
# NOTE: Can be overridden externally.
#

ifeq ($(USE_OPT),)
  USE_OPT = -O2 -ggdb -fomit-frame-pointer -falign-functions=16
endif

ifeq ($(USE_COPT),)
  USE_COPT =
endif

ifeq ($(USE_CPPOPT),)
  USE_CPPOPT = -fno-rtti
endif

ifeq ($(USE_LINK_GC),)
  USE_LINK_GC = yes
endif

ifeq ($(USE_LDOPT),)
  USE_LDOPT =
endif

ifeq ($(USE_LTO),)
  USE_LTO = yes
endif

ifeq ($(USE_VERBOSE_COMPILE),)
  USE_VERBOSE_COMPILE = no
endif

ifeq ($(USE_SMART_BUILD),)
  USE_SMART_BUILD = yes
endif

#
# Build global options
##############################################################################

##############################################################################
# Architecture or project specific options
#

ifeq ($(USE_PROCESS_STACKSIZE),)
  USE_PROCESS_STACKSIZE = 0x400
endif

ifeq ($(USE_EXCEPTIONS_STACKSIZE),)
  USE_EXCEPTIONS_STACKSIZE = 0x400
endif

# Hardware FPU enabled -- the synth loop-setup math (charge-pump
# current / VCO tuning-curve selection, IMPLEMENTATION.md section 6.3)
# benefits enough from real floating point that the Cortex-M4F's
# single-precision FPU is worth using; the per-thread context-switch
# cost of the FPU register bank is accepted as reasonable for this
# application. "hard" selects the hard-float ABI (arguments passed in
# FPU registers), matching -mfpu=fpv4-sp-d16 below.
ifeq ($(USE_FPU),)
  USE_FPU = hard
endif

ifeq ($(USE_FPU_OPT),)
  USE_FPU_OPT = -mfloat-abi=$(USE_FPU) -mfpu=fpv4-sp-d16
endif

#
# Architecture or project specific options
##############################################################################

##############################################################################
# Project, target, sources and paths
#

PROJECT = katcp_stm32_e407

MCU  = cortex-m4

# ChibiOS is vendored as a submodule -- see README "Setup".
CHIBIOS  := third_party/ChibiOS
CONFDIR  := ./cfg
BUILDDIR := ./build
DEPDIR   := ./.dep

# Licensing files.
include $(CHIBIOS)/os/license/license.mk
# Startup files.
include $(CHIBIOS)/os/common/startup/ARMCMx/compilers/GCC/mk/startup_stm32f4xx.mk
# HAL-OSAL files.
include $(CHIBIOS)/os/hal/hal.mk
include $(CHIBIOS)/os/hal/ports/STM32/STM32F4xx/platform.mk
# Rev E and later E407 boards use the LAN8710A PHY -- this must be
# OLIMEX_STM32_E407_REV_D, NOT the plain OLIMEX_STM32_E407 board
# (which targets the older KS8721 PHY and will fail PHY detection
# on this hardware). See targets/stm32-e407/README.md.
include $(CHIBIOS)/os/hal/boards/OLIMEX_STM32_E407_REV_D/board.mk
include $(CHIBIOS)/os/hal/osal/rt-nil/osal.mk
# RTOS files.
include $(CHIBIOS)/os/rt/rt.mk
include $(CHIBIOS)/os/common/ports/ARMv7-M/compilers/GCC/mk/port.mk
# lwIP bindings.
# lwip.mk defaults LWSRC_EXTRAS to the HTTP demo files (which need
# generated fsdata.c content we don't have and never set up) --
# override it to pull in the SNTP client instead, which is what
# src/rtc_sync.c actually needs (see IMPLEMENTATION.md §7).
LWSRC_EXTRAS = $(SNTPFILES)
include $(CHIBIOS)/os/various/lwip_bindings/lwip.mk

# Auto-discover common/'s .c files and include dirs rather than
# listing them by hand (mirrors what a CMake-based target would do
# with a COMMON_DIR variable).
AUTOBUILD_ROOT := ../../common/
include $(CHIBIOS)/tools/mk/autobuild.mk

# Define linker script file here.
LDSCRIPT= $(STARTUPLD)/STM32F407xG.ld

# C sources that can be compiled in ARM or THUMB mode depending on the
# global setting.
# newlib_bindings/syscalls.c: newlib's own _write_r/_read_r/etc. (the
# reentrant wrappers libg.a actually calls) are thin shims expecting
# the target to supply plain _write/_read/etc. -- undefined references
# to those otherwise fail the final link. ChibiOS ships this file
# specifically to supply the reentrant versions directly instead
# (_sbrk_r genuinely backed by ChibiOS's own heap, chCoreAllocFromBase
# -- CH_CFG_USE_HEAP is TRUE in cfg/chconf.h -- not a dummy stub),
# which preempts ever needing newlib's own versions at all. Not
# previously included; found and fixed via a real build against this
# target, not assumed to be needed in advance.
CSRC = $(ALLCSRC) \
       $(CHIBIOS)/os/various/newlib_bindings/syscalls.c \
       src/portab.c \
       src/platform_chibios.c \
       src/rtc_sync.c \
       src/synth_lmx2594.c \
       src/pac1954_i2c.c \
       src/web_server.c \
       src/main.c

# C++ sources that can be compiled in ARM or THUMB mode depending on the
# global setting.
CPPSRC = $(ALLCPPSRC)

# List ASM source files here.
ASMSRC = $(ALLASMSRC)

# List ASM with preprocessor source files here.
ASMXSRC = $(ALLXASMSRC)

# Inclusion directories.
INCDIR = $(CONFDIR) $(ALLINC)

# Define C warning options here.
CWARN = -Wall -Wextra -Wundef -Wstrict-prototypes

# Define C++ warning options here.
CPPWARN = -Wall -Wextra -Wundef

#
# Project, target, sources and paths
##############################################################################

##############################################################################
# Start of user section
#

UDEFS =
UADEFS =
UINCDIR =
ULIBDIR =
ULIBS =

#
# End of user section
##############################################################################

##############################################################################
# Common rules
#

RULESPATH = $(CHIBIOS)/os/common/startup/ARMCMx/compilers/GCC/mk
include $(RULESPATH)/arm-none-eabi.mk
include $(RULESPATH)/rules.mk

#
# Common rules
##############################################################################

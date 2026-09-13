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

# No FPU use anywhere in this project -- see IMPLEMENTATION.md \u00a76.1 on
# why the shared common/ code deliberately avoids float/double.
ifeq ($(USE_FPU),)
  USE_FPU = no
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
include $(CHIBIOS)/os/various/lwip_bindings/lwip.mk

# Code shared with other targets (e.g. targets/rp2040-w5500-pico) lives
# two levels up; auto-discover its .c files and include dirs rather
# than listing them by hand (mirrors CMake's COMMON_DIR on that target).
AUTOBUILD_ROOT := ../../common/
include $(CHIBIOS)/tools/mk/autobuild.mk

# Define linker script file here.
LDSCRIPT= $(STARTUPLD)/STM32F407xG.ld

# C sources that can be compiled in ARM or THUMB mode depending on the
# global setting.
CSRC = $(ALLCSRC) \
       src/portab.c \
       src/platform_chibios.c \
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

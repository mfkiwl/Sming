COMPONENT_SRCDIRS := \
	Core $(call ListAllSubDirs,$(COMPONENT_PATH)/Core) \
	Platform \
	System \
	Wiring \
	Services/HexDump

COMPONENT_INCDIRS := \
	Components \
	System/include \
	Wiring Core \
	.

COMPONENT_DEPENDS := \
	Storage \
	sming-arch \
	FlashString \
	Spiffs \
	IFS \
	SPI \
	terminal

COMPONENT_DOXYGEN_PREDEFINED := \
	ENABLE_CMD_EXECUTOR=1

COMPONENT_DOXYGEN_INPUT := \
	Core \
	$(patsubst $(SMING_HOME)/%,%,$(wildcard $(SMING_HOME)/Arch/*/Core)) \
	Platform \
	Services \
	Wiring \
	System

# => Disable CommandExecutor functionality if not used and save some ROM and RAM
COMPONENT_VARS			+= ENABLE_CMD_EXECUTOR
ENABLE_CMD_EXECUTOR		?= 1
ifeq ($(ENABLE_CMD_EXECUTOR),1)
COMPONENT_SRCDIRS		+= Services/CommandProcessing
endif
GLOBAL_CFLAGS			+= -DENABLE_CMD_EXECUTOR=$(ENABLE_CMD_EXECUTOR)

#
RELINK_VARS += DISABLE_NETWORK
DISABLE_NETWORK ?= 0
ifeq ($(DISABLE_NETWORK),1)
GLOBAL_CFLAGS += -DDISABLE_NETWORK=1
DISABLE_WIFI := 1
else
COMPONENT_DEPENDS += Network
endif

#
RELINK_VARS += DISABLE_WIFI
DISABLE_WIFI ?= 0
ifeq ($(DISABLE_WIFI),1)
GLOBAL_CFLAGS += -DDISABLE_WIFI=1
endif

# => LOCALE
COMPONENT_VARS			+= LOCALE
ifdef LOCALE
	GLOBAL_CFLAGS		+= -DLOCALE=$(LOCALE)
endif

# => SPI
COMPONENT_VARS			+= ENABLE_SPI_DEBUG
ENABLE_SPI_DEBUG		?= 0
ifeq ($(ENABLE_SPI_DEBUG),1)
GLOBAL_CFLAGS			+= -DSPI_DEBUG=1
endif

### Debug output parameters

# By default `debugf` does not print file name and line number. If you want this enabled set the directive below to 1
CONFIG_VARS				+= DEBUG_PRINT_FILENAME_AND_LINE
DEBUG_PRINT_FILENAME_AND_LINE ?= 0
GLOBAL_CFLAGS			+= -DDEBUG_PRINT_FILENAME_AND_LINE=$(DEBUG_PRINT_FILENAME_AND_LINE)
# When rules are created make will see '$*' so substitute the filename
GLOBAL_CFLAGS				+= -DCUST_FILE_BASE=$$*

# Default debug verbose level is INFO, where DEBUG=3 INFO=2 WARNING=1 ERROR=0
CONFIG_VARS				+= DEBUG_VERBOSE_LEVEL
DEBUG_VERBOSE_LEVEL		?= 2
GLOBAL_CFLAGS			+= -DDEBUG_VERBOSE_LEVEL=$(DEBUG_VERBOSE_LEVEL)

CONFIG_VARS			+= ENABLE_GDB
ifeq ($(ENABLE_GDB), 1)
	GLOBAL_CFLAGS	+= -ggdb -DENABLE_GDB=1
endif

# Default COM port speed (generic)
CACHE_VARS			+= COM_SPEED
COM_SPEED			?= 115200

# Default COM port speed used in code
CONFIG_VARS			+= COM_SPEED_SERIAL
COM_SPEED_SERIAL	?= $(COM_SPEED)
APP_CFLAGS			+= \
	-DCOM_SPEED_SERIAL=$(COM_SPEED_SERIAL) \
	-DSERIAL_BAUD_RATE=$(COM_SPEED_SERIAL)

# Task queue counter to check for overflows
COMPONENT_VARS		+= ENABLE_TASK_COUNT
ifeq ($(ENABLE_TASK_COUNT),1)
	GLOBAL_CFLAGS	+= -DENABLE_TASK_COUNT=1
endif

# Task queue length
COMPONENT_VARS		+= TASK_QUEUE_LENGTH
TASK_QUEUE_LENGTH	?= 10
COMPONENT_CXXFLAGS	+= -DTASK_QUEUE_LENGTH=$(TASK_QUEUE_LENGTH)

# Size of a String object - change this to increase space for Small String Optimisation (SSO)
COMPONENT_VARS		+= STRING_OBJECT_SIZE
STRING_OBJECT_SIZE	?= 12
GLOBAL_CFLAGS		+= -DSTRING_OBJECT_SIZE=$(STRING_OBJECT_SIZE) 

##@Flashing

.PHONY: flashinit
flashinit: $(ESPTOOL) | $(FW_BASE) ##Erase your device's flash memory
	$(info Flash init data default and blank data)
	$(Q) $(EraseFlash)

.PHONY: flashboot
flashboot: $(FLASH_BOOT_LOADER) kill_term ##Write just the Bootloader
	$(call WriteFlash,$(FLASH_BOOT_CHUNKS))

.PHONY: flashapp
flashapp: all kill_term ##Write just the application image(s)
	$(Q) $(call CheckPartitionChunks,$(FLASH_APP_CHUNKS))
	$(call WriteFlash,$(FLASH_APP_CHUNKS))

.PHONY: flash
flash: all kill_term ##Write the boot loader and all defined partition images
	$(Q) $(call CheckPartitionChunks,$(FLASH_PARTITION_CHUNKS))
	$(call WriteFlash,$(FLASH_BOOT_CHUNKS) $(FLASH_MAP_CHUNK) $(FLASH_PARTITION_CHUNKS))
ifeq ($(ENABLE_GDB), 1)
	$(GDB_CMDLINE)
else ifneq ($(SMING_ARCH),Host)
	$(TERMINAL)
endif

.PHONY: verifyflash
verifyflash: ##Read all flash sections and verify against source
	$(Q) $(call CheckPartitionChunks,$(FLASH_PARTITION_CHUNKS))
	$(call VerifyFlash,$(FLASH_BOOT_CHUNKS) $(FLASH_MAP_CHUNK) $(FLASH_PARTITION_CHUNKS))

.PHONY: flashid
flashid: ##Read flash identifier and determine actual size
	$(call ReadFlashID)

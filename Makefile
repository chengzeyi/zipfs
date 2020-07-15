# tool marcros
CC := clang
DBGFLAG := -g -DDEBUG

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S), Linux)
    # do nothing
else ifeq ($(UNAME_S), Darwin)
    OSXFUSE_ROOT := /usr/local
    OSXFUSE_INCLUDE_DIR := $(OSXFUSE_ROOT)/include/osxfuse/fuse
    OSXFUSE_LIBRARY_DIR := $(OSXFUSE_ROOT)/lib
    LIBS_OSXFUSE := -losxfuse -L$(OSXFUSE_LIBRARY_DIR)

    CFLAGS_OSXFUSE := -I$(OSXFUSE_INCLUDE_DIR)
    CFLAGS_OSXFUSE += -DFUSE_USE_VERSION=26
    CFLAGS_OSXFUSE += -D_FILE_OFFSET_BITS=64
    CFLAGS_OSXFUSE += -D_DARWIN_USE_64_BIT_INODE
endif

# CCFLAG := $(CFLAGS) $(CFLAGS_OSXFUSE) -Wall
# CCOBJFLAG := $(CCFLAG) $(LIBS_OSXFUSE) -c
CCFLAG := $(CFLAGS) $(LIBS_OSXFUSE) -Wall
CCOBJFLAG := $(CFLAGS) $(CFLAGS_OSXFUSE) -Wall -c

# path marcros
BUILD_PATH := build
SRC_PATH := src
DBG_PATH := debug

# compile marcros
TARGET_NAME := zipfs
ifeq ($(OS), Windows_NT)
    TARGET_NAME := $(addsuffix .exe, $(TARGET_NAME))
else
    ifeq ($(UNAME_S), Linux)
        # do nothing
    else ifeq ($(UNAME_S), Darwin)
        # do nothing
    endif
endif
TARGET := $(BUILD_PATH)/$(TARGET_NAME)
TARGET_DEBUG := $(DBG_PATH)/$(TARGET_NAME)

# src files & obj files
SRC := $(notdir $(foreach x, $(SRC_PATH), $(wildcard $(addprefix $(x)/*, .c*))))
OBJ := $(addprefix $(BUILD_PATH)/, $(addsuffix .o, $(basename $(SRC))))
OBJ_DEBUG := $(addprefix $(DBG_PATH)/, $(addsuffix .o, $(basename $(SRC))))

# clean files list
DISTCLEAN_LIST := $(OBJ) \
	$(OBJ_DEBUG)
CLEAN_LIST := $(TARGET) \
	$(TARGET_DEBUG) \
	$(DISTCLEAN_LIST)

# non-phony targets
$(TARGET): $(OBJ)
	$(CC) $(CCFLAG) -o $@ $^

$(TARGET_DEBUG): $(OBJ_DEBUG)
	$(CC) $(CCFLAG) $(DBGFLAG) -o $@ $^

$(BUILD_PATH)/%.o: $(SRC_PATH)/%.c*
	$(CC) $(CCOBJFLAG) -o $@ $<

$(DBG_PATH)/%.o: $(SRC_PATH)/%.c*
	$(CC) $(CCOBJFLAG) $(DBGFLAG) -o $@ $<

# phony rules

# default rule
.PHONY: default
default: build

.PHONY: build
all: $(TARGET)

.PHONY: debug
debug: $(TARGET_DEBUG)

.PHONY: clean
clean:
	@echo CLEAN $(CLEAN_LIST)
	@rm -f $(CLEAN_LIST)

.PHONY: distclean
distclean:
	@echo DISTCLEAN $(DISTCLEAN_LIST)
	@rm -f $(DISTCLEAN_LIST)

.PHONY: run
run: $(TARGET)
	@./$(TARGET)

.PHONY: rundebug
rundebug: $(TARGET_DEBUG)
	@./$(TARGET_DEBUG)

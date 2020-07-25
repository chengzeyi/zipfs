# tool marcros
CC := clang
DBGFLAG := -g -DDEBUG

CCFLAG := $(CFLAGS) -Wall
CCOBJFLAG := $(CFLAGS) -Wall -c

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S), Linux)
    FUSE_NAME = fuse3
else ifeq ($(UNAME_S), Darwin)
    FUSE_NAME = osxfuse

    CCOBJFLAG += -D_FILE_OFFSET_BITS=64
    CCOBJFLAG += -D_DARWIN_USE_64_BIT_INODE
endif

FUSE_CFLAGS = $(shell pkg-config $(FUSE_NAME) --cflags)
FUSE_LIBS = $(shell pkg-config $(FUSE_NAME) --libs)

CCOBJFLAG += $(FUSE_CFLAGS)
CCFLAG += $(FUSE_LIBS)

# path marcros
BUILD_PATH := build
SRC_PATH := src
DBG_PATH := debug

# compile marcros
TARGET_NAME := zipfs
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

$(BUILD_PATH)/%.o: $(SRC_PATH)/%.c* $(BUILD_PATH)
	$(CC) $(CCOBJFLAG) -o $@ $<

$(DBG_PATH)/%.o: $(SRC_PATH)/%.c* $(DBG_PATH)
	$(CC) $(CCOBJFLAG) $(DBGFLAG) -o $@ $<

$(BUILD_PATH):
	@mkdir -p $@

$(DBG_PATH):
	@mkdir -p $@

# phony rules

# default rule
.PHONY: default
default: build

.PHONY: build
all: $(TARGET)

.PHONY: debug
debug: $(TARGET_DEBUG)

.PHONY: install
install: $(TARGET)
	@cp $(TARGET) /usr/local/bin/$(TARGET_NAME)

.PHONY: uninstall
uninstall:
	@rm -f /usr/local/bin/$(TARGET_NAME)

.PHONY: clean
clean:
	@echo CLEAN $(CLEAN_LIST)
	@rm -f $(CLEAN_LIST)

.PHONY: distclean
distclean:
	@echo DISTCLEAN $(DISTCLEAN_LIST)
	@rm -f $(DISTCLEAN_LIST)

# .PHONY: run
# run: $(TARGET)
# 	@./$(TARGET)

# .PHONY: rundebug
# rundebug: $(TARGET_DEBUG)
# 	@./$(TARGET_DEBUG)

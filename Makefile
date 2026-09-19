CC ?= gcc
PKG_CONFIG ?= pkg-config

PKGS = gtk+-3.0 sqlite3
CFLAGS += -std=c11 -Wall -Wextra -O2 $(shell $(PKG_CONFIG) --cflags $(PKGS))
LDFLAGS += $(shell $(PKG_CONFIG) --libs $(PKGS))

SRC_DIR = src
BUILD_DIR = build
SOURCES = $(wildcard $(SRC_DIR)/*.c)
OBJECTS = $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(SOURCES))

TARGET = quickcart

.PHONY: all clean run dist

all: $(TARGET)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) -o $(TARGET) $(LDFLAGS)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -rf $(BUILD_DIR) $(TARGET)

# Bundles the binary with everything it needs at runtime (images, CSS) into
# dist/ — this is what you'd zip up / hand to someone else, or feed into an
# OS-specific installer (see README.md).
dist: $(TARGET)
	rm -rf dist
	mkdir -p dist/images
	cp $(TARGET) dist/
	cp style.css dist/
	cp images/*.jpg dist/images/
	@echo "Packaged into ./dist — copy that whole folder to deploy."

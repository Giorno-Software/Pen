CC = gcc
CFLAGS = -Wall -Wextra -std=c17 $(shell pkg-config --cflags raylib)
LIBS = $(shell pkg-config --libs raylib) -lm

TARGET = pen
BUILD = build

SRC = src/main.c src/tinyfiledialogs.c
OBJ = $(patsubst src/%.c,$(BUILD)/%.o,$(SRC))

$(TARGET): $(OBJ)
	$(CC) $(OBJ) -o $(TARGET) $(LIBS)

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) -c $< -o $@ $(CFLAGS)

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD) $(TARGET)


PREFIX ?= /usr/local

install: $(TARGET)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 0755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/pen

	install -d $(DESTDIR)$(PREFIX)/share/pen/assets
	cp -r assets/* $(DESTDIR)$(PREFIX)/share/pen/assets/

	install -d $(DESTDIR)$(PREFIX)/share/applications
	install -m 0644 packaging/linux/pen.desktop $(DESTDIR)$(PREFIX)/share/applications/pen.desktop

	install -d $(DESTDIR)$(PREFIX)/share/icons/hicolor/256x256/apps
	install -m 0644 assets/icons/pen-256.png $(DESTDIR)$(PREFIX)/share/icons/hicolor/256x256/apps/pen.png

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/pen
	rm -rf $(DESTDIR)$(PREFIX)/share/pen
	rm -f $(DESTDIR)$(PREFIX)/share/applications/pen.desktop
	rm -f $(DESTDIR)$(PREFIX)/share/icons/hicolor/256x256/apps/pen.png


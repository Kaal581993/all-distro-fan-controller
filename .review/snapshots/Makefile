CC = cc
CXX = c++
CPPFLAGS += -D_FORTIFY_SOURCE=3
CFLAGS ?= -O2 -g
CFLAGS += -std=c17 -Wall -Wextra -Wpedantic -Werror -fstack-protector-strong
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++17 -Wall -Wextra -Wpedantic -Werror -fstack-protector-strong -fPIC
LDFLAGS += -Wl,-z,relro,-z,now
GTK_CFLAGS := $(shell pkg-config --cflags gtk+-3.0)
GTK_LIBS := $(shell pkg-config --libs gtk+-3.0)
QT_CFLAGS := $(shell pkg-config --cflags Qt6Widgets)
QT_LIBS := $(shell pkg-config --libs Qt6Widgets)

.PHONY: all check clean artifacts
all: build/fanctl build/fan-control-gui build/fan-monitor

build:
	mkdir -p build

build/fanctl: src/fanctl.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $< -o $@

build/fan-control-gui: src/gui.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(GTK_CFLAGS) $(LDFLAGS) $< -o $@ $(GTK_LIBS)

build/fan-monitor: src/tray.cpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(QT_CFLAGS) $(LDFLAGS) $< -o $@ $(QT_LIBS)

check: all
	bash -n install.sh launch.sh review.sh tests/integration.sh
	bash tests/integration.sh
	desktop-file-validate packaging/fan-control-center.desktop
	desktop-file-validate packaging/fan-monitor.desktop

artifacts: all
	stat --printf='%n %s bytes\n' build/fanctl build/fan-control-gui build/fan-monitor
	sha256sum build/fanctl build/fan-control-gui build/fan-monitor

clean:
	rm -f build/fanctl build/fan-control-gui build/fan-monitor

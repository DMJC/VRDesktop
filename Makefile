# ---- Compiler and flags ----
CC      := gcc
CXX     := g++
CFLAGS  := -I/usr/include/wayland
CXXFLAGS := -I/usr/include/SDL2 -D_REENTRANT -I/usr/include/openvr -I/usr/include/libayatana-appindicator3-0.1 -I/usr/include/libdbusmenu-glib-0.4 -I/usr/include/giomm-2.4 -I/usr/lib/x86_64-linux-gnu/giomm-2.4/include -I/usr/include/gtkmm-3.0 -I/usr/include/gdkmm-3.0 -I/usr/lib/x86_64-linux-gnu/gdkmm-3.0/include -I/usr/include/glibmm-2.4 -I/usr/lib/x86_64-linux-gnu/glibmm-2.4/include -I/usr/include/sigc++-2.0 -I/usr/lib/x86_64-linux-gnu/sigc++-2.0/include -I/usr/include/libdbusmenu-gtk3-0.4 -I/usr/lib/x86_64-linux-gnu/gdkmm-3.0/include -I/usr/include/gtk-3.0 -I/usr/include/pango-1.0 -I/usr/include/glib-2.0 -I/usr/lib/x86_64-linux-gnu/glib-2.0/include -I/usr/include/sysprof-6 -I/usr/include/harfbuzz -I/usr/include/freetype2 -I/usr/include/libpng16 -I/usr/include/libmount -I/usr/include/blkid -I/usr/include/fribidi -I/usr/include/cairo -I/usr/include/pixman-1 -I/usr/include/gdk-pixbuf-2.0 -I/usr/include/x86_64-linux-gnu -I/usr/include/webp -I/usr/include/gio-unix-2.0 -I/usr/include/cloudproviders -I/usr/include/atk-1.0 -I/usr/include/at-spi2-atk/2.0 -I/usr/include/at-spi-2.0 -I/usr/include/dbus-1.0 -I/usr/lib/x86_64-linux-gnu/dbus-1.0/include -pthread \
-I/usr/lib/x86_64-linux-gnu/gtkmm-3.0/include -I/usr/include/giomm-2.4 -I/usr/lib/x86_64-linux-gnu/giomm-2.4/include -I/usr/include/glib-2.0 -I/usr/lib/x86_64-linux-gnu/glib-2.0/include -I/usr/include/sysprof-6 -I/usr/include/libmount -I/usr/include/blkid -I/usr/include/glibmm-2.4 -I/usr/lib/x86_64-linux-gnu/glibmm-2.4/include -I/usr/include/sigc++-2.0 -I/usr/lib/x86_64-linux-gnu/sigc++-2.0/include -I/usr/include/gtk-3.0 -I/usr/include/pango-1.0 -I/usr/include/harfbuzz -I/usr/include/freetype2 -I/usr/include/libpng16 -I/usr/include/fribidi -I/usr/include/cairo -I/usr/include/pixman-1 -I/usr/include/gdk-pixbuf-2.0 -I/usr/include/x86_64-linux-gnu -I/usr/include/webp -I/usr/include/gio-unix-2.0 -I/usr/include/cloudproviders -I/usr/include/atk-1.0 -I/usr/include/at-spi2-atk/2.0 -I/usr/include/at-spi-2.0 -I/usr/include/dbus-1.0 -I/usr/lib/x86_64-linux-gnu/dbus-1.0/include -I/usr/include/cairomm-1.0 -I/usr/lib/x86_64-linux-gnu/cairomm-1.0/include -I/usr/include/pangomm-1.4 -I/usr/lib/x86_64-linux-gnu/pangomm-1.4/include -I/usr/include/atkmm-1.6 -I/usr/lib/x86_64-linux-gnu/atkmm-1.6/include -I/usr/include/gtk-3.0/unix-print -I/usr/include/gdkmm-3.0 -I/usr/lib/x86_64-linux-gnu/gdkmm-3.0/include -pthread 


# ---- Libraries ----
LIBS := -layatana-appindicator3 -layatana-indicator3 -layatana-ido3-0.4 -ldbusmenu-glib -lgtkmm-3.0 -lwayland-client -lm -lSDL2 -lopenvr_api -ldl -lGL -lgtk-3 -lgdk-3 -lz -lpangocairo-1.0 -lpango-1.0 -lharfbuzz -latk-1.0 -lcairo-gobject -lcairo -lgdk_pixbuf-2.0 -lgio-2.0 -lgobject-2.0 -lglib-2.0 -lglibmm-2.4

# ---- Sources ----
C_SRCS  := wlr-screencopy-unstable-v1-protocol.c xdg-output-unstable-v1-protocol.c
CPP_SRCS := vrdesktop.cpp config.cpp

# ---- Objects ----
C_OBJS   := $(C_SRCS:.c=.o)
CPP_OBJS := $(CPP_SRCS:.cpp=.o)

# ---- Target ----
TARGET := vrdesktop

# ---- Default rule ----
all: $(TARGET)

# ---- Build protocol C files (.c → .o) ----
%.o: %.c
	$(CC) -c $< $(CFLAGS) -o $@

# ---- Build C++ files (.cpp → .o) ----
%.o: %.cpp
	$(CXX) -c $< $(CXXFLAGS) -o $@

# ---- Link final executable ----
$(TARGET): $(C_OBJS) $(CPP_OBJS)
	$(CXX) $^ $(LIBS) -o $@

# ---- Clean ----
clean:
	rm -f $(C_OBJS) $(CPP_OBJS) $(TARGET)

.PHONY: all clean

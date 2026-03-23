CC     = clang
CFLAGS = -Wall -Wextra -std=c11 -g -O2 -D_POSIX_C_SOURCE=200809L \
         -I/opt/homebrew/Cellar/raylib/5.5/include
LIBS   = /opt/homebrew/Cellar/raylib/5.5/lib/libraylib.a \
         -framework OpenGL -framework Cocoa -framework IOKit \
         -framework CoreVideo -framework CoreAudio
TARGET = forth
SRCS   = main.c vm.c gfx.c
OBJS   = $(SRCS:.c=.o)

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

%.o: %.c vm.h gfx.h
	$(CC) $(CFLAGS) -c $<

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET) $(OBJS)

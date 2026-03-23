CC     = clang
CFLAGS = -Wall -Wextra -std=c11 -g -O2 -D_POSIX_C_SOURCE=200809L
TARGET = forth
SRCS   = main.c vm.c
OBJS   = $(SRCS:.c=.o)

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c vm.h
	$(CC) $(CFLAGS) -c $<

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET) $(OBJS)

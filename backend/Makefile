# Makefile - 編譯 C 遊戲引擎（供 Flask subprocess 呼叫）
CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -std=c99 -O2
LDFLAGS ?= -lm

TARGET  = game_engine
OBJS    = main.o entity.o

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

main.o: main.c game.h entity.h
entity.o: entity.c entity.h game.h

clean:
	rm -f $(OBJS) $(TARGET)

# Windows MinGW 若無 rm，可手動刪除 .o 與 game_engine.exe

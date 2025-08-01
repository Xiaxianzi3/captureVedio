CC = gcc
CFLAGS = -Wall -Wextra -g
PKG_FLAGS = $(shell pkg-config --cflags --libs libavformat libavcodec libswscale libavutil)

# 目标可执行文件
TARGET = video.exe

# 源文件和头文件
SRCS = main.c video.c
OBJS = $(SRCS:.c=.o)
HEADERS = video.h

# 默认目标
all: $(TARGET)

# 链接目标文件生成可执行文件
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(PKG_FLAGS)

# 编译每个 .c 文件为 .o 文件
%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

# 清理生成的文件
clean:
	rm -f $(OBJS) 

# 声明伪目标
.PHONY: all clean
# ISKBoard 树莓派网关 Makefile
# 依赖：sudo apt install libglib2.0-dev
#       cjson 和 mosquitto 已自带在 third_party/ 中

CC       = gcc
CFLAGS   = -Wall -Wextra -Wno-unused-parameter -O2 -Isrc -Iinclude \
           -Ithird_party/cjson/include -Ithird_party/cjson/include/cjson \
           -Ithird_party/mosquitto/include \
           $(shell pkg-config --cflags gio-2.0)

LDFLAGS  = third_party/cjson/lib/libcjson.a \
           third_party/mosquitto/lib/libmosquitto.a \
           $(shell pkg-config --libs gio-2.0) -lpthread

TARGET   = isk_gateway

SRCS     = src/main.c src/ble_central.c src/comm_parse.c src/mqtt_publish.c src/log.c
OBJS     = $(SRCS:.c=.o)

$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: clean

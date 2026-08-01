# ISKBoard 树莓派网关 Makefile
# 依赖：sudo apt install libglib2.0-dev libmosquitto-dev libcjson-dev

CC       = gcc
CFLAGS   = -Wall -Wextra -O2 -Isrc -Iinclude
LDFLAGS  = -lmosquitto -lcjson -lglib-2.0 -lgio-2.0 -lgobject-2.0 -lpthread

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

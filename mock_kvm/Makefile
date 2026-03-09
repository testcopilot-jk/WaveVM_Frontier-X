CC = gcc
CFLAGS = -Wall -Wextra -O2 -fPIC -D_GNU_SOURCE -pthread
LDFLAGS = -shared -ldl -pthread

TARGET = libwvm_kvm_mock.so
SRCS = wvm_kvm_mock.c

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRCS) wvm_kvm_mock_internal.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRCS)

clean:
	rm -f $(TARGET)

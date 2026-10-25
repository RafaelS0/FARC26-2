CC = gcc
CFLAGS = -Wall -Wextra -pthread -O2
TARGET = server
OBJS = main.o server.o client_manager.o http.o qos.o config.o admission.o log.o metrics.o monitor.o
TEST_BINS = tests/test_admission tests/test_metrics

CAPACITY ?= 10000

# Declara alvos que não geram arquivos diretamente no disco
.PHONY: all run clean test test-e2e

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

run: $(TARGET)
	./$(TARGET) $(CAPACITY)

test: $(TEST_BINS)
	./tests/test_admission
	./tests/test_metrics

tests/test_admission: tests/test_admission.c admission.c config.c qos.c admission.h config.h qos.h
	$(CC) $(CFLAGS) -I. -o $@ tests/test_admission.c admission.c config.c qos.c

tests/test_metrics: tests/test_metrics.c metrics.c client_manager.c monitor.c admission.c qos.c metrics.h client_manager.h monitor.h
	$(CC) $(CFLAGS) -I. -o $@ tests/test_metrics.c metrics.c client_manager.c monitor.c admission.c qos.c -lm

test-e2e: $(TARGET)
	python3 tests/e2e_admission.py
	python3 tests/e2e_metrics.py

clean:
	rm -f $(OBJS) $(TARGET) $(TEST_BINS) server.log

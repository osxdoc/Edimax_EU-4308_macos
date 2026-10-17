CC ?= clang
CFLAGS ?= -O2 -Wall -Wextra -arch arm64 -arch x86_64
LDFLAGS = -framework IOKit -framework CoreFoundation -framework SystemConfiguration -lpthread

TARGET = edimax_driver
TOOL_MII = tools/dump_mii
PLIST = com.edimax.ax88179.driver.plist

.PHONY: all clean run tools test install uninstall

all: $(TARGET)

$(TARGET): edimax_driver.c
	$(CC) $(CFLAGS) $< $(LDFLAGS) -o $@

tools: $(TOOL_MII)

$(TOOL_MII): tools/dump_mii.c
	$(CC) $(CFLAGS) $< $(LDFLAGS) -o $@

run: $(TARGET)
	sudo ./$(TARGET)

install: $(TARGET)
	sudo /bin/bash tools/service.sh install

uninstall: $(TARGET)
	sudo /bin/bash tools/service.sh uninstall

test:
	$(CC) -O1 -g -Wall -Wextra -fsanitize=address,undefined tests/test_driver.c $(LDFLAGS) -o tests/test_driver
	./tests/test_driver
	bash tests/test_service.sh
	bash -n Install.command Uninstall.command tools/service.sh tests/test_service.sh
	plutil -lint services/$(PLIST)

clean:
	rm -f $(TARGET) $(TOOL_MII) tests/test_driver

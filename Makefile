CC ?= clang
VERSION := $(shell cat VERSION)
CPPFLAGS += -DEDIMAX_VERSION='"$(VERSION)"'
CFLAGS ?= -O2 -Wall -Wextra -Werror=unguarded-availability -mmacosx-version-min=14.0 -arch arm64 -arch x86_64
LDFLAGS = -framework IOKit -framework CoreFoundation -framework SystemConfiguration -lpthread

TARGET = edimax_driver
TOOL_MII = tools/dump_mii
PLIST = com.edimax.ax88179.driver.plist

.PHONY: all clean run tools test package install uninstall

all: $(TARGET)

$(TARGET): edimax_driver.c Makefile VERSION
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LDFLAGS) -o $@

tools: $(TOOL_MII)

$(TOOL_MII): tools/dump_mii.c Makefile
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
	bash tests/test_package.sh
	bash -n Install.command Uninstall.command tools/*.sh tests/*.sh
	plutil -lint services/$(PLIST)

package: $(TARGET)
	/bin/bash tools/package.sh

clean:
	rm -f $(TARGET) $(TOOL_MII) tests/test_driver

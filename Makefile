CC ?= clang
VERSION := $(shell cat VERSION)
CPPFLAGS += -DEDIMAX_VERSION='"$(VERSION)"'
CFLAGS ?= -O2 -Wall -Wextra -Werror=unguarded-availability -mmacosx-version-min=14.0 -arch arm64 -arch x86_64
LDFLAGS = -framework IOKit -framework CoreFoundation -framework SystemConfiguration -lpthread

TARGET = edimax_driver
TOOL_MII = tools/dump_mii
PLIST = com.edimax.ax88179.driver.plist
TESTS = tests/test_driver tests/test_lifecycle tests/test_network tests/test_diagnostic

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

tests/%: tests/%.c edimax_driver.c tools/dump_mii.c
	$(CC) -O1 -g -Wall -Wextra -fsanitize=address,undefined $< $(LDFLAGS) -o $@

test: $(TESTS)
	@for test in $(TESTS); do ./$$test || exit $$?; done
	bash tests/test_service.sh
	bash tests/test_package.sh
	bash -n Install.command Uninstall.command tools/*.sh tests/*.sh
	plutil -lint services/$(PLIST)

package: $(TARGET)
	/bin/bash tools/package.sh

clean:
	rm -f $(TARGET) $(TOOL_MII) $(TESTS)

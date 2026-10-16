CC ?= clang
CFLAGS ?= -O2 -Wall -Wextra -arch arm64 -arch x86_64
LDFLAGS = -framework IOKit -framework CoreFoundation -lpthread

TARGET = edimax_driver
TOOL_MII = tools/dump_mii
PLIST = com.edimax.ax88179.driver.plist

.PHONY: all clean run tools install uninstall

all: $(TARGET)

$(TARGET): edimax_driver.c
	$(CC) $(CFLAGS) $< $(LDFLAGS) -o $@

tools: $(TOOL_MII)

$(TOOL_MII): tools/dump_mii.c
	$(CC) $(CFLAGS) $< $(LDFLAGS) -o $@

run: $(TARGET)
	sudo ./$(TARGET)

install: $(TARGET)
	@echo "==> Stopping any existing driver instance..."
	-sudo launchctl bootout system services/$(PLIST) 2>/dev/null || true
	-sudo launchctl bootout system /Library/LaunchDaemons/$(PLIST) 2>/dev/null || true
	-sudo launchctl unload /Library/LaunchDaemons/$(PLIST) 2>/dev/null || true
	-sudo killall -9 $(TARGET) 2>/dev/null || true
	@echo "==> Installing binary to /usr/local/bin..."
	sudo mkdir -p /usr/local/bin
	sudo cp -f $(TARGET) /usr/local/bin/$(TARGET)
	sudo chmod 755 /usr/local/bin/$(TARGET)
	@echo "==> Installing launchd service..."
	sudo cp services/$(PLIST) /Library/LaunchDaemons/$(PLIST)
	sudo chown root:wheel /Library/LaunchDaemons/$(PLIST)
	sudo chmod 644 /Library/LaunchDaemons/$(PLIST)
	sudo launchctl bootstrap system /Library/LaunchDaemons/$(PLIST) 2>/dev/null || sudo launchctl load /Library/LaunchDaemons/$(PLIST)
	@echo "==> Service installed and active! Check logs with: tail -f /var/log/edimax_driver.log"

uninstall:
	@echo "==> Stopping and unloading launchd service..."
	-sudo launchctl bootout system/$(PLIST) 2>/dev/null || -sudo launchctl unload /Library/LaunchDaemons/$(PLIST) 2>/dev/null
	sudo rm -f /Library/LaunchDaemons/$(PLIST)
	@echo "==> Removing binary..."
	sudo rm -f /usr/local/bin/$(TARGET)
	@echo "==> Uninstalled successfully."

clean:
	rm -f $(TARGET) $(TOOL_MII)

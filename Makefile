# Compiler and linker
CC = gcc

# User Creation
INSTALL_USER = cis
INSTALL_GROUP = cis
SYSLOG_GROUP = syslog

# Directories
SRC_DIR = src
OBJ_DIR = obj
BIN_DIR = bin
INCLUDE_DIR = include
LIB_DIR = /var/lib/cis

# Output executable
TARGET = $(BIN_DIR)/code_issue_service

# Source files
SRCS = $(wildcard $(SRC_DIR)/*.c)

# Object files
OBJS = $(patsubst $(SRC_DIR)/%.c, $(OBJ_DIR)/%.o, $(SRCS))

# Compiler flags
CFLAGS = -I$(INCLUDE_DIR) -Wall -Wextra -O2 -pthread

# Libraries
LIBS = -lmicrohttpd -lhiredis -lgit2 -lcurl -linih -lcjson -lpthread -ldl -lrt

# Systemd service file
SERVICE_FILE = code_issue_service.service

# Config file
CONFIG_DIR = /etc
CONFIG_FILE = code_issue_service.conf

# Installation directories
PREFIX = /usr/local
BINDIR = $(PREFIX)/bin
SYSTEMD_DIR = /etc/systemd/system

# Default target
all: $(TARGET)

# Create directories if they don't exist
$(BIN_DIR):
	@mkdir -p $(BIN_DIR)

$(OBJ_DIR):
	@mkdir -p $(OBJ_DIR)

# Compile source files into object files
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Link object files into the final executable
$(TARGET): $(OBJS) | $(BIN_DIR)
	$(CC) $(OBJS) -o $(TARGET) $(LIBS)

# Install the executable and systemd service
install: all
	# Install the executable
	@echo "Installing executable to $(BINDIR)"
	install -d $(BINDIR)
	install -m 755 $(TARGET) $(BINDIR)
	# Create CIS user and group
	@echo "Creating CIS user and group..."
	@if ! id -u $(INSTALL_USER) >/dev/null 2>&1; then \
		echo "User $(INSTALL_USER) does not exist. Creating..."; \
		groupadd -r $(INSTALL_GROUP); \
		useradd -r -g $(INSTALL_GROUP) -s /sbin/nologin -d $(LIB_DIR) $(INSTALL_USER); \
	else \
		echo "User $(INSTALL_USER) already exists."; \
	fi
	# Set up /var/lib directory
	@echo "Setting up /var/lib directory..."
	@mkdir -p $(LIB_DIR)
	@chown $(INSTALL_USER):$(INSTALL_GROUP) $(LIB_DIR)
	@chmod 0750 $(LIB_DIR)
	# Grant CIS user access to syslog group
	@echo "Granting CIS user access to syslog group..."
	@usermod -aG $(SYSLOG_GROUP) $(INSTALL_USER)
	# Install service file
	@echo "Installing service file to $(SYSTEMD_DIR)"
	install -m 644 $(SERVICE_FILE) $(SYSTEMD_DIR)/$(SERVICE_FILE)
	# Install config file
	@echo "Installing config file to $(CONFIG_DIR)"
	install -m 640 $(CONFIG_FILE) $(CONFIG_DIR)/$(CONFIG_FILE)
	@chown root:$(INSTALL_GROUP) $(CONFIG_DIR)/$(CONFIG_FILE)
	@echo "Installation complete."
	@echo "Please edit $(CONFIG_DIR)/$(CONFIG_FILE)."
	@echo "If you would like CIS to run as a system service,"
	@echo "be sure to ENABLE and START code_issue_service.service"

# Uninstall the service and remove installed files
uninstall:
	@echo "Stopping and disabling the service..."
	-systemctl stop $(SERVICE_FILE)
	-systemctl disable $(SERVICE_FILE)
	@echo "Removing service file..."
	@rm -f $(SYSTEMD_DIR)/$(SERVICE_FILE)
	@echo "Reloading systemd daemon..."
	-systemctl daemon-reload
	@echo "Removing executable..."
	@rm -f $(BINDIR)/$(notdir $(TARGET))
	@echo "Removing configuration file..."
	@rm -f $(CONFIG_DIR)/$(CONFIG_FILE)
	@echo "Removing /var/lib directory..."
	@rm -rf $(LIB_DIR)
	@echo "Removing CIS user from syslog group..."
	-gpasswd -d $(INSTALL_USER) $(SYSLOG_GROUP)
	@echo "Removing CIS user and group..."
	-userdel $(INSTALL_USER)
	-groupdel $(INSTALL_GROUP)
	@echo "Uninstallation complete."

# Clean up generated files
clean:
	rm -rf $(OBJ_DIR) $(BIN_DIR)

.PHONY: all install uninstall clean


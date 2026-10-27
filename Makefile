obj-m	:= hid-shanwan.o
PWD	:= $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

TOOLCHAIN :=
ARCH := x86
KVER := $(shell uname -r)
KDIR := /lib/modules/$(KVER)/build
TCPATH :=

# Only touch PATH for a cross toolchain: "$(TCPATH):$(PATH)" with an empty
# TCPATH starts with ':', which makes the shell search the current
# directory first, also under "sudo make install".
ifneq ($(strip $(TCPATH)),)
export PATH := $(TCPATH):$(PATH)
endif

MODULE := hid-shanwan
INSTALL_DIR := /lib/modules/$(KVER)/extra
# present while the module is loaded
MODULE_SYSFS := /sys/module/$(subst -,_,$(MODULE))

# module options; an existing copy is kept on reinstall, so local changes survive
CONF := $(MODULE).conf
CONF_DIR := /etc/modprobe.d
# sha256 of $(CONF) as earlier versions shipped it (329ab87: strong_boost and
# weak_threshold at 0). An installed copy that matches one has no local
# changes: install replaces it and uninstall removes it. Add the old sum here
# whenever $(CONF) changes.
CONF_OLD := 823db0c6f189f90758d82581d60f39a51927b22829f4235e34ea9d0276303792
# shell condition: file $(1) is an unchanged copy of an earlier $(CONF)
conf_old = sha256sum < $(1) | cut -d' ' -f1 | grep -qxF $(addprefix -e ,$(CONF_OLD))

# Secure Boot signing key, created once with: sudo kmodgenca -a
SIGN_KEY := /etc/pki/akmods/private/private_key.priv
SIGN_CERT := /etc/pki/akmods/certs/public_key.der

.PHONY: all clean install uninstall

all:
	ARCH=$(ARCH) CROSS_COMPILE=$(TOOLCHAIN) $(MAKE) -C $(KDIR) M=$(PWD) modules
clean:
	ARCH=$(ARCH) CROSS_COMPILE=$(TOOLCHAIN) $(MAKE) -C $(KDIR) M=$(PWD) clean

# Run "make" as a normal user first, then "sudo make install".
# The installed copy is signed; the module in this directory is left untouched.
install:
	@test "$$(id -u)" -eq 0 || { echo "run as root: sudo make install"; exit 1; }
	@test -f $(MODULE).ko || { echo "$(MODULE).ko not found, run 'make' first"; exit 1; }
	@test $(MODULE).ko -nt $(MODULE).c || { echo "$(MODULE).ko is older than $(MODULE).c, run 'make' first"; exit 1; }
	@kv=$$(modinfo -F vermagic $(MODULE).ko 2>/dev/null | cut -d' ' -f1); \
	test -n "$$kv" || { echo "cannot read the vermagic of $(MODULE).ko (modinfo)"; exit 1; }; \
	test "$$kv" = "$(KVER)" || { \
		echo "$(MODULE).ko is built for $$kv, the running kernel is $(KVER): run 'make clean && make'"; \
		exit 1; }
	@test -f $(SIGN_KEY) -a -f $(SIGN_CERT) || { \
		echo "Signing key not found. Create and enroll it once:"; \
		echo "  sudo kmodgenca -a"; \
		echo "  sudo mokutil --import $(SIGN_CERT)"; \
		echo "  reboot and choose 'Enroll MOK'"; \
		exit 1; }
	install -v -D -m 644 $(MODULE).ko $(INSTALL_DIR)/$(MODULE).ko
	$(KDIR)/scripts/sign-file sha256 $(SIGN_KEY) $(SIGN_CERT) $(INSTALL_DIR)/$(MODULE).ko
	@echo "installed: vermagic $$(modinfo -F vermagic $(INSTALL_DIR)/$(MODULE).ko)"
	@echo "installed: signer $$(modinfo -F signer $(INSTALL_DIR)/$(MODULE).ko)"
	depmod -a $(KVER)
	@if [ ! -e $(CONF_DIR)/$(CONF) ]; then \
		install -v -m 644 $(CONF) $(CONF_DIR)/$(CONF); \
	elif cmp -s $(CONF) $(CONF_DIR)/$(CONF); then \
		:; \
	elif $(call conf_old,$(CONF_DIR)/$(CONF)); then \
		echo "replacing $(CONF_DIR)/$(CONF), an unchanged copy of an earlier version"; \
		install -v -m 644 $(CONF) $(CONF_DIR)/$(CONF); \
	else \
		echo "keeping $(CONF_DIR)/$(CONF), it has local changes; compare: diff $(CONF_DIR)/$(CONF) $(CONF)"; \
	fi
	@# an already loaded module would keep running the old code
	@if [ -d $(MODULE_SYSFS) ]; then \
		echo "$(MODULE) is loaded, reloading it"; \
		modprobe -r $(MODULE) || { echo "cannot unload $(MODULE), run 'sudo make uninstall' first"; exit 1; }; \
	fi
	@modprobe $(MODULE) || { \
		echo "modprobe failed. 'Key was rejected by service' means the key is not enrolled:"; \
		echo "  sudo mokutil --import $(SIGN_CERT)"; \
		echo "  reboot and choose 'Enroll MOK'"; \
		echo "'Invalid argument' with 'invalid for parameter' in dmesg means a bad value in $(CONF_DIR)/$(CONF)"; \
		exit 1; }
	@echo "$(MODULE) installed to $(INSTALL_DIR) and loaded"
	@echo "parameters: weak_pwm=$$(cat $(MODULE_SYSFS)/parameters/weak_pwm)" \
		"weak_pwm_period=$$(cat $(MODULE_SYSFS)/parameters/weak_pwm_period)" \
		"weak_pwm_min=$$(cat $(MODULE_SYSFS)/parameters/weak_pwm_min)" \
		"strong_min=$$(cat $(MODULE_SYSFS)/parameters/strong_min)"

# Removes the module from every installed kernel. The MOK key is kept, remove it with:
#   sudo mokutil --delete $(SIGN_CERT)
uninstall:
	@test "$$(id -u)" -eq 0 || { echo "run as root: sudo make uninstall"; exit 1; }
	-modprobe -r $(MODULE)
	@for f in /lib/modules/*/extra/$(MODULE).ko; do \
		[ -e "$$f" ] || continue; \
		rm -v "$$f"; \
		kver=$$(echo "$$f" | cut -d/ -f4); \
		depmod -a "$$kver"; \
	done
	@if [ -e $(CONF_DIR)/$(CONF) ]; then \
		if cmp -s $(CONF) $(CONF_DIR)/$(CONF) || $(call conf_old,$(CONF_DIR)/$(CONF)); then \
			rm -v $(CONF_DIR)/$(CONF); \
		else \
			echo "keeping $(CONF_DIR)/$(CONF), it has local changes"; \
		fi; \
	fi
	@echo "$(MODULE) uninstalled"

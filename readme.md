# This is a kernel module for the specific (2563:0575)Shanwan USB WirelessGamepad gamepad.

Particularly the contoller in the picture.
![image](https://raw.githubusercontent.com/hbiyik/hid-shanwan/master/meta/shanwan_usb_wirelessgamepad.png)

I have created this module as a result of my fun weekend reverse engineering project, and decided to log my findings in case someone else finds it usefull for another type of device. All of the information here is my personal itnerperetation there may be few things not addressed correctly in theory, but the module works just fine as per my testing in 2 different kernels (4.19 / 5.13)

# How this device works:

There is actually 2 different device behaviour in this gamepad, first is the normal as expected one, which i call "Rich Mode", and then the fallback mode "Poor Mode".
These modes are not anything official (if there is something official about this device) they are just the names i gave them to describe the device behaviour. When the device receives a HID / USB request from the host that it does not understand, it falls back to [poor mode](https://github.com/hbiyik/hid-shanwan/blob/master/meta/hid_desc_poor.txt) from [rich mode](https://github.com/hbiyik/hid-shanwan/blob/master/meta/hid_desc_rich.txt), providing a totally different HID descriptor, different  USB Vendor ID & Product ID (20BC:0055) by disconnecting itself from the USB host and reconnecting again. The funny thing is both 20BC & 2563 belong to Shanwan as vendor ID. You can observe those behaviour better when you analyze the [wireshark captures](https://raw.githubusercontent.com/hbiyik/hid-shanwan/master/meta/shanwan-kernel4_19.pcapng) in meta folder

The difference between poor mode and rich mode is, in poor mode, force feedback (vibration), enable/disable analog (mode) buttons, L3/R3 pressure sensots do not work. This module enables all of those features and remaps the kepmap according to [Gamepad protocol](https://www.kernel.org/doc/html/latest/input/gamepad.html) defined by the kernel.

# Different Behaviours in Different Linux Kernels
 If you are using a kernel version which is old (i tested with 4.19) which is prior to this [commit](https://github.com/torvalds/linux/commit/9143059fafd4eebed2d43ffb5455178d4010e60a), the usbhid in kernel by default reads all the initial values presented in OUTPUT/FEATURE reports of the HID descriptor. After the mentioned commit this initialization step is disabled in the kernel, but for older kernel, the read request of reports is not understood by the device, therefore it falls back to poor mode. When the device falls back to poor mode, the device is auto recognzed by the hid-betopff existing module, since the new usb vid:pid and the new descriptor matches, however hid-betopff module does not acutally add anything new to to the existing hid-generic module, does not enable force feedback etc. In such a kernel, 'HID_QUIRK_NO_INIT_REPORTS' must be enabled in the usb-core module of the kernel so that it wont try to send not understood commands to the device, and therefore the module can not directly be used as loadable module, because now the usb core module needs to be updated.
 
 Another issue with the old kernels, specific to prior this [commit](https://github.com/torvalds/linux/commit/e04a0442d33b8cf183bba38646447b891bb02123), the hid modules needs to be specifically mentioned to have a dedicated driver in the usb-core module, otherwise hid-generic module will load and the custom module (hid-shanwan) will not load even though though vid:pid and the descriptor matches. This is the second reason you need to integrate this module the usb core and loading this as an extra module basically with modprobe will not work.
 
 In a newer kernel (tested with 5.13) none of the above issue are a problem, you directly insmod as external module and the module will detect the gamepad. For how to integrate to an old kernel, you can check the [patch prepared for kernel 4.19](https://github.com/hbiyik/hid-shanwan/blob/master/meta/hid_shanwan_419.patch).
 
# Upstream Kernel Support

With the current state, the module is fine to be submitted to mainline kernel, but i actually have never done this before, the kernel people may have their own set of requirements to accept PRs and mailing lists etc... If someone else takes this up and submits to mainline kernel, i would just appreciate it.
 
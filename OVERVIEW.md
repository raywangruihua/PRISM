# Overview

This document provides a quick overview and introduction to the technology used in this project.

## OP-TEE: Open Portable Trusted Execution Environment

[OP-TEE NVIDIA Jetson Linux Developer Guide](https://docs.nvidia.com/jetson/archives/r39.2/DeveloperGuide/SD/Security/OpTee.html)

OP-TEE is a Trusted Execution Environment (TEE) designed as a companion to Linux kernels running on ARM processors with ARM TrustZone [1]. In brief, OP-TEE organises software into the Normal world and Trusted world. The main OS and other user applications run in the Normal world, while Trusted Applications (TAs) run in the secure world and are called by Client Applications (CAs) running in the Normal world.

> [!NOTE]
> OP-TEE trusted applications (TAs) are written in C, while client applications (CAs) can be written in Cpp.

### ARM TrustZone

ARM TrustZone is the underlying technology that enables OP-TEE by providing hardware isolation to segregate the Normal and Trusted world [2]. Essentially, the CPU can either be in the Secure state or Non-secure state, which is determined by the Secure Monitor. When a TA is called in a Non-secure state, an exception is raised as a fast interrupt request or a secure monitor call. This triggers the corresponding interrupt handler in the Secure Monitor to save the complete current Non-secure CPU register state and restore the previous Secure CPU register state. Similarly, when a TA finishes execution, the reverse occurs.

![ARM TrustZone architecture](/images/ARMTrustZoneArchitecture.svg)

In addition to Security states, ARM TrustZone also implements 2 physical address spaces: Secure and Non-secure [2]. Software in the Non-secure state can only access Non-secure memory, while Secure state software can access Secure and Non-secure memory. A NS bit determines whether virtual memory tranlsates to Secure or Non-secure memory via the translation table.

ARM TrustZone is supported by the Jetson Xavier series onwards, including Jetson Orin Nano.

## Secure I/O Channels

Secure I/O channels are pathways in the data pipelines designed to prevent modification of sensor data in any way during transport. Data is then processed by the TEE depending on sensitivity.

### Astra Pro Plus

The Astra Pro Plus is only supported by Orbbec SDK v1 (v2 devices use the UVC protocol instead). The Orbbec SDK architecture diagram can be seen below. Data from the Astra Pro Plus camera device is first handled via the USB protocol, which is implemented as `usbcore` on Linux.

![Orbbec SDK architecture diagram](/images/OrbbecSDK-Architecture.png)

A quick walkthrogh of the Linux USB protocol. Linux uses USB Request Blocks (URBs) which information about transfers between a USB device and the USB host[3]. It has a `transfer_buffer` field which contains a virtual CPU address to a buffer on the host, in which a USB transfer request is performed unless `URB_NO_TRANSFER_DMA_MAP` is set. If `URB_NO_TRANSFER_DMA_MAP` is set, a physical DMA address to a buffer on the host in `transfer_dma` field is used instead. When `transfer_buffer_length` is set to 0, neither `transfer_buffer` nor `transfer_dma` is used. The USB device/host driver creates a URB and sends it to the USB host/device driver, which then polls the USB device for data or sends data to the USB device. All memory operations occur on the host system.

When `transfer_buffer` is used, the buffer is usually allocated via `kmalloc()` or taken from the general page pool by the device driver[3]. While `transfer_dma` allocates a DMA buffer with `usb_alloc_coherent()` or call `usb_buffer_map()` by the device driver. Since the Linux OS operates in the Non-secure state, both approaches land the transferred data in the Non-secure. This means that the data is exposed to a Non-secure OS before senstive information can be removed in the TEE.

To resolve this, the data must be handled in the Secure state by an OP-TEE USB driver. To accommodate for higher runtime memory requirements of an additional USB stack alongside OP-TEE OS, the Secure memory or TrustZone-Protected DRAM (TZDRAM) can be configured to be larger in the bootloader configuration files with no limits (the documentation on this is practically non-existent). Alternatively, data can be encrypted by a hardware middleman between the device and the host, then decrypted by a TA in the Secure state. The hardware middleman approach was used for this project.

## References

[1] ‘About OP-TEE — OP-TEE documentation documentation’. Accessed: Sep. 14, 2026. [Online]. Available: https://optee.readthedocs.io/en/latest/general/about.html

[2] ‘Learn the architecture - TrustZone for AArch64’. Accessed: Sep. 14, 2026. [Online]. Available: https://support.arm.com/documentation/102418/0102

[3] ‘The Linux-USB Host Side API — The Linux Kernel documentation’. Accessed: Sep. 02, 2026. [Online]. Available: https://docs.kernel.org/driver-api/usb/usb.html

[4] ‘Device Tree Technical Overview | Toradex Developer Center’. Accessed: Sep. 03, 2026. [Online]. Available: https://developer.toradex.com/software/linux-resources/device-tree/device-tree-overview/

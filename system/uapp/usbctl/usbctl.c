// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <magenta/device/usb-device.h>
#include <magenta/device/usb-virt-bus.h>
#include <magenta/hw/usb-cdc.h>

#include <magenta/types.h>

#define DEV_VIRTUAL_USB "/dev/misc/usb-virtual-bus"
#define DEV_USB_DEVICE_DIR  "/dev/class/usb-device"

#define GOOGLE_VID      0x18D1
#define GOOGLE_CDC_PID  0xA020
#define GOOGLE_UMS_PID  0xA021

enum {
    MANUFACTURER_INDEX = 1,
    PRODUCT_INDEX,
    SERIAL_INDEX,
};

static const usb_device_string_t manufacturer_string = {
    .index = MANUFACTURER_INDEX,
    .string = "Magenta",
};

static const usb_device_string_t cdc_product_string = {
    .index = PRODUCT_INDEX,
    .string = " CDC Ethernet",
};

static const usb_device_string_t ums_product_string = {
    .index = PRODUCT_INDEX,
    .string = "USB Mass Storage",
};

static const usb_device_string_t serial_string = {
    .index = SERIAL_INDEX,
    .string = "12345678",
};

#define USB_STRLEN(s) (sizeof(usb_device_string_t) + strlen((s)->string) + 1)

const usb_function_descriptor_t cdc_function_desc = {
    .interface_class = USB_CLASS_COMM,
    .interface_subclass = USB_CDC_SUBCLASS_ETHERNET,
    .interface_protocol = 0,
};

const usb_function_descriptor_t ums_function_desc = {
    .interface_class = USB_CLASS_MSC,
    .interface_subclass = USB_SUBCLASS_MSC_SCSI,
    .interface_protocol = USB_PROTOCOL_MSC_BULK_ONLY,
};

typedef struct {
    const usb_function_descriptor_t* desc;
    const usb_device_string_t* product_string;
    uint16_t vid;
    uint16_t pid;
} usb_function_t;

static const usb_function_t cdc_function = {
    .desc = &cdc_function_desc,
    .product_string = &cdc_product_string,
    .vid = GOOGLE_VID,
    .pid = GOOGLE_CDC_PID,
};

static const usb_function_t ums_function = {
    .desc = &ums_function_desc,
    .product_string = &ums_product_string,
    .vid = GOOGLE_VID,
    .pid = GOOGLE_UMS_PID,
};

static usb_device_descriptor_t device_desc = {
    .bLength = sizeof(usb_device_descriptor_t),
    .bDescriptorType = USB_DT_DEVICE,
    .bcdUSB = htole16(0x0200),
    .bDeviceClass = 0,
    .bDeviceSubClass = 0,
    .bDeviceProtocol = 0,
    .bMaxPacketSize0 = 64,
//    idVendor and idProduct filled in later
    .bcdDevice = htole16(0x0100),
    .iManufacturer = MANUFACTURER_INDEX,
    .iProduct = PRODUCT_INDEX,
    .iSerialNumber = SERIAL_INDEX,
    .bNumConfigurations = 1,
};

static int open_usb_device(void) {
    struct dirent* de;
    DIR* dir = opendir(DEV_USB_DEVICE_DIR);
    if (!dir) {
        printf("Error opening %s\n", DEV_USB_DEVICE_DIR);
        return -1;
    }

    while ((de = readdir(dir)) != NULL) {
       char devname[128];

        snprintf(devname, sizeof(devname), "%s/%s", DEV_USB_DEVICE_DIR, de->d_name);
        int fd = open(devname, O_RDWR);
        if (fd < 0) {
            printf("Error opening %s\n", devname);
            continue;
        }

        closedir(dir);
        return fd;
    }

    closedir(dir);
    return -1;
}

static mx_status_t device_init(int fd, const usb_function_t* function) {
    device_desc.idVendor = htole16(function->vid);
    device_desc.idProduct = htole16(function->pid);

    // set device descriptor
    mx_status_t status = ioctl_usb_device_set_device_desc(fd, &device_desc);
    if (status < 0) {
        fprintf(stderr, "ioctl_usb_device_set_device_desc failed: %d\n", status);
        return status;
    }

    // set string descriptors
    status = ioctl_usb_device_set_string_desc(fd, &manufacturer_string,
                                              USB_STRLEN(&manufacturer_string));
    if (status < 0) {
        fprintf(stderr, "ioctl_usb_device_set_string_desc failed: %d\n", status);
        return status;
    }
    status = ioctl_usb_device_set_string_desc(fd, function->product_string,
                                              USB_STRLEN(function->product_string));
    if (status < 0) {
        fprintf(stderr, "ioctl_usb_device_set_string_desc failed: %d\n", status);
        return status;
    }
    status = ioctl_usb_device_set_string_desc(fd, &serial_string,
                                              USB_STRLEN(&serial_string));
    if (status < 0) {
        fprintf(stderr, "ioctl_usb_device_set_string_desc failed: %d\n", status);
        return status;
    }

    status = ioctl_usb_device_add_function(fd, function->desc);
    if (status < 0) {
        fprintf(stderr, "ioctl_usb_device_add_function failed: %d\n", status);
        return status;
    }

    status = ioctl_usb_device_bind_functions(fd);
    if (status < 0) {
        fprintf(stderr, "ioctl_usb_device_bind_functions failed: %d\n", status);
    }

    return status;
}

static int device_command(int argc, const char** argv) {
    int fd = open_usb_device();
    if (fd < 0) {
        fprintf(stderr, "could not find a device in %s\n", DEV_USB_DEVICE_DIR);
        return fd;
    }

    if (argc != 2) {
        goto usage;
    }

    mx_status_t status = MX_OK;
    const char* command = argv[1];
    if (!strcmp(command, "reset")) {
        status = ioctl_usb_device_clear_functions(fd);
    } else if (!strcmp(command, "init-cdc")) {
        status = device_init(fd, &cdc_function);
    } else if (!strcmp(command, "init-ums")) {
        status = device_init(fd, &ums_function); 
     } else {
        goto usage;
    }

    close(fd);
    return status == MX_OK ? 0 : -1;

usage:
    close(fd);
    fprintf(stderr, "usage: usbctl device [reset|init-ums]\n");
    return -1;
}

static int virtual_command(int argc, const char** argv) {
    int fd = open(DEV_VIRTUAL_USB, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "could not open %s\n", DEV_VIRTUAL_USB);
        return fd;
    }

    if (argc != 2) {
        goto usage;
    }

    mx_status_t status = MX_OK;
    const char* command = argv[1];
    if (!strcmp(command, "enable")) {
        int enabled = 1;
        status = ioctl_usb_virt_bus_enable(fd, &enabled);
    } else if (!strcmp(command, "disable")) {
        int enabled = 0;
        status = ioctl_usb_virt_bus_enable(fd, &enabled);
    } else if (!strcmp(command, "connect")) {
        int connected = 1;
        status = ioctl_usb_virt_bus_set_connected(fd, &connected);
    } else if (!strcmp(command, "disconnect")) {
        int connected = 0;
        status = ioctl_usb_virt_bus_set_connected(fd, &connected);
    } else {
        goto usage;
    }

    close(fd);
    return status == MX_OK ? 0 : -1;

usage:
    close(fd);
    fprintf(stderr, "usage: usbctl virtual [enable|disable|connect|disconnect]\n");
    return -1;
}

typedef struct {
    const char* name;
    int (*command)(int argc, const char* argv[]);
    const char* description;
} usbctl_command_t;

static usbctl_command_t commands[] = {
    {
        "device",
        device_command,
        "device [reset|init-cdc|init-ums] resets the device or "
        "initializes the UMS function"
    },
    {
        "virtual",
        virtual_command,
        "virtual [enable|disable|connect|disconnect] - controls USB virtual bus"
    },
    { NULL, NULL, NULL },
};

static void usage(void) {
    fprintf(stderr, "usage: \"usbctl <command>\", where command is one of:\n");

    usbctl_command_t* command = commands;
    while (command->name) {
        fprintf(stderr, "    %s\n", command->description);
        command++;
    }
}

int main(int argc, const char** argv) {
    if (argc < 2) {
        usage();
        return -1;
    }

    const char* command_name = argv[1];
    usbctl_command_t* command = commands;
    while (command->name) {
        if (!strcmp(command_name, command->name)) {
            return command->command(argc - 1, argv + 1);
        }
        command++;
    }

    usage();
    return -1;
}

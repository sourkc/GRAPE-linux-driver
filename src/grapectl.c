#include <endian.h>
#include <errno.h>
#include <inttypes.h>
#include <libusb-1.0/libusb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gfxlink_protocol.h"
#include "grapectl_transport.h"
#include "grapectl_commands.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        grapectl_usage(argv[0]);
        return 2;
    }

    libusb_context *usb = NULL;
    if (libusb_init(&usb) != 0) return 1;
    libusb_device_handle *device = libusb_open_device_with_vid_pid(usb, GFXLINK_USB_VID, GFXLINK_USB_PID);
    if (!device) {
        fprintf(stderr, "GRAPE %04x:%04x not found\n", GFXLINK_USB_VID, GFXLINK_USB_PID);
        libusb_exit(usb);
        return 1;
    }
    int ret = libusb_claim_interface(device, GFXLINK_USB_INTERFACE);
    if (ret != 0) {
        fprintf(stderr, "Unable to claim GFXLINK interface: %s\n", libusb_error_name(ret));
        libusb_close(device);
        libusb_exit(usb);
        return 1;
    }

    int result = grapectl_dispatch(device, argc, argv);
    libusb_release_interface(device, GFXLINK_USB_INTERFACE);
    libusb_close(device);
    libusb_exit(usb);
    return result;
}

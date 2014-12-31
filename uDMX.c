/*
 * Name: uDMX.c
 * Project: Commandline utility for uDMX interface
 * Author: Markus Baertschi
 * Based on work from: Christian Starkjohann, 2005-01-16
 *
 * Creation Date: 2005-01-16
 * Tabsize: 4
 * Copyright: (c) 2005 by OBJECTIVE DEVELOPMENT Software GmbH
 * License: Proprietary, free under certain conditions. See Documentation.
 * This Revision: $Id: uDMX.c,v 1.1.1.1 2006/02/15 17:55:06 cvs Exp $
 */

/*
General Description:
This program controls the PowerSwitch USB device from the command line.
It must be linked with libusb, a library for accessing the USB bus from
Linux, FreeBSD, Mac OS X and other Unix operating systems. Libusb can be
obtained from http://libusb.sourceforge.net/.
*/

#define UDMXVERSION "1.0"

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <usb.h> /* this is libusb, see http://libusb.sourceforge.net/ */
#include "uDMX_cmds.h"

#define USBDEV_SHARED_VENDOR 0x16C0  /* Obdev's free shared VID */
#define USBDEV_SHARED_PRODUCT 0x05DC /* Obdev's free shared PID */
/* Use obdev's generic shared VID/PID pair and follow the rules outlined
 * in firmware/usbdrv/USBID-License.txt.
 */

int debug = 0;
int verbose = 0;

static void usage(char *name) {
    fprintf(stderr, "usage:\n");
    fprintf(stderr, "  %s [-v] [-d] <channel> <value> [<value> ...]\n", name);
    fprintf(stderr, "     -v          	verbose, display interesting runtime info\n");
    fprintf(stderr, "     -d          	debug, display debugging info\n");
    fprintf(stderr, "  %s -bootloader   start bootloader for firmware update\n", name);
}

static int usbGetStringAscii(usb_dev_handle *dev, int index, int langid,
                             char *buf, int buflen) {
    char buffer[256];
    int rval, i;

    if ((rval = usb_control_msg(dev, USB_ENDPOINT_IN, USB_REQ_GET_DESCRIPTOR,
                                (USB_DT_STRING << 8) + index, langid, buffer,
                                sizeof(buffer), 1000)) < 0)
        return rval;
    if (buffer[1] != USB_DT_STRING)
        return 0;
    if ((unsigned char)buffer[0] < rval)
        rval = (unsigned char)buffer[0];
    rval /= 2;
    /* lossy conversion to ISO Latin1 */
    for (i = 1; i < rval; i++) {
        if (i > buflen) /* destination buffer overflow */
            break;
        buf[i - 1] = buffer[2 * i];
        if (buffer[2 * i + 1] != 0) /* outside of ISO Latin1 range */
            buf[i - 1] = '?';
    }
    buf[i - 1] = 0;
    return i - 1;
}

/*
 * uDMX uses the free shared default VID/PID.
 * To avoid talking to some other device we check the vendor and
 * device strings returned.
 */
static usb_dev_handle *findDevice(void) {
    struct usb_bus *bus;
    struct usb_device *dev;
    char string[256];
    int len;
    usb_dev_handle *handle = 0;

    usb_find_busses();
    usb_find_devices();
    for (bus = usb_busses; bus; bus = bus->next) {
        for (dev = bus->devices; dev; dev = dev->next) {
            if (dev->descriptor.idVendor == USBDEV_SHARED_VENDOR &&
                dev->descriptor.idProduct == USBDEV_SHARED_PRODUCT) {
                if (debug) { printf("Found device with %x:%x\n",
                               USBDEV_SHARED_VENDOR, USBDEV_SHARED_PRODUCT); }

                /* open the device to query strings */
                handle = usb_open(dev);
                if (!handle) {
                    fprintf(stderr, "Warning: cannot open USB device: %s\n",
                            usb_strerror());
                    continue;
                }

                /* now find out whether the device actually is a uDMX */
                len = usbGetStringAscii(handle, dev->descriptor.iManufacturer,
                                        0x0409, string, sizeof(string));
                if (len < 0) {
                    fprintf(stderr, "warning: cannot query manufacturer for device: %s\n",
                            usb_strerror());
                    goto skipDevice;
                }
                if (debug) { printf("Device vendor is %s\n",string); }
                if (strcmp(string, "www.anyma.ch") != 0)
                    goto skipDevice;

                len = usbGetStringAscii(handle, dev->descriptor.iProduct, 0x0409, string, sizeof(string));
                if (len < 0) {
                    fprintf(stderr, "warning: cannot query product for device: %s\n", usb_strerror());
                    goto skipDevice;
                }
                if (debug) { printf("Device product is %s\n",string); }
                if (strcmp(string, "uDMX") == 0)
                    break;

            skipDevice:
                usb_close(handle);
                handle = NULL;
            }
        }
        if (handle)
            break;
    }
    return handle;
}

char * read_uDMXrc (char * keyword1, char * keyword2) {
    FILE *uDMXrc;
    char uDMXrcname[1024];
    char *HOME, *p, *linebuf;
    char keyword[1024];
    size_t len = 0;
    ssize_t read;

    HOME=getenv("HOME");
    strcpy(uDMXrcname,HOME);
    strcat(uDMXrcname,"/.uDMXrc");
    uDMXrc = fopen(uDMXrcname,"r");
    if (! uDMXrc) { 
        if (debug) {
            perror("Opening $HOME/.uDMXrc failed");
        }
        return 0;
    }

    sprintf(keyword,"%s %s ",keyword1,keyword2);
    while ((read = getline(&linebuf, &len, uDMXrc)) != -1) {
        linebuf[strlen(linebuf)-1]='\0';
//        printf("Line: '%s', looking for '%s'\n",linebuf,keyword);
        if (linebuf[0]=='#') { continue; }
        if ((p=strcasestr(linebuf,keyword))!=NULL) {
//            printf(">>> Found '%s' in line '%s'\n",keyword,linebuf);
//            printf("<<< %ld %ld\n",(linebuf-p)+strlen(keyword),&p);
            return p+strlen(keyword);
        }
    }
    fclose(uDMXrc);
    return 0;
}

int main(int argc, char **argv) {
    usb_dev_handle *handle = NULL;
    char buffer[8];
    char *linebuf, *token;
    int i,nBytes;
    int arg,val=0,channel=0;
    unsigned char values[512];
    usb_set_debug(0);

    /*
     * Commandline - simple flags
     *
     * The argument '-bootloader' als only argument starts the bootloader and exits
     * The firmware download is then performed separately with uboot utility
     * The flags '-d' and '-v' provide more verbose output
     */
    if (argc == 2 && !strcmp(argv[1], "-bootloader")) {
        printf("Starting bootloader...\nPlease use the ./uboot utility to "
               "update firmware.\n");
        nBytes = usb_control_msg(
            handle, USB_TYPE_VENDOR | USB_RECIP_DEVICE | USB_ENDPOINT_IN,
            cmd_StartBootloader, 0, 0, buffer, sizeof(buffer), 5000);
        exit(0);
    }

    for (arg=1; arg<argc; arg++) {
        if (argv[arg][0]=='-') {
            if (argv[arg][1]=='v') { verbose=1; continue; }
            if (argv[arg][1]=='d') { debug=1; continue; }
            usage(argv[0]);
            exit(1);
        }
        break;
    }
    if (verbose) fprintf(stderr, "uDMX utility, version %s\n",UDMXVERSION);
    /*
     * USB Initialisation
     *
     * If we can not find the uDMX device we exit
     */
    usb_init();
    handle = findDevice();
    if (handle == NULL) {
        fprintf(stderr,
                "Could not find USB device www.anyma.ch/uDMX (vid=0x%x pid=0x%x)\n",
                USBDEV_SHARED_VENDOR, USBDEV_SHARED_PRODUCT);
        exit(1);
    }

    /* at this point we need at lead two aguments - channel and value(s) */
    if ((argc-arg) < 2) {
        usage(argv[0]);
        exit(1);
    }

//    printf("verbose=%d  debug=%d  arg=%i args=%i  next=%s\n",verbose,debug,arg,argc-arg,argv[arg]);

    /* next argument must be the channel, look it up in uDMXrc */
    linebuf=read_uDMXrc("channel",argv[arg]);
    if (linebuf) {
        channel = atoi(linebuf);
//        printf("Found channel '%s' as '%s'\n",argv[arg],linebuf);
    } else {
        channel = atoi(argv[arg]);
    }
    if ((channel<1) || (channel>512)) {
        fprintf(stderr,"Error: Channel '%i' out of range, must be between 1 and 512 !\n",channel);
        usage(argv[0]);
        exit(1);
    }
    arg++;

    /* remaining arguments must be value, look them up one by one */
    for (;arg<argc;arg++) {
//        printf("Looking up arg %d - %s\n",arg,argv[arg]);
        linebuf=read_uDMXrc("values",argv[arg]);
        if (linebuf) {
//            printf("Found values '%s' as '%s'\n",argv[arg],linebuf);
            token=strtok(linebuf," ");
            while (token != NULL) {
                values[val]=atoi(token);
//                printf("Token %s int %d\n",token,values[val]);
                if ((values[val]<0)||(values[val]>256)) {
                    fprintf(stderr,"Error: Value '%d' in '%s' out of range, must be 0-255\n",values[val],argv[arg]);
                    exit(1);
                }
                val++;
                token=strtok(NULL," ");
            }
        } else {
            if (argv[arg][0]=='0') {
                values[val++]=0;
            } else {
                values[val]=atoi(argv[arg]);
                if (values[val]==0) {
                    fprintf(stderr,"Error: Value '%s' not a number or alias\n",argv[arg]);
                    exit(1);
                }
                if ((values[val]<0)||(values[val]>256)) {
                    fprintf(stderr,"Error: Value '%s' out of range, must be 0-255\n",argv[arg]);
                    exit(1);
                }
                val++;
            }
        }
    }

    if (val == 1) {
        /* Single value = we have a single <channel> / <value> pair */
        if (verbose) printf("uDMX: Setting channel %d to %d\n", channel, values[val-1]);

        nBytes = usb_control_msg(handle, USB_TYPE_VENDOR | USB_RECIP_DEVICE | USB_ENDPOINT_OUT,
                                 cmd_SetSingleChannel, values[val-1], channel-1, NULL, 0, 1000);
        if (nBytes < 0)
            fprintf(stderr, "USB error: %s\n", usb_strerror());
    } 
    if (val > 1) {
        /* More than 1 value to set, send the array*/
        if (verbose) {
            printf("uDMX: Setting channels %d-%d to", channel, channel + val - 1);
            for (i=0; i<val; i++) printf(" %d",values[i]);
            printf("\n");
        }

        nBytes = usb_control_msg(handle, USB_TYPE_VENDOR | USB_RECIP_DEVICE | USB_ENDPOINT_OUT,
                                 cmd_SetChannelRange, val, channel - 1, (char *)values, val, 1000);
        if (nBytes < 0)
            fprintf(stderr, "USB error: %s\n", usb_strerror());
    }
    if (val < 1) {
        fprintf(stderr, "Error: No value found to set\n");
        exit(1);
    }
    usb_close(handle);
    return 0;
}

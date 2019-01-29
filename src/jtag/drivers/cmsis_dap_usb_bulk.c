/***************************************************************************
 *   Copyright (C) 2018 by Mickaël Thomas                                  *
 *   mickael9@gmail.com                                                    *
 *                                                                         *
 *   Copyright (C) 2016 by Maksym Hilliaka                                 *
 *   oter@frozen-team.com                                                  *
 *                                                                         *
 *   Copyright (C) 2016 by Phillip Pearson                                 *
 *   pp@myelin.co.nz                                                       *
 *                                                                         *
 *   Copyright (C) 2014 by Paul Fertser                                    *
 *   fercerpav@gmail.com                                                   *
 *                                                                         *
 *   Copyright (C) 2013 by mike brown                                      *
 *   mike@theshedworks.org.uk                                              *
 *                                                                         *
 *   Copyright (C) 2013 by Spencer Oliver                                  *
 *   spen@spen-soft.co.uk                                                  *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>. *
 ***************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <libusb.h>
#include <helper/log.h>

#include "cmsis_dap.h"

struct cmsis_dap_backend_data {
	libusb_context *usb_ctx;
	libusb_device_handle *dev_handle;
	unsigned int ep_out;
	unsigned int ep_in;
	int interface;
};

static int cmsis_dap_usb_interface = -1;

static int cmsis_dap_usb_open(struct cmsis_dap *dap, uint16_t vids[], uint16_t pids[], char *serial)
{
	int err;
	libusb_context *ctx;
	libusb_device **device_list;

	err = libusb_init(&ctx);
	if (err) {
		LOG_ERROR("libusb initialization failed: %s", libusb_strerror(err));
		return ERROR_FAIL;
	}

	int num_devices = libusb_get_device_list(ctx, &device_list);
	if (num_devices < 0) {
		LOG_ERROR("could not enumerate USB devices: %s", libusb_strerror(num_devices));
		libusb_exit(ctx);
		return ERROR_FAIL;
	}

	for (int i = 0; i < num_devices; i++) {
		libusb_device *dev = device_list[i];
		struct libusb_device_descriptor dev_desc;

		err = libusb_get_device_descriptor(dev, &dev_desc);
		if (err) {
			LOG_ERROR("could not get device descriptor for device %d: %s", i, libusb_strerror(err));
			continue;
		}

		/* Match VID/PID */

		bool id_match = true; /* match if we don't enter the loop (no filter) */
		for (int id = 0; vids[id] || pids[id]; id++) {
			id_match = !vids[id] || dev_desc.idVendor == vids[id];
			id_match &= !pids[id] || dev_desc.idProduct == pids[id];

			if (id_match)
				break;
		}

		if (!id_match)
			continue;

		/* Don't continue if we asked for a serial number and the device doesn't have one */
		if (dev_desc.iSerialNumber == 0 && serial && serial[0])
			continue;

		libusb_device_handle *dev_handle = NULL;
		err = libusb_open(dev, &dev_handle);
		if (err) {
			/* It's to be expected that most USB devices can't be opened
			 * so only report an error if it was explicitly selected
			 */
			if (vids[0] || pids[0]) {
				LOG_ERROR("could not open device 0x%04x:0x%04x: %s",
						dev_desc.idVendor, dev_desc.idProduct, libusb_strerror(err));
			} else {
				LOG_DEBUG("could not open device 0x%04x:0x%04x: %s",
						dev_desc.idVendor, dev_desc.idProduct, libusb_strerror(err));
			}
			continue;
		}

		/* Match serial number */

		bool serial_match = (serial == NULL);
		char dev_serial[256] = {0};
		if (dev_desc.iSerialNumber > 0) {
			err = libusb_get_string_descriptor_ascii(
					dev_handle, dev_desc.iSerialNumber,
					(uint8_t *)dev_serial, sizeof(dev_serial));

			if (err < 0) {
				LOG_ERROR("could not read serial number for device 0x%04x:0x%04x: %s",
						dev_desc.idVendor, dev_desc.idProduct, libusb_strerror(err));
			} else if (serial && strncmp(dev_serial, serial, sizeof(dev_serial)) == 0) {
				serial_match = true;
			}
		}

		if (!serial_match) {
			libusb_close(dev_handle);
			continue;
		}

		/* Find the CMSIS-DAP string in product string */

		bool cmsis_dap_found = false;
		char product_string[256] = {0};
		if (dev_desc.iProduct > 0) {
			err = libusb_get_string_descriptor_ascii(
					dev_handle, dev_desc.iProduct,
					(uint8_t *)product_string, sizeof(product_string));
			if (err < 0) {
				LOG_ERROR("could not read product string for device 0x%04x:0x%04x: %s",
						dev_desc.idVendor, dev_desc.idProduct, libusb_strerror(err));
			} else if (strstr(product_string, "CMSIS-DAP")) {
				LOG_DEBUG("CMSIS-DAP found in product string");
				cmsis_dap_found = true;
			}
		}

		/* Find the CMSIS-DAP interface */

		for (int config = 0; config < dev_desc.bNumConfigurations; config++) {
			struct libusb_config_descriptor *config_desc;
			err = libusb_get_config_descriptor(dev, config, &config_desc);
			if (err) {
				LOG_ERROR("could not get configuration descriptor %d for device 0x%04x:0x%04x: %s",
						config, dev_desc.idVendor, dev_desc.idProduct, libusb_strerror(err));
				continue;
			}

			int config_num = config_desc->bConfigurationValue;

			for (int interface = 0; interface < config_desc->bNumInterfaces; interface++) {
				const struct libusb_interface_descriptor *intf_desc = &config_desc->interface[interface].altsetting[0];
				int interface_num = intf_desc->bInterfaceNumber;

				/* Skip this interface if another one was requested explicitly */
				if (cmsis_dap_usb_interface != -1 && cmsis_dap_usb_interface != interface_num)
					continue;

				/* CMSIS-DAP v2 spec says:
				 *
				 * CMSIS-DAP with default V2 configuration uses WinUSB and is therefore faster.
				 * Optionally support for streaming SWO trace is provided via an additional USB endpoint.
				 *
				 * The WinUSB configuration requires custom class support with the interface setting
				 *     Class Code: 0xFF (Vendor specific)
				 *     Subclass: 0x00
				 *     Protocol code: 0x00
				 *
				 * Depending on the configuration it uses the following USB endpoints which should be configured
				 * in the interface descriptor in this order:
				 *  - Endpoint 1: Bulk Out – used for commands received from host PC.
				 *  - Endpoint 2: Bulk In – used for responses send to host PC.
				 *  - Endpoint 3: Bulk In (optional) – used for streaming SWO trace (if enabled with SWO_STREAM).
				 */

				if (intf_desc->bNumEndpoints < 2)
					continue;

				if ((intf_desc->endpoint[0].bmAttributes & 3) != LIBUSB_TRANSFER_TYPE_BULK ||
						(intf_desc->endpoint[0].bEndpointAddress & 0x80) != LIBUSB_ENDPOINT_OUT)
					continue;

				if ((intf_desc->endpoint[1].bmAttributes & 3) != LIBUSB_TRANSFER_TYPE_BULK ||
						(intf_desc->endpoint[1].bEndpointAddress & 0x80) != LIBUSB_ENDPOINT_IN)
					continue;

				/* Bypass the following checks if this interface was explicitly requested. */
				if (cmsis_dap_usb_interface == -1) {
					if (intf_desc->bInterfaceClass != LIBUSB_CLASS_VENDOR_SPEC ||
							intf_desc->bInterfaceSubClass != 0 || intf_desc->bInterfaceProtocol != 0)
						continue;

					/* Search for "CMSIS-DAP" in the interface string */
					if (cmsis_dap_usb_interface != -1 && !cmsis_dap_found) {
						if (intf_desc->iInterface == 0)
							continue;

						char interface_str[256] = {0};

						err = libusb_get_string_descriptor_ascii(
								dev_handle, intf_desc->iInterface,
								(uint8_t *)interface_str, sizeof(interface_str));
						if (err < 0) {
							LOG_ERROR("could not read interface string for device 0x%04x:0x%04x: %s",
									dev_desc.idVendor, dev_desc.idProduct, libusb_strerror(err));
							continue;
						} else if (!strstr(interface_str, "CMSIS-DAP")) {
							continue;
						} else {
							LOG_DEBUG("CMSIS-DAP found in interface string");
						}
					}
				}

				int packet_size = intf_desc->endpoint[0].wMaxPacketSize;
				int ep_out = intf_desc->endpoint[0].bEndpointAddress;
				int ep_in = intf_desc->endpoint[1].bEndpointAddress;

				/* That's the one! */
				libusb_free_config_descriptor(config_desc);
				libusb_free_device_list(device_list, true);

				LOG_INFO("Using CMSIS-DAPv2 interface with VID:PID=0x%04x:0x%04x, serial=%s",
						dev_desc.idVendor, dev_desc.idProduct, dev_serial);

				int current_config;
				err = libusb_get_configuration(dev_handle, &current_config);
				if (err) {
					LOG_ERROR("could not find current configuration: %s", libusb_strerror(err));
					libusb_close(dev_handle);
					libusb_exit(ctx);
					return ERROR_FAIL;
				}

				if (config_num != current_config) {
					err = libusb_set_configuration(dev_handle, config_num);
					if (err) {
						LOG_ERROR("could not set configuration: %s", libusb_strerror(err));
						libusb_close(dev_handle);
						libusb_exit(ctx);
						return ERROR_FAIL;
					}
				}

				err = libusb_claim_interface(dev_handle, interface_num);
				if (err)
					LOG_WARNING("could not claim interface: %s", libusb_strerror(err));

				dap->bdata = malloc(sizeof(struct cmsis_dap_backend_data));
				if (dap->bdata == NULL) {
					LOG_ERROR("unable to allocate memory");
					libusb_release_interface(dev_handle, interface_num);
					libusb_close(dev_handle);
					libusb_exit(ctx);
					return ERROR_FAIL;
				}

				dap->packet_size = packet_size + 1; /* "+ 1" for compatibility with the HID backend */
				dap->bdata->usb_ctx = ctx;
				dap->bdata->dev_handle = dev_handle;
				dap->bdata->ep_out = ep_out;
				dap->bdata->ep_in = ep_in;
				dap->bdata->interface = interface_num;
				return ERROR_OK;
			}

			libusb_free_config_descriptor(config_desc);
		}

		libusb_close(dev_handle);
	}

	libusb_free_device_list(device_list, true);

	libusb_exit(ctx);
	return ERROR_FAIL;
}

static void cmsis_dap_usb_close(struct cmsis_dap *dap)
{
	libusb_release_interface(dap->bdata->dev_handle, dap->bdata->interface);
	libusb_close(dap->bdata->dev_handle);
	libusb_exit(dap->bdata->usb_ctx);
	free(dap->bdata);
	dap->bdata = NULL;
}

static int cmsis_dap_usb_read(struct cmsis_dap *dap, int timeout_ms)
{
	int transferred = 0;
	int err;

	err = libusb_bulk_transfer(dap->bdata->dev_handle, dap->bdata->ep_in,
							dap->packet_buffer, dap->packet_size - 1, &transferred, timeout_ms);
	if (err) {
		if (err == LIBUSB_ERROR_TIMEOUT) {
			return ERROR_TIMEOUT_REACHED;
		} else {
			LOG_ERROR("error reading data: %s", libusb_strerror(err));
			return ERROR_FAIL;
		}
	}

	memset(&dap->packet_buffer[transferred], 0, dap->packet_size - transferred);

	return transferred;
}

static int cmsis_dap_usb_write(struct cmsis_dap *dap, int txlen, int timeout_ms)
{
	int transferred = 0;
	int err;

	/* skip the first byte that is only used by the HID backend */
	err = libusb_bulk_transfer(dap->bdata->dev_handle, dap->bdata->ep_out,
							dap->packet_buffer + 1, txlen - 1, &transferred, timeout_ms);
	if (err) {
		if (err == LIBUSB_ERROR_TIMEOUT) {
			return ERROR_TIMEOUT_REACHED;
		} else {
			LOG_ERROR("error writing data: %s", libusb_strerror(err));
			return ERROR_FAIL;
		}
	}

	return transferred;
}

COMMAND_HANDLER(cmsis_dap_handle_usb_interface_command)
{
	if (CMD_ARGC == 1)
		cmsis_dap_usb_interface = strtoul(CMD_ARGV[0], NULL, 10);
	else
		LOG_ERROR("expected exactly one argument to cmsis_dap_usb_interface <interface_number>");

	return ERROR_OK;
}

const struct command_registration cmsis_dap_usb_subcommand_handlers[] = {
	{
		.name = "interface",
		.handler = &cmsis_dap_handle_usb_interface_command,
		.mode = COMMAND_CONFIG,
		.help = "set the USB interface number to use (for USB bulk backend only)",
		.usage = "<interface_number>",
	},
	COMMAND_REGISTRATION_DONE
};

const struct cmsis_dap_backend cmsis_dap_usb_backend = {
	.name = "usb_bulk",
	.open = cmsis_dap_usb_open,
	.close = cmsis_dap_usb_close,
	.read = cmsis_dap_usb_read,
	.write = cmsis_dap_usb_write,
};

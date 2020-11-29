/*
 *  Copyright (C) 2012, Samsung Electronics Co. Ltd. All Rights Reserved.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 */
#include "ssp.h"

#if defined(CONFIG_SENSORS_SSP_CANVAS)
#define SSP_FIRMWARE_REVISION_BCM_OLD	20062201		// bcm4776 (rev:17 ~ 20)
#define SSP_FIRMWARE_REVISION_BCM		20082700		// bcm4775 (rev:~16, 21~)
#elif defined(CONFIG_SENSORS_SSP_PICASSO)
#define SSP_FIRMWARE_REVISION_BCM	20082700
#elif defined(CONFIG_SENSORS_SSP_R8)
#define SSP_FIRMWARE_REVISION_BCM	20082600
#else
#define SSP_FIRMWARE_REVISION_BCM	00000000
#endif

unsigned int get_module_rev(struct ssp_data *data)
{
#if defined(CONFIG_SENSORS_SSP_CANVAS)
	int patch_version = get_patch_version(data->ap_type, data->ap_rev);
	if(patch_version == bbd_old)
		return SSP_FIRMWARE_REVISION_BCM_OLD;
#endif
	return SSP_FIRMWARE_REVISION_BCM;
}

/*
 *
 *  oFono - Open Source Telephony - RIL-based devices
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2012 Canonical Ltd.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <gril.h>
#include <parcel.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>
#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/devinfo.h>
#include <ofono/phonebook.h>
#include <ofono/netreg.h>
#include <ofono/voicecall.h>
#include <ofono/sms.h>
#include <ofono/cbs.h>
#include <ofono/sim.h>
#include <ofono/ussd.h>
#include <ofono/call-forwarding.h>
#include <ofono/call-settings.h>
#include <ofono/call-barring.h>
#include <ofono/call-meter.h>
#include <ofono/call-volume.h>
#include <ofono/radio-settings.h>
#include <ofono/gprs.h>
#include <ofono/gprs-context.h>
#include <ofono/audio-settings.h>
#include <ofono/types.h>
#include <ofono.h>

#include "drivers/rilmodem/rilmodem.h"

#define MAX_POWER_ON_RETRIES 5

struct ril_data {
	const char *ifname;
	GRil *modem;
	int power_on_retries;

	ofono_bool_t have_sim;
	ofono_bool_t online;
	ofono_bool_t reported;
};

static char print_buf[PRINT_BUF_SIZE];

static gboolean power_on(gpointer user_data);

static void ril_debug(const char *str, void *user_data)
{
	const char *prefix = user_data;

	ofono_info("%s%s", prefix, str);
}

static void power_cb(struct ril_msg *message, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct ril_data *ril = ofono_modem_get_data(modem);

	if (message->error != RIL_E_SUCCESS) {
		ril->power_on_retries++;
		ofono_warn("Radio Power On request failed: %d; retries: %d",
				message->error, ril->power_on_retries);

		if (ril->power_on_retries < MAX_POWER_ON_RETRIES)
			g_timeout_add_seconds(1, power_on, modem);
		else
			ofono_error("Max retries for radio power on exceeded!");
	} else {
		DBG("Radio POWER-ON OK, calling set_powered(TRUE).");
		ofono_modem_set_powered(modem, TRUE);
	}
}

static gboolean power_on(gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct parcel rilp;
	struct ril_data *ril = ofono_modem_get_data(modem);

	DBG("");

	parcel_init(&rilp);
	parcel_w_int32(&rilp, 1); /* size of array */
	parcel_w_int32(&rilp, 1); /* POWER=ON */

	g_ril_send(ril->modem, RIL_REQUEST_RADIO_POWER,
			rilp.data, rilp.size, power_cb, modem, NULL);

	parcel_free(&rilp);

	/* Makes this a single shot */
	return FALSE;
}

static void sim_status_cb(struct ril_msg *message, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct ril_data *ril = ofono_modem_get_data(modem);

	DBG("");

	/* Returns TRUE if cardstate == PRESENT */
	/* TODO: Third argument of type struct sim_data* is passed as NULL since
	 * at this point struct ofono_sim contained in modem doesn't have sim_data
	 * set.
	 * sim_data is created and added to ofono_sim a bit later on in
	 * drivers/rilmodem/sim.c:ril_sim_probe() */
	if (ril_util_parse_sim_status(message, NULL, NULL)) {
		DBG("have_sim = TRUE; powering on modem.");

		/* TODO: check PinState=DISABLED, for now just
		 * set state to valid... */
		ril->have_sim = TRUE;
		power_on(modem);
	}

	/* TODO: handle emergency calls if SIM !present or locked */
}

static int send_get_sim_status(struct ofono_modem *modem)
{
	struct ril_data *ril = ofono_modem_get_data(modem);
	int ret;

	ret = g_ril_send(ril->modem, RIL_REQUEST_GET_SIM_STATUS,
				NULL, 0, sim_status_cb, modem, NULL);

	/* TODO: make conditional */
	ril_clear_print_buf;
	ril_print_request(ret, RIL_REQUEST_GET_SIM_STATUS);
	/* TODO: make conditional */

	return ret;
}

static int ril_probe(struct ofono_modem *modem)
{
	char const *ifname = ofono_modem_get_string(modem, "Interface");
	unsigned address = ofono_modem_get_integer(modem, "Address");
	struct ril_data *ril = NULL;

	if (!ifname) {
		DBG("(%p) no ifname", modem);
		return -EINVAL;
	}

	DBG("(%p) with %s / %d", modem, ifname, address);

	ril = g_try_new0(struct ril_data, 1);
	if (ril == NULL) {
		errno = ENOMEM;
		goto error;
	}

        ril->modem = NULL;
	ril->ifname = ifname;

	ofono_modem_set_data(modem, ril);

	return 0;

error:
	g_free(ril);

	return -errno;
}

static void ril_remove(struct ofono_modem *modem)
{
	struct ril_data *ril = ofono_modem_get_data(modem);

	DBG("(%p) with %s", modem, ril->ifname);

	ofono_modem_set_data(modem, NULL);

	if (!ril)
		return;

	g_ril_unref(ril->modem);

	g_free(ril);
}

static void ril_pre_sim(struct ofono_modem *modem)
{
	struct ril_data *ril = ofono_modem_get_data(modem);
	struct ofono_sim *sim;

	DBG("(%p) with %s", modem, ril->ifname);

	sim = ofono_sim_create(modem, 0, "rilmodem", ril->modem);
	ofono_devinfo_create(modem, 0, "rilmodem", ril->modem);
	ofono_voicecall_create(modem, 0, "rilmodem", ril->modem);

	if (sim && ril->have_sim)
		ofono_sim_inserted_notify(sim, TRUE);
}

static void ril_post_sim(struct ofono_modem *modem)
{
	struct ril_data *ril = ofono_modem_get_data(modem);
	struct ofono_gprs *gprs;
	struct ofono_gprs_context *gc;



	DBG("(%p) with %s", modem, ril->ifname);

	/* TODO: this function should setup:
	 *  - phonebook
	 *  - stk ( SIM toolkit )
	 */
	ofono_sms_create(modem, 0, "rilmodem", ril->modem);

	gprs = ofono_gprs_create(modem, 0, "rilmodem", ril->modem);
 	gc = ofono_gprs_context_create(modem, 0, "rilmodem", ril->modem);

	if (gprs && gc) {
		DBG("calling gprs_add_context");
		ofono_gprs_add_context(gprs, gc);
	}

	ofono_radio_settings_create(modem, 0, "rilmodem", ril->modem);
	ofono_phonebook_create(modem, 0, "rilmodem", ril->modem);
}

static void ril_post_online(struct ofono_modem *modem)
{
	struct ril_data *ril = ofono_modem_get_data(modem);

	DBG("(%p) with %s", modem, ril->ifname);

	ofono_call_volume_create(modem, 0, "rilmodem", ril->modem);
	ofono_netreg_create(modem, 0, "rilmodem", ril->modem);
	ofono_ussd_create(modem, 0, "rilmodem", ril->modem);
	ofono_call_settings_create(modem, 0, "rilmodem", ril->modem);
}

static int ril_enable(struct ofono_modem *modem)
{
	struct ril_data *ril = ofono_modem_get_data(modem);

	DBG("modem=%p with %s", modem, ril ? ril->ifname : NULL);

	ril->have_sim = FALSE;

        ril->modem = g_ril_new();

        /* NOTE: Since AT modems open a tty, and then call 
	 * g_at_chat_new(), they're able to return -EIO if
	 * the first fails, and -ENOMEM if the second fails.
	 * in our case, we already return -EIO if the ril_new
	 * fails.  If this is important, we can create a ril_socket
	 * abstraction... ( probaby not a bad idea ). 
	 */

        if (ril->modem == NULL) {
		DBG("g_ril_new() failed to create modem!");
		return -EIO;
	}

	if (getenv("OFONO_RIL_DEBUG")) {
		DBG("calling g_ril_set_debug");
		g_ril_set_debug(ril->modem, ril_debug, "Device: ");
	}

	send_get_sim_status(modem);

        return -EINPROGRESS;
}

static int ril_disable(struct ofono_modem *modem)
{
	struct ril_data *ril = ofono_modem_get_data(modem);

	DBG("modem=%p with %p", modem, ril ? ril->ifname : NULL);

        return 0;
}

static struct ofono_modem_driver ril_driver = {
	.name = "ril",
	.probe = ril_probe,
	.remove = ril_remove,
	.enable = ril_enable,
	.disable = ril_disable,
	.pre_sim = ril_pre_sim,
	.post_sim = ril_post_sim,
	.post_online = ril_post_online,
};

/*
 * Note - as an aal+ container doesn't include a running udev,
 * the udevng plugin will never detect a modem, and thus modem
 * creation for a RIL-based modem needs to be hard-coded.
 *
 * Typically, udevng would create the modem, which in turn would
 * lead to this plugin's probe function being called.
 *
 * This is a first attempt at registering like this.
 *
 * IMPORTANT - this code relies on the fact that the 'rilmodem' is
 * added to top-level Makefile's builtin_modules *after* 'ril'.
 * This has means 'rilmodem' will already be registered before we try
 * to create and register the modem.  In standard ofono, 'udev'/'udevng'
 * is initialized last due to the fact that it's the first module
 * added in the top-level Makefile.
 */
static int ril_init(void)
{
	int retval = 0;
	struct ofono_modem *modem;

	DBG("ofono_modem_register returned: %d", retval);
        
	if ((retval = ofono_modem_driver_register(&ril_driver))) {
		DBG("ofono_modem_driver_register returned: %d", retval);
                return retval;
        }

        /* everything after _modem_driver_register, is
	 * non-standard ( see udev comment above ).
	 * usually called by undevng::create_modem
	 *
	 * args are name (optional) & type
	 */
	modem = ofono_modem_create(NULL, "ril");
	if (modem == NULL) {
		DBG("ofono_modem_create failed for ril");
		return -ENODEV;
	}

	/* TODO: these are both placeholders; we should
	 * determine if they can be removed.
	 */
	ofono_modem_set_string(modem, "Interface", "ttys");
	ofono_modem_set_integer(modem, "Address", 0);

	/* This causes driver->probe() to be called... */
	retval = ofono_modem_register(modem);
	DBG("ofono_modem_register returned: %d", retval);

        /* kickstart the modem:
	 * causes core modem code to call
	 * - set_powered(TRUE) - which in turn
	 *   calls driver->enable()
	 *
	 * - driver->pre_sim()
	 *
	 * Could also be done via:
	 *
	 * - a DBus call to SetProperties w/"Powered=TRUE" *1
	 * - sim_state_watch ( handles SIM removal? LOCKED states? **2
	 * - ofono_modem_set_powered()
	 */
        ofono_modem_reset(modem);

	return retval;
}

static void ril_exit(void)
{
	DBG("");
	ofono_modem_driver_unregister(&ril_driver);
}

OFONO_PLUGIN_DEFINE(ril, "RIL modem driver", VERSION,
			OFONO_PLUGIN_PRIORITY_DEFAULT, ril_init, ril_exit)
 

// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (c) 1996, 2003 VIA Networking Technologies, Inc.
 * All rights reserved.
 *
 * File: card.c
 * Purpose: Provide functions to setup NIC operation mode
 * Functions:
 *      vnt_set_rspinf - Set RSPINF
 *      vnt_update_ifs - Update slotTime,SIFS,DIFS, and EIFS
 *      vnt_update_top_rates - Update BasicTopRate
 *      vnt_add_basic_rate - Add to BasicRateSet
 *      vnt_ofdm_min_rate - Check if any OFDM rate is in BasicRateSet
 *      vnt_get_tsf_offset - Calculate TSFOffset
 *      vnt_get_current_tsf - Read Current NIC TSF counter
 *      vnt_get_next_tbtt - Calculate Next Beacon TSF counter
 *      vnt_reset_next_tbtt - Set NIC Beacon time
 *      vnt_update_next_tbtt - Sync. NIC Beacon time
 *      vnt_radio_power_off - Turn Off NIC Radio Power
 *      vnt_radio_power_on - Turn On NIC Radio Power
 *
 * Revision History:
 *      06-10-2003 Bryan YC Fan:  Re-write codes to support VT3253 spec.
 *      08-26-2003 Kyle Hsu:      Modify the definition type of dwIoBase.
 *      09-01-2003 Bryan YC Fan:  Add vnt_update_ifs().
 *
 */

#include <linux/bits.h>
#include "device.h"
#include "card.h"
#include "baseband.h"
#include "mac.h"
#include "desc.h"
#include "rf.h"
#include "power.h"
#include "key.h"
#include "usbpipe.h"

/* const u16 cw_rxbcntsf_off[MAX_RATE] =
 *   {17, 34, 96, 192, 34, 23, 17, 11, 8, 5, 4, 3};
 */

static const u16 cw_rxbcntsf_off[MAX_RATE] = {
	192, 96, 34, 17, 34, 23, 17, 11, 8, 5, 4, 3
};

/*
 * Description: Set NIC media channel
 *
 * Parameters:
 *  In:
 *      pDevice             - The adapter to be set
 *      connection_channel  - Channel to be set
 *  Out:
 *      none
 */
void vnt_set_channel(struct vnt_private *priv, u32 connection_channel)
{
	if (connection_channel > CB_MAX_CHANNEL || !connection_channel)
		return;

	/* clear NAV */
	vnt_mac_reg_bits_on(priv, MAC_REG_MACCR, MACCR_CLRNAV);

	/* Set Channel[7] = 0 to tell H/W channel is changing now. */
	vnt_mac_reg_bits_off(priv, MAC_REG_CHANNEL,
			     (BIT(7) | BIT(5) | BIT(4)));

	vnt_control_out(priv, MESSAGE_TYPE_SELECT_CHANNEL,
			connection_channel, 0, 0, NULL);

	vnt_control_out_u8(priv, MESSAGE_REQUEST_MACREG, MAC_REG_CHANNEL,
			   (u8)(connection_channel | 0x80));
}

static const u8 vnt_rspinf_b_short_table[] = {
	0x70, 0x00, 0x00, 0x00, 0x38, 0x00, 0x09, 0x00,
	0x15, 0x00, 0x0a, 0x00, 0x0b, 0x00, 0x0b, 0x80
};

static const u8 vnt_rspinf_b_long_table[] = {
	0x70, 0x00, 0x00, 0x00, 0x38, 0x00, 0x01, 0x00,
	0x15, 0x00, 0x02, 0x00, 0x0b, 0x00, 0x03, 0x80
};

static const u8 vnt_rspinf_a_table[] = {
	0x9b, 0x18, 0x9f, 0x10, 0x9a, 0x0a, 0x9e, 0x08, 0x99,
	0x08, 0x9d, 0x04, 0x98, 0x04, 0x9c, 0x04, 0x9c, 0x04
};

static const u8 vnt_rspinf_gb_table[] = {
	0x8b, 0x1e, 0x8f, 0x16, 0x8a, 0x12, 0x8e, 0x0e, 0x89,
	0x0e, 0x8d, 0x0a, 0x88, 0x0a, 0x8c, 0x0a, 0x8c, 0x0a
};

/*
 * Description: Set RSPINF
 *
 * Parameters:
 *  In:
 *      pDevice             - The adapter to be set
 *  Out:
 *      none
 *
 * Return Value: None.
 *
 */

void vnt_set_rspinf(struct vnt_private *priv, u8 bb_type)
{
	const u8 *data;
	u16 len;

	if (priv->preamble_type) {
		data = vnt_rspinf_b_short_table;
		len = ARRAY_SIZE(vnt_rspinf_b_short_table);
	} else {
		data = vnt_rspinf_b_long_table;
		len = ARRAY_SIZE(vnt_rspinf_b_long_table);
	}

	 /* RSPINF_b_1 to RSPINF_b_11 */
	vnt_control_out(priv, MESSAGE_TYPE_WRITE, MAC_REG_RSPINF_B_1,
			MESSAGE_REQUEST_MACREG, len, data);

	if (bb_type == BB_TYPE_11A) {
		data = vnt_rspinf_a_table;
		len = ARRAY_SIZE(vnt_rspinf_a_table);
	} else {
		data = vnt_rspinf_gb_table;
		len = ARRAY_SIZE(vnt_rspinf_gb_table);
	}

	/* RSPINF_a_6 to RSPINF_a_72 */
	vnt_control_out(priv, MESSAGE_TYPE_WRITE, MAC_REG_RSPINF_A_6,
			MESSAGE_REQUEST_MACREG, len, data);
}

/*
 * Description: Update IFS
 *
 * Parameters:
 *  In:
 *	priv - The adapter to be set
 * Out:
 *	none
 *
 * Return Value: None.
 *
 */
void vnt_update_ifs(struct vnt_private *priv)
{
	u8 max_min = 0;
	u8 data[4];

	if (priv->packet_type == PK_TYPE_11A) {
		priv->slot = C_SLOT_SHORT;
		priv->sifs = C_SIFS_A;
		priv->difs = C_SIFS_A + 2 * C_SLOT_SHORT;
		max_min = 4;
	} else {
		priv->sifs = C_SIFS_BG;

		if (priv->short_slot_time) {
			priv->slot = C_SLOT_SHORT;
			max_min = 4;
		} else {
			priv->slot = C_SLOT_LONG;
			max_min = 5;
		}

		priv->difs = C_SIFS_BG + 2 * priv->slot;
	}

	priv->eifs = C_EIFS;

	switch (priv->rf_type) {
	case RF_VT3226D0:
		if (priv->bb_type != BB_TYPE_11B) {
			priv->sifs -= 1;
			priv->difs -= 1;
			break;
		}
		/* fall through */
	case RF_AIROHA7230:
	case RF_AL2230:
	case RF_AL2230S:
		if (priv->bb_type != BB_TYPE_11B)
			break;
		/* fall through */
	case RF_RFMD2959:
	case RF_VT3226:
	case RF_VT3342A0:
		priv->sifs -= 3;
		priv->difs -= 3;
		break;
	case RF_MAXIM2829:
		if (priv->bb_type == BB_TYPE_11A) {
			priv->sifs -= 5;
			priv->difs -= 5;
		} else {
			priv->sifs -= 2;
			priv->difs -= 2;
		}

		break;
	}

	data[0] = (u8)priv->sifs;
	data[1] = (u8)priv->difs;
	data[2] = (u8)priv->eifs;
	data[3] = (u8)priv->slot;

	vnt_control_out(priv, MESSAGE_TYPE_WRITE, MAC_REG_SIFS,
			MESSAGE_REQUEST_MACREG, 4, &data[0]);

	max_min |= 0xa0;

	vnt_control_out(priv, MESSAGE_TYPE_WRITE, MAC_REG_CWMAXMIN0,
			MESSAGE_REQUEST_MACREG, 1, &max_min);
}

void vnt_update_top_rates(struct vnt_private *priv)
{
	u8 top_ofdm = RATE_24M, top_cck = RATE_1M;
	u8 i;

	/*Determines the highest basic rate.*/
	for (i = RATE_54M; i >= RATE_6M; i--) {
		if (priv->basic_rates & BIT(i)) {
			top_ofdm = i;
			break;
		}
	}

	priv->top_ofdm_basic_rate = top_ofdm;

	for (i = RATE_11M;; i--) {
		if (priv->basic_rates & BIT(i)) {
			top_cck = i;
			break;
		}
		if (i == RATE_1M)
			break;
	}

	priv->top_cck_basic_rate = top_cck;
}

int vnt_ofdm_min_rate(struct vnt_private *priv)
{
	int ii;

	for (ii = RATE_54M; ii >= RATE_6M; ii--) {
		if ((priv->basic_rates) & ((u16)BIT(ii)))
			return true;
	}

	return false;
}

u8 vnt_get_pkt_type(struct vnt_private *priv)
{
	if (priv->bb_type == BB_TYPE_11A || priv->bb_type == BB_TYPE_11B)
		return (u8)priv->bb_type;
	else if (vnt_ofdm_min_rate(priv))
		return PK_TYPE_11GA;
	return PK_TYPE_11GB;
}

/*
 * Description: Calculate TSF offset of two TSF input
 *              Get TSF Offset from RxBCN's TSF and local TSF
 *
 * Parameters:
 *  In:
 *      rx_rate	- rx rate.
 *      tsf1	- Rx BCN's TSF
 *      tsf2	- Local TSF
 *  Out:
 *      none
 *
 * Return Value: TSF Offset value
 *
 */
u64 vnt_get_tsf_offset(u8 rx_rate, u64 tsf1, u64 tsf2)
{
	return tsf1 - tsf2 - (u64)cw_rxbcntsf_off[rx_rate % MAX_RATE];
}

/*
 * Description: Sync. TSF counter to BSS
 *              Get TSF offset and write to HW
 *
 * Parameters:
 *  In:
 *      priv		- The adapter to be sync.
 *      time_stamp	- Rx BCN's TSF
 *      local_tsf	- Local TSF
 *  Out:
 *      none
 *
 * Return Value: none
 *
 */
void vnt_adjust_tsf(struct vnt_private *priv, u8 rx_rate,
		    u64 time_stamp, u64 local_tsf)
{
	u64 tsf_offset = 0;
	u8 data[8];

	tsf_offset = vnt_get_tsf_offset(rx_rate, time_stamp, local_tsf);

	data[0] = (u8)tsf_offset;
	data[1] = (u8)(tsf_offset >> 8);
	data[2] = (u8)(tsf_offset >> 16);
	data[3] = (u8)(tsf_offset >> 24);
	data[4] = (u8)(tsf_offset >> 32);
	data[5] = (u8)(tsf_offset >> 40);
	data[6] = (u8)(tsf_offset >> 48);
	data[7] = (u8)(tsf_offset >> 56);

	vnt_control_out(priv, MESSAGE_TYPE_SET_TSFTBTT,
			MESSAGE_REQUEST_TSF, 0, 8, data);
}

/*
 * Description: Read NIC TSF counter
 *              Get local TSF counter
 *
 * Parameters:
 *  In:
 *	priv		- The adapter to be read
 *  Out:
 *	current_tsf	- Current TSF counter
 *
 * Return Value: true if success; otherwise false
 *
 */
bool vnt_get_current_tsf(struct vnt_private *priv, u64 *current_tsf)
{
	*current_tsf = priv->current_tsf;

	return true;
}

/*
 * Description: Clear NIC TSF counter
 *              Clear local TSF counter
 *
 * Parameters:
 *  In:
 *      priv	- The adapter to be read
 *
 * Return Value: true if success; otherwise false
 *
 */
bool vnt_clear_current_tsf(struct vnt_private *priv)
{
	vnt_mac_reg_bits_on(priv, MAC_REG_TFTCTL, TFTCTL_TSFCNTRST);

	priv->current_tsf = 0;

	return true;
}

/*
 * Description: Read NIC TSF counter
 *              Get NEXTTBTT from adjusted TSF and Beacon Interval
 *
 * Parameters:
 *  In:
 *      tsf		- Current TSF counter
 *      beacon_interval - Beacon Interval
 *  Out:
 *      tsf		- Current TSF counter
 *
 * Return Value: TSF value of next Beacon
 *
 */
u64 vnt_get_next_tbtt(u64 tsf, u16 beacon_interval)
{
	u32 beacon_int;

	beacon_int = beacon_interval * 1024;

	/* Next TBTT =
	 *	((local_current_TSF / beacon_interval) + 1) * beacon_interval
	 */
	if (beacon_int) {
		do_div(tsf, beacon_int);
		tsf += 1;
		tsf *= beacon_int;
	}

	return tsf;
}

/*
 * Description: Set NIC TSF counter for first Beacon time
 *              Get NEXTTBTT from adjusted TSF and Beacon Interval
 *
 * Parameters:
 *  In:
 *      dwIoBase        - IO Base
 *	beacon_interval - Beacon Interval
 *  Out:
 *      none
 *
 * Return Value: none
 *
 */
void vnt_reset_next_tbtt(struct vnt_private *priv, u16 beacon_interval)
{
	u64 next_tbtt = 0;
	u8 data[8];

	vnt_clear_current_tsf(priv);

	next_tbtt = vnt_get_next_tbtt(next_tbtt, beacon_interval);

	data[0] = (u8)next_tbtt;
	data[1] = (u8)(next_tbtt >> 8);
	data[2] = (u8)(next_tbtt >> 16);
	data[3] = (u8)(next_tbtt >> 24);
	data[4] = (u8)(next_tbtt >> 32);
	data[5] = (u8)(next_tbtt >> 40);
	data[6] = (u8)(next_tbtt >> 48);
	data[7] = (u8)(next_tbtt >> 56);

	vnt_control_out(priv, MESSAGE_TYPE_SET_TSFTBTT,
			MESSAGE_REQUEST_TBTT, 0, 8, data);
}

/*
 * Description: Sync NIC TSF counter for Beacon time
 *              Get NEXTTBTT and write to HW
 *
 * Parameters:
 *  In:
 *	priv		- The adapter to be set
 *      tsf		- Current TSF counter
 *      beacon_interval - Beacon Interval
 *  Out:
 *      none
 *
 * Return Value: none
 *
 */
void vnt_update_next_tbtt(struct vnt_private *priv, u64 tsf,
			  u16 beacon_interval)
{
	u8 data[8];

	tsf = vnt_get_next_tbtt(tsf, beacon_interval);

	data[0] = (u8)tsf;
	data[1] = (u8)(tsf >> 8);
	data[2] = (u8)(tsf >> 16);
	data[3] = (u8)(tsf >> 24);
	data[4] = (u8)(tsf >> 32);
	data[5] = (u8)(tsf >> 40);
	data[6] = (u8)(tsf >> 48);
	data[7] = (u8)(tsf >> 56);

	vnt_control_out(priv, MESSAGE_TYPE_SET_TSFTBTT,
			MESSAGE_REQUEST_TBTT, 0, 8, data);

	dev_dbg(&priv->usb->dev, "%s TBTT: %8llx\n", __func__, tsf);
}

/*
 * Description: Turn off Radio power
 *
 * Parameters:
 *  In:
 *      priv         - The adapter to be turned off
 *  Out:
 *      none
 *
 * Return Value: true if success; otherwise false
 *
 */
int vnt_radio_power_off(struct vnt_private *priv)
{
	int ret = 0;

	switch (priv->rf_type) {
	case RF_AL2230:
	case RF_AL2230S:
	case RF_AIROHA7230:
	case RF_VT3226:
	case RF_VT3226D0:
	case RF_VT3342A0:
		ret = vnt_mac_reg_bits_off(priv, MAC_REG_SOFTPWRCTL,
					(SOFTPWRCTL_SWPE2 | SOFTPWRCTL_SWPE3));
		break;
	}

	if (ret)
		goto end;

	ret = vnt_mac_reg_bits_off(priv, MAC_REG_HOSTCR, HOSTCR_RXON);
	if (ret)
		goto end;

	ret = vnt_set_deep_sleep(priv);
	if (ret)
		goto end;

	ret = vnt_mac_reg_bits_on(priv, MAC_REG_GPIOCTL1, GPIO3_INTMD);

end:
	return ret;
}

/*
 * Description: Turn on Radio power
 *
 * Parameters:
 *  In:
 *      priv         - The adapter to be turned on
 *  Out:
 *      none
 *
 * Return Value: true if success; otherwise false
 *
 */
int vnt_radio_power_on(struct vnt_private *priv)
{
	int ret = 0;

	ret = vnt_exit_deep_sleep(priv);
	if (ret)
		return ret;

	ret = vnt_mac_reg_bits_on(priv, MAC_REG_HOSTCR, HOSTCR_RXON);
	if (ret)
		return ret;

	switch (priv->rf_type) {
	case RF_AL2230:
	case RF_AL2230S:
	case RF_AIROHA7230:
	case RF_VT3226:
	case RF_VT3226D0:
	case RF_VT3342A0:
		ret = vnt_mac_reg_bits_on(priv, MAC_REG_SOFTPWRCTL,
					  (SOFTPWRCTL_SWPE2 |
					   SOFTPWRCTL_SWPE3));
		if (ret)
			return ret;
	}

	return vnt_mac_reg_bits_off(priv, MAC_REG_GPIOCTL1, GPIO3_INTMD);
}

void vnt_set_bss_mode(struct vnt_private *priv)
{
	if (priv->rf_type == RF_AIROHA7230 && priv->bb_type == BB_TYPE_11A)
		vnt_mac_set_bb_type(priv, BB_TYPE_11G);
	else
		vnt_mac_set_bb_type(priv, priv->bb_type);

	priv->packet_type = vnt_get_pkt_type(priv);

	if (priv->bb_type == BB_TYPE_11A)
		vnt_control_out_u8(priv, MESSAGE_REQUEST_BBREG, 0x88, 0x03);
	else if (priv->bb_type == BB_TYPE_11B)
		vnt_control_out_u8(priv, MESSAGE_REQUEST_BBREG, 0x88, 0x02);
	else if (priv->bb_type == BB_TYPE_11G)
		vnt_control_out_u8(priv, MESSAGE_REQUEST_BBREG, 0x88, 0x08);

	vnt_update_ifs(priv);
	vnt_set_rspinf(priv, (u8)priv->bb_type);

	if (priv->bb_type == BB_TYPE_11A) {
		if (priv->rf_type == RF_AIROHA7230) {
			priv->bb_vga[0] = 0x20;

			vnt_control_out_u8(priv, MESSAGE_REQUEST_BBREG,
					   0xe7, priv->bb_vga[0]);
		}

		priv->bb_vga[2] = 0x10;
		priv->bb_vga[3] = 0x10;
	} else {
		if (priv->rf_type == RF_AIROHA7230) {
			priv->bb_vga[0] = 0x1c;

			vnt_control_out_u8(priv, MESSAGE_REQUEST_BBREG,
					   0xe7, priv->bb_vga[0]);
		}

		priv->bb_vga[2] = 0x0;
		priv->bb_vga[3] = 0x0;
	}

	vnt_set_vga_gain_offset(priv, priv->bb_vga[0]);
}

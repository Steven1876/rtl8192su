/******************************************************************************
 *
 * Copyright(c) 2009-2012  Realtek Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110, USA
 *
 * The full GNU General Public License is included in this distribution in the
 * file called LICENSE.
 *
 * Contact Information:
 * wlanfae <wlanfae@realtek.com>
 * Realtek Corporation, No. 2, Innovation Road II, Hsinchu Science Park,
 * Hsinchu 300, Taiwan.
 *
 * Larry Finger <Larry.Finger@lwfinger.net>
 *
 *****************************************************************************/

#include "../wifi.h"
#include "../usb.h"
#include "../base.h"
#include "../stats.h"
#include "../rtl8192s/reg_common.h"
#include "../rtl8192s/def_common.h"
#include "../rtl8192s/phy_common.h"
#include "../rtl8192s/fw_common.h"
#include "trx.h"
#include "led.h"

static u8 _rtl92s_map_hwqueue_to_fwqueue(struct sk_buff *skb, u8 skb_queue)
{
	__le16 fc = rtl_get_fc(skb);

	if (unlikely(ieee80211_is_beacon(fc)))
		return QSLT_BEACON;
	if (ieee80211_is_mgmt(fc) || ieee80211_is_ctl(fc))
		return QSLT_MGNT;
	if (ieee80211_is_nullfunc(fc))
		return QSLT_HIGH;

	/* Kernel commit 1bf4bbb4024dcdab changed EAPOL packets to use
	 * queue V0 at priority 7; however, the RTL8192SE appears to have
	 * that queue at priority 6
	 */
        if (skb->priority == 7)
                return QSLT_MGNT;

	return skb->priority;
}

/* endpoint mapping */
int rtl92su_endpoint_mapping(struct ieee80211_hw *hw)
{
	struct rtl_usb_priv *usb_priv = rtl_usbpriv(hw);
	struct rtl_usb *rtlusb = rtl_usbdev(usb_priv);
	struct rtl_ep_map *ep_map = &(rtlusb->ep_map);

	ep_map->ep_mapping[RTL_TXQ_BE]  = 0x06;
	ep_map->ep_mapping[RTL_TXQ_VO]	= 0x04;

	switch (rtlusb->out_ep_nums) {
	case 3:
		ep_map->ep_mapping[RTL_TXQ_BK]	= 0x06;
		ep_map->ep_mapping[RTL_TXQ_MGT] = 0x0d;
		ep_map->ep_mapping[RTL_TXQ_VI]	= 0x04;
		ep_map->ep_mapping[RTL_TXQ_BCN] = 0x0d;
		ep_map->ep_mapping[RTL_TXQ_HI]	= 0x0d;
		break;
	case 5:
		ep_map->ep_mapping[RTL_TXQ_BK]  = 0x07;
		ep_map->ep_mapping[RTL_TXQ_VI]  = 0x05;
		ep_map->ep_mapping[RTL_TXQ_MGT] = 0x0d;
		ep_map->ep_mapping[RTL_TXQ_BCN] = 0x0d;
		ep_map->ep_mapping[RTL_TXQ_HI]  = 0x0d;
		break;
	case 8:
		ep_map->ep_mapping[RTL_TXQ_BK]  = 0x07;
		ep_map->ep_mapping[RTL_TXQ_VI]  = 0x05;
		ep_map->ep_mapping[RTL_TXQ_MGT] = 0x0c;
		ep_map->ep_mapping[RTL_TXQ_BCN] = 0x0a;
		ep_map->ep_mapping[RTL_TXQ_HI]  = 0x0b;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

u16 rtl92su_mq_to_hwq(__le16 fc, u16 mac80211_queue_index)
{
	u16 hw_queue_index;

	if (unlikely(ieee80211_is_beacon(fc))) {
		hw_queue_index = RTL_TXQ_BCN;
		goto out;
	}
	if (ieee80211_is_mgmt(fc)) {
		hw_queue_index = RTL_TXQ_MGT;
		goto out;
	}
	switch (mac80211_queue_index) {
	case 0:
		hw_queue_index = RTL_TXQ_VO;
		break;
	case 1:
		hw_queue_index = RTL_TXQ_VI;
		break;
	case 2:
		hw_queue_index = RTL_TXQ_BE;
		break;
	case 3:
		hw_queue_index = RTL_TXQ_BK;
		break;
	default:
		hw_queue_index = RTL_TXQ_BE;
		RT_ASSERT(false, "QSLT_BE queue, skb_queue:%d\n",
			  mac80211_queue_index);
		break;
	}
out:
	return hw_queue_index;
}

static void _rtl92se_query_rxphystatus(struct ieee80211_hw *hw,
				       struct rtl_stats *pstats, u8 *pdesc,
				       struct rx_fwinfo *p_drvinfo,
				       bool packet_match_bssid,
				       bool packet_toself,
				       bool packet_beacon)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct phy_sts_cck_8192s_t *cck_buf;
	s8 rx_pwr_all = 0, rx_pwr[4];
	u8 rf_rx_num = 0, evm, pwdb_all;
	u8 i, max_spatial_stream;
	u32 rssi, total_rssi = 0;
	bool is_cck = pstats->is_cck;

	pstats->packet_matchbssid = packet_match_bssid;
	pstats->packet_toself = packet_toself;
	pstats->packet_beacon = packet_beacon;
	pstats->rx_mimo_sig_qual[0] = -1;
	pstats->rx_mimo_sig_qual[1] = -1;

	if (is_cck) {
		u8 report, cck_highpwr;
		cck_buf = (struct phy_sts_cck_8192s_t *)p_drvinfo;

		if (!cck_highpwr) {
			u8 cck_agc_rpt = cck_buf->cck_agc_rpt;
			report = cck_buf->cck_agc_rpt & 0xc0;
			report = report >> 6;
			switch (report) {
			case 0x3:
				rx_pwr_all = -40 - (cck_agc_rpt & 0x3e);
				break;
			case 0x2:
				rx_pwr_all = -20 - (cck_agc_rpt & 0x3e);
				break;
			case 0x1:
				rx_pwr_all = -2 - (cck_agc_rpt & 0x3e);
				break;
			case 0x0:
				rx_pwr_all = 14 - (cck_agc_rpt & 0x3e);
				break;
			}
		} else {
			u8 cck_agc_rpt = cck_buf->cck_agc_rpt;
			report = p_drvinfo->cfosho[0] & 0x60;
			report = report >> 5;
			switch (report) {
			case 0x3:
				rx_pwr_all = -40 - ((cck_agc_rpt & 0x1f) << 1);
				break;
			case 0x2:
				rx_pwr_all = -20 - ((cck_agc_rpt & 0x1f) << 1);
				break;
			case 0x1:
				rx_pwr_all = -2 - ((cck_agc_rpt & 0x1f) << 1);
				break;
			case 0x0:
				rx_pwr_all = 14 - ((cck_agc_rpt & 0x1f) << 1);
				break;
			}
		}

		pwdb_all = rtl_query_rxpwrpercentage(rx_pwr_all);

		/* CCK gain is smaller than OFDM/MCS gain,  */
		/* so we add gain diff by experiences, the val is 6 */
		pwdb_all += 6;
		if (pwdb_all > 100)
			pwdb_all = 100;
		/* modify the offset to make the same gain index with OFDM. */
		if (pwdb_all > 34 && pwdb_all <= 42)
			pwdb_all -= 2;
		else if (pwdb_all > 26 && pwdb_all <= 34)
			pwdb_all -= 6;
		else if (pwdb_all > 14 && pwdb_all <= 26)
			pwdb_all -= 8;
		else if (pwdb_all > 4 && pwdb_all <= 14)
			pwdb_all -= 4;

		pstats->rx_pwdb_all = pwdb_all;
		pstats->recvsignalpower = rx_pwr_all;

		if (packet_match_bssid) {
			u8 sq;
			if (pstats->rx_pwdb_all > 40) {
				sq = 100;
			} else {
				sq = cck_buf->sq_rpt;
				if (sq > 64)
					sq = 0;
				else if (sq < 20)
					sq = 100;
				else
					sq = ((64 - sq) * 100) / 44;
			}

			pstats->signalquality = sq;
			pstats->rx_mimo_sig_qual[0] = sq;
			pstats->rx_mimo_sig_qual[1] = -1;
		}
	} else {
		rtlpriv->dm.rfpath_rxenable[0] =
		    rtlpriv->dm.rfpath_rxenable[1] = true;
		for (i = RF90_PATH_A; i < RF6052_MAX_PATH; i++) {
			if (rtlpriv->dm.rfpath_rxenable[i])
				rf_rx_num++;

			rx_pwr[i] = ((p_drvinfo->gain_trsw[i] &
				    0x3f) * 2) - 110;
			rssi = rtl_query_rxpwrpercentage(rx_pwr[i]);
			total_rssi += rssi;
			rtlpriv->stats.rx_snr_db[i] =
					 (long)(p_drvinfo->rxsnr[i] / 2);

			if (packet_match_bssid)
				pstats->rx_mimo_signalstrength[i] = (u8) rssi;
		}

		rx_pwr_all = ((p_drvinfo->pwdb_all >> 1) & 0x7f) - 110;
		pwdb_all = rtl_query_rxpwrpercentage(rx_pwr_all);
		pstats->rx_pwdb_all = pwdb_all;
		pstats->rxpower = rx_pwr_all;
		pstats->recvsignalpower = rx_pwr_all;

		if (pstats->is_ht && pstats->rate >= DESC92_RATEMCS8 &&
		    pstats->rate <= DESC92_RATEMCS15)
			max_spatial_stream = 2;
		else
			max_spatial_stream = 1;

		for (i = 0; i < max_spatial_stream; i++) {
			evm = rtl_evm_db_to_percentage(p_drvinfo->rxevm[i]);

			if (packet_match_bssid) {
				if (i == 0)
					pstats->signalquality = (u8)(evm &
								 0xff);
				pstats->rx_mimo_sig_qual[i] = (u8) (evm & 0xff);
			}
		}
	}

	if (is_cck)
		pstats->signalstrength = (u8)(rtl_signal_scale_mapping(hw,
					 pwdb_all));
	else if (rf_rx_num != 0)
		pstats->signalstrength = (u8) (rtl_signal_scale_mapping(hw,
				total_rssi /= rf_rx_num));
}

static void _rtl92se_translate_rx_signal_stuff(struct ieee80211_hw *hw,
		struct sk_buff *skb, struct rtl_stats *pstats,
		u8 *pdesc, struct rx_fwinfo *p_drvinfo)
{
	struct rtl_mac *mac = rtl_mac(rtl_priv(hw));
	struct rtl_efuse *rtlefuse = rtl_efuse(rtl_priv(hw));

	struct ieee80211_hdr *hdr;
	u8 *tmp_buf;
	u8 *praddr;
	__le16 fc;
	u16 type, cfc;
	bool packet_matchbssid, packet_toself, packet_beacon = false;

	tmp_buf = skb->data + pstats->rx_drvinfo_size + pstats->rx_bufshift;

	hdr = (struct ieee80211_hdr *)tmp_buf;
	fc = hdr->frame_control;
	cfc = le16_to_cpu(fc);
	type = WLAN_FC_GET_TYPE(fc);
	praddr = hdr->addr1;

	packet_matchbssid = ((IEEE80211_FTYPE_CTL != type) &&
	     ether_addr_equal(mac->bssid,
			      (cfc & IEEE80211_FCTL_TODS) ? hdr->addr1 :
			      (cfc & IEEE80211_FCTL_FROMDS) ? hdr->addr2 :
			      hdr->addr3) &&
	     (!pstats->hwerror) && (!pstats->crc) && (!pstats->icv));

	packet_toself = packet_matchbssid &&
	    ether_addr_equal(praddr, rtlefuse->dev_addr);

	if (ieee80211_is_beacon(fc))
		packet_beacon = true;

	_rtl92se_query_rxphystatus(hw, pstats, pdesc, p_drvinfo,
			packet_matchbssid, packet_toself, packet_beacon);
	rtl_process_phyinfo(hw, tmp_buf, pstats);
}

bool rtl92su_rx_query_desc(struct ieee80211_hw *hw, struct rtl_stats *stats,
			   struct ieee80211_rx_status *rx_status, u8 *pdesc,
			   struct sk_buff *skb)
{
	struct rx_fwinfo *p_drvinfo;
	u32 phystatus = (u32)GET_RX_STATUS_DESC_PHY_STATUS(pdesc);
	struct ieee80211_hdr *hdr;
	bool first_ampdu = false;

	stats->length = (u16)GET_RX_STATUS_DESC_PKT_LEN(pdesc);
	stats->rx_drvinfo_size = (u8)GET_RX_STATUS_DESC_DRVINFO_SIZE(pdesc) * 8;
	stats->rx_bufshift = (u8)(GET_RX_STATUS_DESC_SHIFT(pdesc) & 0x03);
	stats->icv = (u16)GET_RX_STATUS_DESC_ICV(pdesc);
	stats->crc = (u16)GET_RX_STATUS_DESC_CRC32(pdesc);
	stats->hwerror = (u16)(stats->crc | stats->icv);
	stats->decrypted = !GET_RX_STATUS_DESC_SWDEC(pdesc);

	stats->rate = (u8)GET_RX_STATUS_DESC_RX_MCS(pdesc);
	stats->shortpreamble = (u16)GET_RX_STATUS_DESC_SPLCP(pdesc);
	stats->isampdu = (bool)(GET_RX_STATUS_DESC_PAGGR(pdesc) == 1);
	stats->isfirst_ampdu = (bool) ((GET_RX_STATUS_DESC_PAGGR(pdesc) == 1)
			       && (GET_RX_STATUS_DESC_FAGGR(pdesc) == 1));
	stats->timestamp_low = GET_RX_STATUS_DESC_TSFL(pdesc);
	stats->rx_is40Mhzpacket = (bool)GET_RX_STATUS_DESC_BW(pdesc);
	stats->is_ht = (bool)GET_RX_STATUS_DESC_RX_HT(pdesc);
	stats->is_cck = SE_RX_HAL_IS_CCK_RATE(pdesc);

	if (stats->hwerror)
		return false;

	rx_status->freq = hw->conf.chandef.chan->center_freq;
	rx_status->band = hw->conf.chandef.chan->band;

	if (stats->crc)
		rx_status->flag |= RX_FLAG_FAILED_FCS_CRC;

	if (stats->rx_is40Mhzpacket)
		rx_status->flag |= RX_FLAG_40MHZ;

	if (stats->is_ht)
		rx_status->flag |= RX_FLAG_HT;

	rx_status->flag |= RX_FLAG_MACTIME_START;

	/* hw will set stats->decrypted true, if it finds the
	 * frame is open data frame or mgmt frame,
	 * hw will not decrypt robust managment frame
	 * for IEEE80211w but still set stats->decrypted
	 * true, so here we should set it back to undecrypted
	 * for IEEE80211w frame, and mac80211 sw will help
	 * to decrypt it */
	if (stats->decrypted) {
		hdr = (struct ieee80211_hdr *)(skb->data +
		       stats->rx_drvinfo_size + stats->rx_bufshift);

		if (!hdr) {
			/* during testing, hdr was NULL here */
			return false;
		}
		if ((_ieee80211_is_robust_mgmt_frame(hdr)) &&
			(ieee80211_has_protected(hdr->frame_control)))
			rx_status->flag &= ~RX_FLAG_DECRYPTED;
		else
			rx_status->flag |= RX_FLAG_DECRYPTED;
	}

	rx_status->rate_idx = rtlwifi_rate_mapping(hw,
			      stats->is_ht, stats->rate, first_ampdu);

	rx_status->mactime = stats->timestamp_low;
	if (phystatus) {
		p_drvinfo = (struct rx_fwinfo *)(skb->data +
						 stats->rx_bufshift);
		_rtl92se_translate_rx_signal_stuff(hw, skb, stats, pdesc,
						   p_drvinfo);
	}

	/*rx_status->qual = stats->signal; */
	rx_status->signal = stats->recvsignalpower + 10;

	return true;
}

void rtl92su_tx_fill_desc(struct ieee80211_hw *hw,
		struct ieee80211_hdr *hdr, u8 *pdesc_tx,
		struct ieee80211_tx_info *info,
		struct ieee80211_sta *sta,
		struct sk_buff *skb,
		u8 hw_queue, struct rtl_tcb_desc *ptcb_desc)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_mac *mac = rtl_mac(rtl_priv(hw));
	struct rtl_hal *rtlhal = rtl_hal(rtl_priv(hw));
	u8 *pdesc;
	u16 seq_number;
	__le16 fc = hdr->frame_control;
	u8 reserved_macid = 0;
	u8 fw_qsel = _rtl92s_map_hwqueue_to_fwqueue(skb, hw_queue);
	bool firstseg = (!(hdr->seq_ctrl & cpu_to_le16(IEEE80211_SCTL_FRAG)));
	bool lastseg = (!(hdr->frame_control &
			cpu_to_le16(IEEE80211_FCTL_MOREFRAGS)));
	u8 bw_40 = 0;

	if (mac->opmode == NL80211_IFTYPE_STATION) {
		bw_40 = mac->bw_40;
	} else if (mac->opmode == NL80211_IFTYPE_AP ||
		mac->opmode == NL80211_IFTYPE_ADHOC) {
		if (sta)
			bw_40 = sta->bandwidth >= IEEE80211_STA_RX_BW_40;
	}

	seq_number = (le16_to_cpu(hdr->seq_ctrl) & IEEE80211_SCTL_SEQ) >> 4;

	rtl_get_tcb_desc(hw, info, sta, skb, ptcb_desc);
	pdesc = (u8 *)skb_push(skb, RTL_TX_HEADER_SIZE);
	memset(pdesc, 0, RTL_TX_HEADER_SIZE);

	if (ieee80211_is_nullfunc(fc) || ieee80211_is_ctl(fc)) {
		firstseg = true;
		lastseg = true;
	}

	if (firstseg) {
		if (rtlpriv->dm.useramask) {
			/* set txdesc macId */
			if (ptcb_desc->mac_id < 32) {
				SET_TX_DESC_MACID(pdesc, ptcb_desc->mac_id);
				reserved_macid |= ptcb_desc->mac_id;
			}
		}
		SET_TX_DESC_RSVD_MACID(pdesc, reserved_macid);

		SET_TX_DESC_TXHT(pdesc, ((ptcb_desc->hw_rate >=
				 DESC92_RATEMCS0) ? 1 : 0));

		if (rtlhal->version == VERSION_8192S_ACUT) {
			if (ptcb_desc->hw_rate == DESC92_RATE1M ||
				ptcb_desc->hw_rate  == DESC92_RATE2M ||
				ptcb_desc->hw_rate == DESC92_RATE5_5M ||
				ptcb_desc->hw_rate == DESC92_RATE11M) {
				ptcb_desc->hw_rate = DESC92_RATE12M;
			}
		}

		SET_TX_DESC_TX_RATE(pdesc, ptcb_desc->hw_rate);

		if (ptcb_desc->use_shortgi || ptcb_desc->use_shortpreamble)
			SET_TX_DESC_TX_SHORT(pdesc, 0);

		/* Aggregation related */
		if (info->flags & IEEE80211_TX_CTL_AMPDU)
			SET_TX_DESC_AGG_ENABLE(pdesc, 1);

		/* For AMPDU, we must insert SSN into TX_DESC */
		SET_TX_DESC_SEQ(pdesc, seq_number);

		/* Protection mode related */
		/* For 92S, if RTS/CTS are set, HW will execute RTS. */
		/* We choose only one protection mode to execute */
		SET_TX_DESC_RTS_ENABLE(pdesc, ((ptcb_desc->rts_enable &&
				!ptcb_desc->cts_enable) ? 1 : 0));
		SET_TX_DESC_CTS_ENABLE(pdesc, ((ptcb_desc->cts_enable) ?
				       1 : 0));
		SET_TX_DESC_RTS_STBC(pdesc, ((ptcb_desc->rts_stbc) ? 1 : 0));

		SET_TX_DESC_RTS_RATE(pdesc, ptcb_desc->rts_rate);
		SET_TX_DESC_RTS_BANDWIDTH(pdesc, 0);
		SET_TX_DESC_RTS_SUB_CARRIER(pdesc, ptcb_desc->rts_sc);
		SET_TX_DESC_RTS_SHORT(pdesc, ((ptcb_desc->rts_rate <=
		       DESC92_RATE54M) ?
		       (ptcb_desc->rts_use_shortpreamble ? 1 : 0)
		       : (ptcb_desc->rts_use_shortgi ? 1 : 0)));


		/* Set Bandwidth and sub-channel settings. */
		if (bw_40) {
			if (ptcb_desc->packet_bw) {
				SET_TX_DESC_TX_BANDWIDTH(pdesc, 1);
				/* use duplicated mode */
				SET_TX_DESC_TX_SUB_CARRIER(pdesc, 0);
			} else {
				SET_TX_DESC_TX_BANDWIDTH(pdesc, 0);
				SET_TX_DESC_TX_SUB_CARRIER(pdesc,
						   mac->cur_40_prime_sc);
			}
		} else {
			SET_TX_DESC_TX_BANDWIDTH(pdesc, 0);
			SET_TX_DESC_TX_SUB_CARRIER(pdesc, 0);
		}

		/* 3 Fill necessary field in First Descriptor */
		/*DWORD 0*/
		SET_TX_DESC_LINIP(pdesc, 0);
		SET_TX_DESC_OFFSET(pdesc, 32);
		SET_TX_DESC_PKT_SIZE(pdesc, (u16) skb->len - RTL_TX_HEADER_SIZE);

		/*DWORD 1*/
		SET_TX_DESC_RA_BRSR_ID(pdesc, ptcb_desc->ratr_index);

		/* Fill security related */
		if (info->control.hw_key) {
			struct ieee80211_key_conf *keyconf;

			keyconf = info->control.hw_key;
			switch (keyconf->cipher) {
			case WLAN_CIPHER_SUITE_WEP40:
			case WLAN_CIPHER_SUITE_WEP104:
				SET_TX_DESC_SEC_TYPE(pdesc, 0x1);
				break;
			case WLAN_CIPHER_SUITE_TKIP:
				SET_TX_DESC_SEC_TYPE(pdesc, 0x2);
				break;
			case WLAN_CIPHER_SUITE_CCMP:
				SET_TX_DESC_SEC_TYPE(pdesc, 0x3);
				break;
			default:
				SET_TX_DESC_SEC_TYPE(pdesc, 0x0);
				break;

			}
		}

		/* Set Packet ID */
		SET_TX_DESC_PACKET_ID(pdesc, 0);

		/* We will assign magement queue to BK. */
		SET_TX_DESC_QUEUE_SEL(pdesc, fw_qsel);

		/* Alwasy enable all rate fallback range */
		SET_TX_DESC_DATA_RATE_FB_LIMIT(pdesc, 0x1F);

		/* Fix: I don't kown why hw use 6.5M to tx when set it */
		SET_TX_DESC_USER_RATE(pdesc,
				      ptcb_desc->use_driver_rate ? 1 : 0);

		/* Set NON_QOS bit. */
		if (!ieee80211_is_data_qos(fc))
			SET_TX_DESC_NON_QOS(pdesc, 1);

	}

	/* Fill fields that are required to be initialized
	 * in all of the descriptors */
	/*DWORD 0 */
	SET_TX_DESC_FIRST_SEG(pdesc, (firstseg ? 1 : 0));
	SET_TX_DESC_LAST_SEG(pdesc, (lastseg ? 1 : 0));
	SET_TX_DESC_OWN(pdesc, 1);

	/* DWORD 7 */
	SET_TX_DESC_TX_BUFFER_SIZE(pdesc, (u16) (skb->len - RTL_TX_HEADER_SIZE));

	RT_TRACE(rtlpriv, COMP_SEND, DBG_TRACE, "\n");
}

void rtl92su_tx_fill_cmddesc(struct ieee80211_hw *hw, u8 *pdesc,
	bool firstseg, bool lastseg, struct sk_buff *skb)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_hal *rtlhal = rtl_hal(rtlpriv);
	struct rtl_tcb_desc *tcb_desc = (struct rtl_tcb_desc *)(skb->cb);

	/* Clear all status	*/
	memset((void *)pdesc, 0, RTL_TX_HEADER_SIZE);

	/* This bit indicate this packet is used for FW download. */
	if (tcb_desc->cmd_or_init == DESC_PACKET_TYPE_INIT) {
		/* For firmware downlaod we only need to set LINIP */
		SET_TX_DESC_LINIP(pdesc, tcb_desc->last_inipkt);

		/* 92SU need not to set TX packet size when firmware download */
		SET_TX_DESC_PKT_SIZE(pdesc, (u16)(skb->len - RTL_TX_HEADER_SIZE));
	} else { /* H2C Command Desc format (Host TXCMD) */
		/* 92SE must set as 1 for firmware download HW DMA error */
		SET_TX_DESC_FIRST_SEG(pdesc, 1);
		SET_TX_DESC_LAST_SEG(pdesc, 1);

		SET_TX_DESC_OFFSET(pdesc, 0x20);

		/* Buffer size + command header */
		SET_TX_DESC_PKT_SIZE(pdesc, (u16)(skb->len - RTL_TX_HEADER_SIZE));
		/* Fixed queue of H2C command */
		SET_TX_DESC_QUEUE_SEL(pdesc, 0x13);

		SET_BITS_TO_LE_4BYTE(skb->data, 24, 7, rtlhal->h2c_txcmd_seq);

		SET_TX_DESC_TX_BUFFER_SIZE(pdesc, (u16)(skb->len));
	}
}

static void _rtl92s_cmd_complete(struct urb *urb)
{
	struct sk_buff *skb = urb->context;
	dev_kfree_skb_irq(skb);
}

bool rtl92su_cmd_send_packet(struct ieee80211_hw *hw, struct sk_buff *skb)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_tcb_desc *tcb_desc;
	u8 *pdesc;
	int err;

	pdesc = (u8 *)skb_push(skb, RTL_TX_HEADER_SIZE);
	rtlpriv->cfg->ops->fill_tx_cmddesc(hw, pdesc, 1, 1, skb);

	tcb_desc = (struct rtl_tcb_desc *)(skb->cb);
	err = rtl_usb_transmit(hw, skb, tcb_desc->queue_index,
				_rtl92s_cmd_complete);
	return err ? false : true;
}

#define RTL_RX_DRV_INFO_UNIT		8

static void _rtl_rx_process(struct ieee80211_hw *hw, struct sk_buff *skb)
{
	struct ieee80211_rx_status *rx_status =
		 (struct ieee80211_rx_status *)IEEE80211_SKB_RXCB(skb);
	u32 skb_len, pkt_len, drvinfo_len;
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	u8 *rxdesc;
	struct rtl_stats stats = {
		.signal = 0,
		.rate = 0,
	};
	struct rx_fwinfo *p_drvinfo;
	bool bv;
	__le16 fc;
	struct ieee80211_hdr *hdr;

	memset(rx_status, 0, sizeof(*rx_status));
	rxdesc	= skb->data;
	skb_len	= skb->len;
	drvinfo_len = (GET_RX_STATUS_DESC_DRVINFO_SIZE(rxdesc) *
		       RTL_RX_DRV_INFO_UNIT);
	pkt_len		= GET_RX_STATUS_DESC_PKT_LEN(rxdesc);
	/* TODO: Error recovery. drop this skb or something. */
	WARN_ON(skb_len < (pkt_len + RTL_RX_DESC_SIZE + drvinfo_len));
	stats.length = (u16) GET_RX_STATUS_DESC_PKT_LEN(rxdesc);
	stats.rx_drvinfo_size = (u8)GET_RX_STATUS_DESC_DRVINFO_SIZE(rxdesc) *
				RX_DRV_INFO_SIZE_UNIT;
	stats.rx_bufshift = (u8) (GET_RX_STATUS_DESC_SHIFT(rxdesc) & 0x03);
	stats.icv = (u16) GET_RX_STATUS_DESC_ICV(rxdesc);
	stats.crc = (u16) GET_RX_STATUS_DESC_CRC32(rxdesc);
	stats.hwerror = (stats.crc | stats.icv);
	stats.decrypted = !GET_RX_STATUS_DESC_SWDEC(rxdesc);
	stats.rate = (u8) GET_RX_STATUS_DESC_RX_MCS(rxdesc);
	stats.shortpreamble = (u16) GET_RX_STATUS_DESC_SPLCP(rxdesc);
	stats.isampdu = (bool) ((GET_RX_STATUS_DESC_PAGGR(rxdesc) == 1)
				   && (GET_RX_STATUS_DESC_FAGGR(rxdesc) == 1));
	stats.timestamp_low = GET_RX_STATUS_DESC_TSFL(rxdesc);
	stats.rx_is40Mhzpacket = (bool) GET_RX_STATUS_DESC_BW(rxdesc);
	/* TODO: is center_freq changed when doing scan? */
	/* TODO: Shall we add protection or just skip those two step? */
	rx_status->freq = hw->conf.chandef.chan->center_freq;
	rx_status->band = hw->conf.chandef.chan->band;
	if (GET_RX_STATUS_DESC_CRC32(rxdesc))
		rx_status->flag |= RX_FLAG_FAILED_FCS_CRC;
	if (!GET_RX_STATUS_DESC_SWDEC(rxdesc))
		rx_status->flag |= RX_FLAG_DECRYPTED;
	if (GET_RX_STATUS_DESC_BW(rxdesc))
		rx_status->flag |= RX_FLAG_40MHZ;
	if (GET_RX_STATUS_DESC_RX_HT(rxdesc))
		rx_status->flag |= RX_FLAG_HT;
	/* Data rate */
	rx_status->rate_idx = rtlwifi_rate_mapping(hw,
		(bool)GET_RX_STATUS_DESC_RX_HT(rxdesc),
		(u8)GET_RX_STATUS_DESC_RX_MCS(rxdesc),
		(bool)GET_RX_STATUS_DESC_PAGGR(rxdesc));
	/*  There is a phy status after this rx descriptor. */
	if (GET_RX_STATUS_DESC_PHY_STATUS(rxdesc)) {
		p_drvinfo = (struct rx_fwinfo *)(skb->data +
						 stats.rx_bufshift);
		_rtl92se_translate_rx_signal_stuff(hw, skb, &stats, rxdesc,
						   p_drvinfo);
	}
	skb_pull(skb, (drvinfo_len + RTL_RX_DESC_SIZE));
	hdr = (struct ieee80211_hdr *)(skb->data);
	fc = hdr->frame_control;
	bv = ieee80211_is_probe_resp(fc);
	if (bv)
		RT_TRACE(rtlpriv, COMP_INIT, DBG_DMESG,
			 "Got probe response frame\n");
	if (ieee80211_is_beacon(fc))
		RT_TRACE(rtlpriv, COMP_INIT, DBG_DMESG, "Got beacon frame\n");
	if (ieee80211_is_data(fc))
		RT_TRACE(rtlpriv, COMP_INIT, DBG_DMESG, "Got data frame\n");
	RT_TRACE(rtlpriv, COMP_INIT, DBG_DMESG,
		 "Fram: fc = 0x%X addr1 = 0x%02X:0x%02X:0x%02X:0x%02X:0x%02X:0x%02X\n",
		 fc,
		 (u32)hdr->addr1[0], (u32)hdr->addr1[1],
		 (u32)hdr->addr1[2], (u32)hdr->addr1[3],
		 (u32)hdr->addr1[4], (u32)hdr->addr1[5]);
	memcpy(IEEE80211_SKB_RXCB(skb), rx_status, sizeof(*rx_status));
	ieee80211_rx(hw, skb);
}

// not needed?
void rtl92su_rx_hdl(struct ieee80211_hw *hw, struct sk_buff * skb)
{
	_rtl_rx_process(hw, skb);
}

void rtl92s_rx_segregate_hdl(
	struct ieee80211_hw *hw,
	struct sk_buff *skb,
	struct sk_buff_head *skb_list)
{
}

void rtl92su_tx_cleanup(struct ieee80211_hw *hw, struct sk_buff  *skb)
{
}

int rtl92su_tx_post_hdl(struct ieee80211_hw *hw, struct urb *urb,
                         struct sk_buff *skb)
{
        return 0;
}

struct sk_buff *rtl92su_tx_aggregate_hdl(struct ieee80211_hw *hw,
                                         struct sk_buff_head *list)
{
        return skb_dequeue(list);
}

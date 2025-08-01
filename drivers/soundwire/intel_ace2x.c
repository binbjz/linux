// SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause)
// Copyright(c) 2023 Intel Corporation

/*
 * Soundwire Intel ops for LunarLake
 */

#include <linux/acpi.h>
#include <linux/cleanup.h>
#include <linux/device.h>
#include <linux/soundwire/sdw_registers.h>
#include <linux/soundwire/sdw.h>
#include <linux/soundwire/sdw_intel.h>
#include <linux/string_choices.h>
#include <sound/hdaudio.h>
#include <sound/hda-mlink.h>
#include <sound/hda-sdw-bpt.h>
#include <sound/hda_register.h>
#include <sound/pcm_params.h>
#include "cadence_master.h"
#include "bus.h"
#include "intel.h"

static int sdw_slave_bpt_stream_add(struct sdw_slave *slave, struct sdw_stream_runtime *stream)
{
	struct sdw_stream_config sconfig = {0};
	struct sdw_port_config pconfig = {0};
	int ret;

	/* arbitrary configuration */
	sconfig.frame_rate = 16000;
	sconfig.ch_count = 1;
	sconfig.bps = 32; /* this is required for BPT/BRA */
	sconfig.direction = SDW_DATA_DIR_RX;
	sconfig.type = SDW_STREAM_BPT;

	pconfig.num = 0;
	pconfig.ch_mask = BIT(0);

	ret = sdw_stream_add_slave(slave, &sconfig, &pconfig, 1, stream);
	if (ret)
		dev_err(&slave->dev, "%s: failed: %d\n", __func__, ret);

	return ret;
}

static int intel_ace2x_bpt_open_stream(struct sdw_intel *sdw, struct sdw_slave *slave,
				       struct sdw_bpt_msg *msg)
{
	struct sdw_cdns *cdns = &sdw->cdns;
	struct sdw_bus *bus = &cdns->bus;
	struct sdw_master_prop *prop = &bus->prop;
	struct sdw_stream_runtime *stream;
	struct sdw_stream_config sconfig;
	struct sdw_port_config *pconfig;
	unsigned int pdi0_buffer_size;
	unsigned int tx_dma_bandwidth;
	unsigned int pdi1_buffer_size;
	unsigned int rx_dma_bandwidth;
	unsigned int data_per_frame;
	unsigned int tx_total_bytes;
	struct sdw_cdns_pdi *pdi0;
	struct sdw_cdns_pdi *pdi1;
	unsigned int num_frames;
	int command;
	int ret1;
	int ret;
	int dir;
	int i;

	stream = sdw_alloc_stream("BPT", SDW_STREAM_BPT);
	if (!stream)
		return -ENOMEM;

	cdns->bus.bpt_stream = stream;

	ret = sdw_slave_bpt_stream_add(slave, stream);
	if (ret < 0)
		goto release_stream;

	/* handle PDI0 first */
	dir = SDW_DATA_DIR_TX;

	pdi0 = sdw_cdns_alloc_pdi(cdns, &cdns->pcm, 1,  dir, 0);
	if (!pdi0) {
		dev_err(cdns->dev, "%s: sdw_cdns_alloc_pdi0 failed\n", __func__);
		ret = -EINVAL;
		goto remove_slave;
	}

	sdw_cdns_config_stream(cdns, 1, dir, pdi0);

	/* handle PDI1  */
	dir = SDW_DATA_DIR_RX;

	pdi1 = sdw_cdns_alloc_pdi(cdns, &cdns->pcm, 1,  dir, 1);
	if (!pdi1) {
		dev_err(cdns->dev, "%s: sdw_cdns_alloc_pdi1 failed\n", __func__);
		ret = -EINVAL;
		goto remove_slave;
	}

	sdw_cdns_config_stream(cdns, 1, dir, pdi1);

	/*
	 * the port config direction, number of channels and frame
	 * rate is totally arbitrary
	 */
	sconfig.direction = dir;
	sconfig.ch_count = 1;
	sconfig.frame_rate = 16000;
	sconfig.type = SDW_STREAM_BPT;
	sconfig.bps = 32; /* this is required for BPT/BRA */

	/* Port configuration */
	pconfig = kcalloc(2, sizeof(*pconfig), GFP_KERNEL);
	if (!pconfig) {
		ret =  -ENOMEM;
		goto remove_slave;
	}

	for (i = 0; i < 2 /* num_pdi */; i++) {
		pconfig[i].num = i;
		pconfig[i].ch_mask = 1;
	}

	ret = sdw_stream_add_master(&cdns->bus, &sconfig, pconfig, 2, stream);
	kfree(pconfig);

	if (ret < 0) {
		dev_err(cdns->dev, "add master to stream failed:%d\n", ret);
		goto remove_slave;
	}

	ret = sdw_prepare_stream(cdns->bus.bpt_stream);
	if (ret < 0)
		goto remove_master;

	command = (msg->flags & SDW_MSG_FLAG_WRITE) ? 0 : 1;

	ret = sdw_cdns_bpt_find_buffer_sizes(command, cdns->bus.params.row, cdns->bus.params.col,
					     msg->len, SDW_BPT_MSG_MAX_BYTES, &data_per_frame,
					     &pdi0_buffer_size, &pdi1_buffer_size, &num_frames);
	if (ret < 0)
		goto deprepare_stream;

	sdw->bpt_ctx.pdi0_buffer_size = pdi0_buffer_size;
	sdw->bpt_ctx.pdi1_buffer_size = pdi1_buffer_size;
	sdw->bpt_ctx.num_frames = num_frames;
	sdw->bpt_ctx.data_per_frame = data_per_frame;
	tx_dma_bandwidth = div_u64((u64)pdi0_buffer_size * 8 * (u64)prop->default_frame_rate,
				   num_frames);
	rx_dma_bandwidth = div_u64((u64)pdi1_buffer_size * 8 * (u64)prop->default_frame_rate,
				   num_frames);

	dev_dbg(cdns->dev, "Message len %d transferred in %d frames (%d per frame)\n",
		msg->len, num_frames, data_per_frame);
	dev_dbg(cdns->dev, "sizes pdi0 %d pdi1 %d tx_bandwidth %d rx_bandwidth %d\n",
		pdi0_buffer_size, pdi1_buffer_size, tx_dma_bandwidth, rx_dma_bandwidth);

	ret = hda_sdw_bpt_open(cdns->dev->parent, /* PCI device */
			       sdw->instance, &sdw->bpt_ctx.bpt_tx_stream,
			       &sdw->bpt_ctx.dmab_tx_bdl, pdi0_buffer_size, tx_dma_bandwidth,
			       &sdw->bpt_ctx.bpt_rx_stream, &sdw->bpt_ctx.dmab_rx_bdl,
			       pdi1_buffer_size, rx_dma_bandwidth);
	if (ret < 0) {
		dev_err(cdns->dev, "%s: hda_sdw_bpt_open failed %d\n", __func__, ret);
		goto deprepare_stream;
	}

	if (!command) {
		ret = sdw_cdns_prepare_write_dma_buffer(msg->dev_num, msg->addr, msg->buf,
							msg->len, data_per_frame,
							sdw->bpt_ctx.dmab_tx_bdl.area,
							pdi0_buffer_size, &tx_total_bytes);
	} else {
		ret = sdw_cdns_prepare_read_dma_buffer(msg->dev_num, msg->addr,	msg->len,
						       data_per_frame,
						       sdw->bpt_ctx.dmab_tx_bdl.area,
						       pdi0_buffer_size, &tx_total_bytes);
	}

	if (!ret)
		return 0;

	dev_err(cdns->dev, "%s: sdw_prepare_%s_dma_buffer failed %d\n",
		__func__, str_read_write(command), ret);

	ret1 = hda_sdw_bpt_close(cdns->dev->parent, /* PCI device */
				 sdw->bpt_ctx.bpt_tx_stream, &sdw->bpt_ctx.dmab_tx_bdl,
				 sdw->bpt_ctx.bpt_rx_stream, &sdw->bpt_ctx.dmab_rx_bdl);
	if (ret1 < 0)
		dev_err(cdns->dev, "%s:  hda_sdw_bpt_close failed: ret %d\n",
			__func__, ret1);

deprepare_stream:
	sdw_deprepare_stream(cdns->bus.bpt_stream);

remove_master:
	ret1 = sdw_stream_remove_master(&cdns->bus, cdns->bus.bpt_stream);
	if (ret1 < 0)
		dev_err(cdns->dev, "%s: remove master failed: %d\n",
			__func__, ret1);

remove_slave:
	ret1 = sdw_stream_remove_slave(slave, cdns->bus.bpt_stream);
	if (ret1 < 0)
		dev_err(cdns->dev, "%s: remove slave failed: %d\n",
			__func__, ret1);

release_stream:
	sdw_release_stream(cdns->bus.bpt_stream);
	cdns->bus.bpt_stream = NULL;

	return ret;
}

static void intel_ace2x_bpt_close_stream(struct sdw_intel *sdw, struct sdw_slave *slave,
					 struct sdw_bpt_msg *msg)
{
	struct sdw_cdns *cdns = &sdw->cdns;
	int ret;

	ret = hda_sdw_bpt_close(cdns->dev->parent /* PCI device */, sdw->bpt_ctx.bpt_tx_stream,
				&sdw->bpt_ctx.dmab_tx_bdl, sdw->bpt_ctx.bpt_rx_stream,
				&sdw->bpt_ctx.dmab_rx_bdl);
	if (ret < 0)
		dev_err(cdns->dev, "%s:  hda_sdw_bpt_close failed: ret %d\n",
			__func__, ret);

	ret = sdw_deprepare_stream(cdns->bus.bpt_stream);
	if (ret < 0)
		dev_err(cdns->dev, "%s: sdw_deprepare_stream failed: ret %d\n",
			__func__, ret);

	ret = sdw_stream_remove_master(&cdns->bus, cdns->bus.bpt_stream);
	if (ret < 0)
		dev_err(cdns->dev, "%s: remove master failed: %d\n",
			__func__, ret);

	ret = sdw_stream_remove_slave(slave, cdns->bus.bpt_stream);
	if (ret < 0)
		dev_err(cdns->dev, "%s: remove slave failed: %d\n",
			__func__, ret);

	cdns->bus.bpt_stream = NULL;
}

#define INTEL_BPT_MSG_BYTE_MIN 16

static int intel_ace2x_bpt_send_async(struct sdw_intel *sdw, struct sdw_slave *slave,
				      struct sdw_bpt_msg *msg)
{
	struct sdw_cdns *cdns = &sdw->cdns;
	int ret;

	if (msg->len < INTEL_BPT_MSG_BYTE_MIN) {
		dev_err(cdns->dev, "BPT message length %d is less than the minimum bytes %d\n",
			msg->len, INTEL_BPT_MSG_BYTE_MIN);
		return -EINVAL;
	}

	dev_dbg(cdns->dev, "BPT Transfer start\n");

	ret = intel_ace2x_bpt_open_stream(sdw, slave, msg);
	if (ret < 0)
		return ret;

	ret = hda_sdw_bpt_send_async(cdns->dev->parent, /* PCI device */
				     sdw->bpt_ctx.bpt_tx_stream, sdw->bpt_ctx.bpt_rx_stream);
	if (ret < 0) {
		dev_err(cdns->dev, "%s:   hda_sdw_bpt_send_async failed: %d\n",
			__func__, ret);

		intel_ace2x_bpt_close_stream(sdw, slave, msg);

		return ret;
	}

	ret = sdw_enable_stream(cdns->bus.bpt_stream);
	if (ret < 0) {
		dev_err(cdns->dev, "%s: sdw_stream_enable failed: %d\n",
			__func__, ret);
		intel_ace2x_bpt_close_stream(sdw, slave, msg);
	}

	return ret;
}

static int intel_ace2x_bpt_wait(struct sdw_intel *sdw, struct sdw_slave *slave,
				struct sdw_bpt_msg *msg)
{
	struct sdw_cdns *cdns = &sdw->cdns;
	int ret;

	dev_dbg(cdns->dev, "BPT Transfer wait\n");

	ret = hda_sdw_bpt_wait(cdns->dev->parent, /* PCI device */
			       sdw->bpt_ctx.bpt_tx_stream, sdw->bpt_ctx.bpt_rx_stream);
	if (ret < 0)
		dev_err(cdns->dev, "%s: hda_sdw_bpt_wait failed: %d\n", __func__, ret);

	ret = sdw_disable_stream(cdns->bus.bpt_stream);
	if (ret < 0) {
		dev_err(cdns->dev, "%s: sdw_stream_enable failed: %d\n",
			__func__, ret);
		goto err;
	}

	if (msg->flags & SDW_MSG_FLAG_WRITE) {
		ret = sdw_cdns_check_write_response(cdns->dev, sdw->bpt_ctx.dmab_rx_bdl.area,
						    sdw->bpt_ctx.pdi1_buffer_size,
						    sdw->bpt_ctx.num_frames);
		if (ret < 0)
			dev_err(cdns->dev, "%s: BPT Write failed %d\n", __func__, ret);
	} else {
		ret = sdw_cdns_check_read_response(cdns->dev, sdw->bpt_ctx.dmab_rx_bdl.area,
						   sdw->bpt_ctx.pdi1_buffer_size,
						   msg->buf, msg->len, sdw->bpt_ctx.num_frames,
						   sdw->bpt_ctx.data_per_frame);
		if (ret < 0)
			dev_err(cdns->dev, "%s: BPT Read failed %d\n", __func__, ret);
	}

err:
	intel_ace2x_bpt_close_stream(sdw, slave, msg);

	return ret;
}

/*
 * shim vendor-specific (vs) ops
 */

static void intel_shim_vs_init(struct sdw_intel *sdw)
{
	void __iomem *shim_vs = sdw->link_res->shim_vs;
	struct sdw_bus *bus = &sdw->cdns.bus;
	struct sdw_intel_prop *intel_prop;
	u16 clde;
	u16 doaise2;
	u16 dodse2;
	u16 clds;
	u16 clss;
	u16 doaise;
	u16 doais;
	u16 dodse;
	u16 dods;
	u16 act;

	intel_prop = bus->vendor_specific_prop;
	clde = intel_prop->clde;
	doaise2 = intel_prop->doaise2;
	dodse2 = intel_prop->dodse2;
	clds = intel_prop->clds;
	clss = intel_prop->clss;
	doaise = intel_prop->doaise;
	doais = intel_prop->doais;
	dodse = intel_prop->dodse;
	dods = intel_prop->dods;

	act = intel_readw(shim_vs, SDW_SHIM2_INTEL_VS_ACTMCTL);
	u16p_replace_bits(&act, clde, SDW_SHIM3_INTEL_VS_ACTMCTL_CLDE);
	u16p_replace_bits(&act, doaise2, SDW_SHIM3_INTEL_VS_ACTMCTL_DOAISE2);
	u16p_replace_bits(&act, dodse2, SDW_SHIM3_INTEL_VS_ACTMCTL_DODSE2);
	u16p_replace_bits(&act, clds, SDW_SHIM3_INTEL_VS_ACTMCTL_CLDS);
	u16p_replace_bits(&act, clss, SDW_SHIM3_INTEL_VS_ACTMCTL_CLSS);
	u16p_replace_bits(&act, doaise, SDW_SHIM2_INTEL_VS_ACTMCTL_DOAISE);
	u16p_replace_bits(&act, doais, SDW_SHIM2_INTEL_VS_ACTMCTL_DOAIS);
	u16p_replace_bits(&act, dodse, SDW_SHIM2_INTEL_VS_ACTMCTL_DODSE);
	u16p_replace_bits(&act, dods, SDW_SHIM2_INTEL_VS_ACTMCTL_DODS);
	act |= SDW_SHIM2_INTEL_VS_ACTMCTL_DACTQE;
	intel_writew(shim_vs, SDW_SHIM2_INTEL_VS_ACTMCTL, act);
	usleep_range(10, 15);
}

static void intel_shim_vs_set_clock_source(struct sdw_intel *sdw, u32 source)
{
	void __iomem *shim_vs = sdw->link_res->shim_vs;
	u32 val;

	val = intel_readl(shim_vs, SDW_SHIM2_INTEL_VS_LVSCTL);

	u32p_replace_bits(&val, source, SDW_SHIM2_INTEL_VS_LVSCTL_MLCS);

	intel_writel(shim_vs, SDW_SHIM2_INTEL_VS_LVSCTL, val);

	dev_dbg(sdw->cdns.dev, "clock source %d LVSCTL %#x\n", source, val);
}

static int intel_shim_check_wake(struct sdw_intel *sdw)
{
	/*
	 * We follow the HDaudio example and resume unconditionally
	 * without checking the WAKESTS bit for that specific link
	 */

	return 1;
}

static void intel_shim_wake(struct sdw_intel *sdw, bool wake_enable)
{
	u16 lsdiid = 0;
	u16 wake_en;
	u16 wake_sts;
	int ret;

	mutex_lock(sdw->link_res->shim_lock);

	ret = hdac_bus_eml_sdw_get_lsdiid_unlocked(sdw->link_res->hbus, sdw->instance, &lsdiid);
	if (ret < 0)
		goto unlock;

	wake_en = snd_hdac_chip_readw(sdw->link_res->hbus, WAKEEN);

	if (wake_enable) {
		/* Enable the wakeup */
		wake_en |= lsdiid;

		snd_hdac_chip_writew(sdw->link_res->hbus, WAKEEN, wake_en);
	} else {
		/* Disable the wake up interrupt */
		wake_en &= ~lsdiid;
		snd_hdac_chip_writew(sdw->link_res->hbus, WAKEEN, wake_en);

		/* Clear wake status (W1C) */
		wake_sts = snd_hdac_chip_readw(sdw->link_res->hbus, STATESTS);
		wake_sts |= lsdiid;
		snd_hdac_chip_writew(sdw->link_res->hbus, STATESTS, wake_sts);
	}
unlock:
	mutex_unlock(sdw->link_res->shim_lock);
}

static int intel_link_power_up(struct sdw_intel *sdw)
{
	struct sdw_bus *bus = &sdw->cdns.bus;
	struct sdw_master_prop *prop = &bus->prop;
	u32 *shim_mask = sdw->link_res->shim_mask;
	unsigned int link_id = sdw->instance;
	u32 clock_source;
	u32 syncprd;
	int ret;

	if (prop->mclk_freq % 6000000) {
		if (prop->mclk_freq % 2400000) {
			syncprd = SDW_SHIM_SYNC_SYNCPRD_VAL_24_576;
			clock_source = SDW_SHIM2_MLCS_CARDINAL_CLK;
		} else {
			syncprd = SDW_SHIM_SYNC_SYNCPRD_VAL_38_4;
			clock_source = SDW_SHIM2_MLCS_XTAL_CLK;
		}
	} else {
		syncprd = SDW_SHIM_SYNC_SYNCPRD_VAL_96;
		clock_source = SDW_SHIM2_MLCS_AUDIO_PLL_CLK;
	}

	mutex_lock(sdw->link_res->shim_lock);

	ret = hdac_bus_eml_sdw_power_up_unlocked(sdw->link_res->hbus, link_id);
	if (ret < 0) {
		dev_err(sdw->cdns.dev, "%s: hdac_bus_eml_sdw_power_up failed: %d\n",
			__func__, ret);
		goto out;
	}

	intel_shim_vs_set_clock_source(sdw, clock_source);

	if (!*shim_mask) {
		/* we first need to program the SyncPRD/CPU registers */
		dev_dbg(sdw->cdns.dev, "first link up, programming SYNCPRD\n");

		ret =  hdac_bus_eml_sdw_set_syncprd_unlocked(sdw->link_res->hbus, syncprd);
		if (ret < 0) {
			dev_err(sdw->cdns.dev, "%s: hdac_bus_eml_sdw_set_syncprd failed: %d\n",
				__func__, ret);
			goto out;
		}

		/* SYNCPU will change once link is active */
		ret =  hdac_bus_eml_sdw_wait_syncpu_unlocked(sdw->link_res->hbus);
		if (ret < 0) {
			dev_err(sdw->cdns.dev, "%s: hdac_bus_eml_sdw_wait_syncpu failed: %d\n",
				__func__, ret);
			goto out;
		}

		hdac_bus_eml_enable_interrupt_unlocked(sdw->link_res->hbus, true,
						       AZX_REG_ML_LEPTR_ID_SDW, true);
	}

	*shim_mask |= BIT(link_id);

	sdw->cdns.link_up = true;

	intel_shim_vs_init(sdw);

out:
	mutex_unlock(sdw->link_res->shim_lock);

	return ret;
}

static int intel_link_power_down(struct sdw_intel *sdw)
{
	u32 *shim_mask = sdw->link_res->shim_mask;
	unsigned int link_id = sdw->instance;
	int ret;

	mutex_lock(sdw->link_res->shim_lock);

	sdw->cdns.link_up = false;

	*shim_mask &= ~BIT(link_id);

	if (!*shim_mask)
		hdac_bus_eml_enable_interrupt_unlocked(sdw->link_res->hbus, true,
						       AZX_REG_ML_LEPTR_ID_SDW, false);

	ret = hdac_bus_eml_sdw_power_down_unlocked(sdw->link_res->hbus, link_id);
	if (ret < 0) {
		dev_err(sdw->cdns.dev, "%s: hdac_bus_eml_sdw_power_down failed: %d\n",
			__func__, ret);

		/*
		 * we leave the sdw->cdns.link_up flag as false since we've disabled
		 * the link at this point and cannot handle interrupts any longer.
		 */
	}

	mutex_unlock(sdw->link_res->shim_lock);

	return ret;
}

static void intel_sync_arm(struct sdw_intel *sdw)
{
	unsigned int link_id = sdw->instance;

	mutex_lock(sdw->link_res->shim_lock);

	hdac_bus_eml_sdw_sync_arm_unlocked(sdw->link_res->hbus, link_id);

	mutex_unlock(sdw->link_res->shim_lock);
}

static int intel_sync_go_unlocked(struct sdw_intel *sdw)
{
	int ret;

	ret = hdac_bus_eml_sdw_sync_go_unlocked(sdw->link_res->hbus);
	if (ret < 0)
		dev_err(sdw->cdns.dev, "%s: SyncGO clear failed: %d\n", __func__, ret);

	return ret;
}

static int intel_sync_go(struct sdw_intel *sdw)
{
	int ret;

	mutex_lock(sdw->link_res->shim_lock);

	ret = intel_sync_go_unlocked(sdw);

	mutex_unlock(sdw->link_res->shim_lock);

	return ret;
}

static bool intel_check_cmdsync_unlocked(struct sdw_intel *sdw)
{
	return hdac_bus_eml_sdw_check_cmdsync_unlocked(sdw->link_res->hbus);
}

/* DAI callbacks */
static int intel_params_stream(struct sdw_intel *sdw,
			       struct snd_pcm_substream *substream,
			       struct snd_soc_dai *dai,
			       struct snd_pcm_hw_params *hw_params,
			       int link_id, int alh_stream_id)
{
	struct sdw_intel_link_res *res = sdw->link_res;
	struct sdw_intel_stream_params_data params_data;

	params_data.substream = substream;
	params_data.dai = dai;
	params_data.hw_params = hw_params;
	params_data.link_id = link_id;
	params_data.alh_stream_id = alh_stream_id;

	if (res->ops && res->ops->params_stream && res->dev)
		return res->ops->params_stream(res->dev,
					       &params_data);
	return -EIO;
}

static int intel_free_stream(struct sdw_intel *sdw,
			     struct snd_pcm_substream *substream,
			     struct snd_soc_dai *dai,
			     int link_id)

{
	struct sdw_intel_link_res *res = sdw->link_res;
	struct sdw_intel_stream_free_data free_data;

	free_data.substream = substream;
	free_data.dai = dai;
	free_data.link_id = link_id;

	if (res->ops && res->ops->free_stream && res->dev)
		return res->ops->free_stream(res->dev,
					     &free_data);

	return 0;
}

/*
 * DAI operations
 */
static int intel_hw_params(struct snd_pcm_substream *substream,
			   struct snd_pcm_hw_params *params,
			   struct snd_soc_dai *dai)
{
	struct sdw_cdns *cdns = snd_soc_dai_get_drvdata(dai);
	struct sdw_intel *sdw = cdns_to_intel(cdns);
	struct sdw_cdns_dai_runtime *dai_runtime;
	struct sdw_cdns_pdi *pdi;
	struct sdw_stream_config sconfig;
	int ch, dir;
	int ret;

	dai_runtime = cdns->dai_runtime_array[dai->id];
	if (!dai_runtime)
		return -EIO;

	ch = params_channels(params);
	if (substream->stream == SNDRV_PCM_STREAM_CAPTURE)
		dir = SDW_DATA_DIR_RX;
	else
		dir = SDW_DATA_DIR_TX;

	pdi = sdw_cdns_alloc_pdi(cdns, &cdns->pcm, ch, dir, dai->id);
	if (!pdi)
		return -EINVAL;

	/* use same definitions for alh_id as previous generations */
	pdi->intel_alh_id = (sdw->instance * 16) + pdi->num + 3;
	if (pdi->num >= 2)
		pdi->intel_alh_id += 2;

	/* the SHIM will be configured in the callback functions */

	sdw_cdns_config_stream(cdns, ch, dir, pdi);

	/* store pdi and state, may be needed in prepare step */
	dai_runtime->paused = false;
	dai_runtime->suspended = false;
	dai_runtime->pdi = pdi;

	/* Inform DSP about PDI stream number */
	ret = intel_params_stream(sdw, substream, dai, params,
				  sdw->instance,
				  pdi->intel_alh_id);
	if (ret)
		return ret;

	sconfig.direction = dir;
	sconfig.ch_count = ch;
	sconfig.frame_rate = params_rate(params);
	sconfig.type = dai_runtime->stream_type;

	sconfig.bps = snd_pcm_format_width(params_format(params));

	/* Port configuration */
	struct sdw_port_config *pconfig __free(kfree) = kzalloc(sizeof(*pconfig),
								GFP_KERNEL);
	if (!pconfig)
		return -ENOMEM;

	pconfig->num = pdi->num;
	pconfig->ch_mask = (1 << ch) - 1;

	ret = sdw_stream_add_master(&cdns->bus, &sconfig,
				    pconfig, 1, dai_runtime->stream);
	if (ret)
		dev_err(cdns->dev, "add master to stream failed:%d\n", ret);

	return ret;
}

static int intel_prepare(struct snd_pcm_substream *substream,
			 struct snd_soc_dai *dai)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct sdw_cdns *cdns = snd_soc_dai_get_drvdata(dai);
	struct sdw_intel *sdw = cdns_to_intel(cdns);
	struct sdw_cdns_dai_runtime *dai_runtime;
	struct snd_pcm_hw_params *hw_params;
	int ch, dir;

	dai_runtime = cdns->dai_runtime_array[dai->id];
	if (!dai_runtime) {
		dev_err(dai->dev, "failed to get dai runtime in %s\n",
			__func__);
		return -EIO;
	}

	hw_params = &rtd->dpcm[substream->stream].hw_params;
	if (dai_runtime->suspended) {
		dai_runtime->suspended = false;

		/*
		 * .prepare() is called after system resume, where we
		 * need to reinitialize the SHIM/ALH/Cadence IP.
		 * .prepare() is also called to deal with underflows,
		 * but in those cases we cannot touch ALH/SHIM
		 * registers
		 */

		/* configure stream */
		ch = params_channels(hw_params);
		if (substream->stream == SNDRV_PCM_STREAM_CAPTURE)
			dir = SDW_DATA_DIR_RX;
		else
			dir = SDW_DATA_DIR_TX;

		/* the SHIM will be configured in the callback functions */

		sdw_cdns_config_stream(cdns, ch, dir, dai_runtime->pdi);
	}

	/* Inform DSP about PDI stream number */
	return intel_params_stream(sdw, substream, dai, hw_params, sdw->instance,
				   dai_runtime->pdi->intel_alh_id);
}

static int
intel_hw_free(struct snd_pcm_substream *substream, struct snd_soc_dai *dai)
{
	struct sdw_cdns *cdns = snd_soc_dai_get_drvdata(dai);
	struct sdw_intel *sdw = cdns_to_intel(cdns);
	struct sdw_cdns_dai_runtime *dai_runtime;
	int ret;

	dai_runtime = cdns->dai_runtime_array[dai->id];
	if (!dai_runtime)
		return -EIO;

	/*
	 * The sdw stream state will transition to RELEASED when stream->
	 * master_list is empty. So the stream state will transition to
	 * DEPREPARED for the first cpu-dai and to RELEASED for the last
	 * cpu-dai.
	 */
	ret = sdw_stream_remove_master(&cdns->bus, dai_runtime->stream);
	if (ret < 0) {
		dev_err(dai->dev, "remove master from stream %s failed: %d\n",
			dai_runtime->stream->name, ret);
		return ret;
	}

	ret = intel_free_stream(sdw, substream, dai, sdw->instance);
	if (ret < 0) {
		dev_err(dai->dev, "intel_free_stream: failed %d\n", ret);
		return ret;
	}

	dai_runtime->pdi = NULL;

	return 0;
}

static int intel_pcm_set_sdw_stream(struct snd_soc_dai *dai,
				    void *stream, int direction)
{
	return cdns_set_sdw_stream(dai, stream, direction);
}

static void *intel_get_sdw_stream(struct snd_soc_dai *dai,
				  int direction)
{
	struct sdw_cdns *cdns = snd_soc_dai_get_drvdata(dai);
	struct sdw_cdns_dai_runtime *dai_runtime;

	dai_runtime = cdns->dai_runtime_array[dai->id];
	if (!dai_runtime)
		return ERR_PTR(-EINVAL);

	return dai_runtime->stream;
}

static int intel_trigger(struct snd_pcm_substream *substream, int cmd, struct snd_soc_dai *dai)
{
	struct sdw_cdns *cdns = snd_soc_dai_get_drvdata(dai);
	struct sdw_intel *sdw = cdns_to_intel(cdns);
	struct sdw_intel_link_res *res = sdw->link_res;
	struct sdw_cdns_dai_runtime *dai_runtime;
	int ret = 0;

	/*
	 * The .trigger callback is used to program HDaudio DMA and send required IPC to audio
	 * firmware.
	 */
	if (res->ops && res->ops->trigger) {
		ret = res->ops->trigger(substream, cmd, dai);
		if (ret < 0)
			return ret;
	}

	dai_runtime = cdns->dai_runtime_array[dai->id];
	if (!dai_runtime) {
		dev_err(dai->dev, "failed to get dai runtime in %s\n",
			__func__);
		return -EIO;
	}

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_SUSPEND:

		/*
		 * The .prepare callback is used to deal with xruns and resume operations.
		 * In the case of xruns, the DMAs and SHIM registers cannot be touched,
		 * but for resume operations the DMAs and SHIM registers need to be initialized.
		 * the .trigger callback is used to track the suspend case only.
		 */

		dai_runtime->suspended = true;

		break;

	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		dai_runtime->paused = true;
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		dai_runtime->paused = false;
		break;
	default:
		break;
	}

	return ret;
}

static const struct snd_soc_dai_ops intel_pcm_dai_ops = {
	.hw_params = intel_hw_params,
	.prepare = intel_prepare,
	.hw_free = intel_hw_free,
	.trigger = intel_trigger,
	.set_stream = intel_pcm_set_sdw_stream,
	.get_stream = intel_get_sdw_stream,
};

static const struct snd_soc_component_driver dai_component = {
	.name			= "soundwire",
};

/*
 * PDI routines
 */
static void intel_pdi_init(struct sdw_intel *sdw,
			   struct sdw_cdns_stream_config *config)
{
	void __iomem *shim = sdw->link_res->shim;
	int pcm_cap;

	/* PCM Stream Capability */
	pcm_cap = intel_readw(shim, SDW_SHIM2_PCMSCAP);

	config->pcm_bd = FIELD_GET(SDW_SHIM2_PCMSCAP_BSS, pcm_cap);
	config->pcm_in = FIELD_GET(SDW_SHIM2_PCMSCAP_ISS, pcm_cap);
	config->pcm_out = FIELD_GET(SDW_SHIM2_PCMSCAP_ISS, pcm_cap);

	dev_dbg(sdw->cdns.dev, "PCM cap bd:%d in:%d out:%d\n",
		config->pcm_bd, config->pcm_in, config->pcm_out);
}

static int
intel_pdi_get_ch_cap(struct sdw_intel *sdw, unsigned int pdi_num)
{
	void __iomem *shim = sdw->link_res->shim;

	/* zero based values for channel count in register */
	return intel_readw(shim, SDW_SHIM2_PCMSYCHC(pdi_num)) + 1;
}

static void intel_pdi_get_ch_update(struct sdw_intel *sdw,
				    struct sdw_cdns_pdi *pdi,
				    unsigned int num_pdi,
				    unsigned int *num_ch)
{
	int ch_count = 0;
	int i;

	for (i = 0; i < num_pdi; i++) {
		pdi->ch_count = intel_pdi_get_ch_cap(sdw, pdi->num);
		ch_count += pdi->ch_count;
		pdi++;
	}

	*num_ch = ch_count;
}

static void intel_pdi_stream_ch_update(struct sdw_intel *sdw,
				       struct sdw_cdns_streams *stream)
{
	intel_pdi_get_ch_update(sdw, stream->bd, stream->num_bd,
				&stream->num_ch_bd);

	intel_pdi_get_ch_update(sdw, stream->in, stream->num_in,
				&stream->num_ch_in);

	intel_pdi_get_ch_update(sdw, stream->out, stream->num_out,
				&stream->num_ch_out);
}

static int intel_create_dai(struct sdw_cdns *cdns,
			    struct snd_soc_dai_driver *dais,
			    enum intel_pdi_type type,
			    u32 num, u32 off, u32 max_ch)
{
	int i;

	if (!num)
		return 0;

	for (i = off; i < (off + num); i++) {
		dais[i].name = devm_kasprintf(cdns->dev, GFP_KERNEL,
					      "SDW%d Pin%d",
					      cdns->instance, i);
		if (!dais[i].name)
			return -ENOMEM;

		if (type == INTEL_PDI_BD || type == INTEL_PDI_OUT) {
			dais[i].playback.channels_min = 1;
			dais[i].playback.channels_max = max_ch;
		}

		if (type == INTEL_PDI_BD || type == INTEL_PDI_IN) {
			dais[i].capture.channels_min = 1;
			dais[i].capture.channels_max = max_ch;
		}

		dais[i].ops = &intel_pcm_dai_ops;
	}

	return 0;
}

static int intel_register_dai(struct sdw_intel *sdw)
{
	struct sdw_cdns_dai_runtime **dai_runtime_array;
	struct sdw_cdns_stream_config config;
	struct sdw_cdns *cdns = &sdw->cdns;
	struct sdw_cdns_streams *stream;
	struct snd_soc_dai_driver *dais;
	int num_dai;
	int ret;
	int off = 0;

	/* Read the PDI config and initialize cadence PDI */
	intel_pdi_init(sdw, &config);
	ret = sdw_cdns_pdi_init(cdns, config);
	if (ret)
		return ret;

	intel_pdi_stream_ch_update(sdw, &sdw->cdns.pcm);

	/* DAIs are created based on total number of PDIs supported */
	num_dai = cdns->pcm.num_pdi;

	dai_runtime_array = devm_kcalloc(cdns->dev, num_dai,
					 sizeof(struct sdw_cdns_dai_runtime *),
					 GFP_KERNEL);
	if (!dai_runtime_array)
		return -ENOMEM;
	cdns->dai_runtime_array = dai_runtime_array;

	dais = devm_kcalloc(cdns->dev, num_dai, sizeof(*dais), GFP_KERNEL);
	if (!dais)
		return -ENOMEM;

	/* Create PCM DAIs */
	stream = &cdns->pcm;

	ret = intel_create_dai(cdns, dais, INTEL_PDI_IN, cdns->pcm.num_in,
			       off, stream->num_ch_in);
	if (ret)
		return ret;

	off += cdns->pcm.num_in;
	ret = intel_create_dai(cdns, dais, INTEL_PDI_OUT, cdns->pcm.num_out,
			       off, stream->num_ch_out);
	if (ret)
		return ret;

	off += cdns->pcm.num_out;
	ret = intel_create_dai(cdns, dais, INTEL_PDI_BD, cdns->pcm.num_bd,
			       off, stream->num_ch_bd);
	if (ret)
		return ret;

	return devm_snd_soc_register_component(cdns->dev, &dai_component,
					       dais, num_dai);
}

static void intel_program_sdi(struct sdw_intel *sdw, int dev_num)
{
	int ret;

	ret = hdac_bus_eml_sdw_set_lsdiid(sdw->link_res->hbus, sdw->instance, dev_num);
	if (ret < 0)
		dev_err(sdw->cdns.dev, "%s: could not set lsdiid for link %d %d\n",
			__func__, sdw->instance, dev_num);
}

static int intel_get_link_count(struct sdw_intel *sdw)
{
	int ret;

	ret = hdac_bus_eml_get_count(sdw->link_res->hbus, true, AZX_REG_ML_LEPTR_ID_SDW);
	if (!ret) {
		dev_err(sdw->cdns.dev, "%s: could not retrieve link count\n", __func__);
		return -ENODEV;
	}

	if (ret > SDW_INTEL_MAX_LINKS) {
		dev_err(sdw->cdns.dev, "%s: link count %d exceed max %d\n", __func__, ret, SDW_INTEL_MAX_LINKS);
		return -EINVAL;
	}

	return ret;
}

const struct sdw_intel_hw_ops sdw_intel_lnl_hw_ops = {
	.debugfs_init = intel_ace2x_debugfs_init,
	.debugfs_exit = intel_ace2x_debugfs_exit,

	.get_link_count = intel_get_link_count,

	.register_dai = intel_register_dai,

	.check_clock_stop = intel_check_clock_stop,
	.start_bus = intel_start_bus,
	.start_bus_after_reset = intel_start_bus_after_reset,
	.start_bus_after_clock_stop = intel_start_bus_after_clock_stop,
	.stop_bus = intel_stop_bus,

	.link_power_up = intel_link_power_up,
	.link_power_down = intel_link_power_down,

	.shim_check_wake = intel_shim_check_wake,
	.shim_wake = intel_shim_wake,

	.pre_bank_switch = intel_pre_bank_switch,
	.post_bank_switch = intel_post_bank_switch,

	.sync_arm = intel_sync_arm,
	.sync_go_unlocked = intel_sync_go_unlocked,
	.sync_go = intel_sync_go,
	.sync_check_cmdsync_unlocked = intel_check_cmdsync_unlocked,

	.program_sdi = intel_program_sdi,

	.bpt_send_async = intel_ace2x_bpt_send_async,
	.bpt_wait = intel_ace2x_bpt_wait,
};
EXPORT_SYMBOL_NS(sdw_intel_lnl_hw_ops, "SOUNDWIRE_INTEL");

MODULE_IMPORT_NS("SND_SOC_SOF_HDA_MLINK");
MODULE_IMPORT_NS("SND_SOC_SOF_INTEL_HDA_SDW_BPT");

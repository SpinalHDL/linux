/*
 * spinal_lib_audio_out.c
 *
 *  Created on: Sep 11, 2020
 *      Author: rawrr
 */

#include <linux/io.h>
#include <linux/of_platform.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/init.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/dmaengine_pcm.h>
#include <linux/clk.h>

#define AUDIO_OUT_STATUS 0x10
#define AUDIO_OUT_RATE 0x14

struct spinal_lib_audio_out_device {
	void __iomem *regs;
	struct device *dev;
	struct snd_card *card;
	struct snd_pcm *pcm;
	struct dma_chan *dma;
	int position;
	u32 hz;
};

static const struct snd_pcm_hardware sdma_pcm_hardware = {
	.info = SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID |
		SNDRV_PCM_INFO_NO_PERIOD_WAKEUP | SNDRV_PCM_INFO_INTERLEAVED,

	.formats = (SNDRV_PCM_FMTBIT_S16_LE),
	.rates = (SNDRV_PCM_RATE_CONTINUOUS | SNDRV_PCM_RATE_8000_192000),
	.rate_min = 5500,
	.rate_max = 192000,
	.channels_min = 1,
	.channels_max = 2,
	.period_bytes_min = 1 * 1024,
	.period_bytes_max = 64 * 1024,
	.buffer_bytes_max = 128 * 1024,
	.periods_min = 2,
	.periods_max = 255,
	.fifo_size = 0,
};

static int spinal_lib_audio_out_open(struct snd_pcm_substream *substream)
{
	struct spinal_lib_audio_out_device *priv =
		snd_pcm_substream_chip(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;

	//printk("spinal_lib_audio_out_open\n");

	runtime->hw = sdma_pcm_hardware;
	runtime->private_data = priv;
	substream->private_data = priv;

	return 0;
}

static int spinal_lib_audio_out_close(struct snd_pcm_substream *substream)
{
	struct spinal_lib_audio_out_device *priv =
		snd_pcm_substream_chip(substream);
	//printk("spinal_lib_audio_out_close\n");
	dmaengine_synchronize(priv->dma);
	return 0;
}

static int spinal_lib_audio_out_hw_params(struct snd_pcm_substream *substream,
					  struct snd_pcm_hw_params *hw_params)
{
	//printk("spinal_lib_audio_out_hw_params\n");

	return snd_pcm_lib_malloc_pages(substream,
					params_buffer_bytes(hw_params));
}

static int spinal_lib_audio_out_hw_free(struct snd_pcm_substream *substream)
{
	//printk("spinal_lib_audio_out_hw_free\n");
	return snd_pcm_lib_free_pages(substream);
}

static void spinal_lib_audio_out_complete(void *arg)
{
	struct snd_pcm_substream *substream = arg;
	struct spinal_lib_audio_out_device *priv =
		snd_pcm_substream_chip(substream);
	//printk("spinal_lib_audio_out_complete\n");

	priv->position += snd_pcm_lib_period_bytes(substream);
	if (priv->position >= snd_pcm_lib_buffer_bytes(substream))
		priv->position = 0;

	snd_pcm_period_elapsed(substream);
}

static int spinal_lib_audio_out_trigger(struct snd_pcm_substream *substream,
					int cmd)
{
	struct spinal_lib_audio_out_device *priv =
		snd_pcm_substream_chip(substream);
	//printk("spinal_lib_audio_out_trigger\n");
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START: {
		struct dma_async_tx_descriptor *desc;
		unsigned long flags = DMA_CTRL_ACK;
		dma_cookie_t cookie;

		dev_info(priv->dev, "Start cha=%d rate=%d buf=%d per=%d\n",
			 (u32)substream->runtime->channels,
			 (u32)substream->runtime->rate,
			 (u32)substream->runtime->buffer_size,
			 (u32)substream->runtime->period_size);
		writel(priv->hz / substream->runtime->rate,
		       priv->regs + AUDIO_OUT_RATE);
		writel(substream->runtime->channels,
		       priv->regs + AUDIO_OUT_STATUS);

		priv->position = 0;

		desc = dmaengine_prep_dma_cyclic(
			priv->dma, substream->runtime->dma_addr,
			snd_pcm_lib_buffer_bytes(substream),
			snd_pcm_lib_period_bytes(substream), DMA_MEM_TO_DEV,
			flags);

		if (!desc)
			return -ENOMEM;

		desc->callback = spinal_lib_audio_out_complete;
		desc->callback_param = substream;
		cookie = desc->tx_submit(desc);
		if (dma_submit_error(cookie))
			return cookie;

		dma_async_issue_pending(priv->dma);

		return 0;
	}
	case SNDRV_PCM_TRIGGER_STOP:
		dmaengine_terminate_async(priv->dma);
		writel(0, priv->regs + AUDIO_OUT_STATUS);
		return 0;

	case SNDRV_PCM_TRIGGER_RESUME:
		return -EINVAL;
	case SNDRV_PCM_TRIGGER_SUSPEND:
		return -EINVAL;
	}
	return -EINVAL;
}

static int spinal_lib_audio_out_prepare(struct snd_pcm_substream *substream)
{
	struct spinal_lib_audio_out_device *priv =
		snd_pcm_substream_chip(substream);
	//printk("spinal_lib_audio_out_prepare\n");

	priv->position = 0;

	if (IS_ERR(priv->dma)) {
		return -EBUSY;
	}

	return 0;
}

static snd_pcm_uframes_t
spinal_lib_audio_out_pointer(struct snd_pcm_substream *substream)
{
	struct spinal_lib_audio_out_device *priv =
		snd_pcm_substream_chip(substream);
	//printk("spinal_lib_audio_out_pointer %d\n", priv->position);

	return bytes_to_frames(substream->runtime, priv->position);
}

static struct snd_pcm_ops spinal_lib_audio_out_ops = {
	.open = spinal_lib_audio_out_open,
	.close = spinal_lib_audio_out_close,
	.ioctl = snd_pcm_lib_ioctl,
	.hw_params = spinal_lib_audio_out_hw_params,
	.hw_free = spinal_lib_audio_out_hw_free,
	.prepare = spinal_lib_audio_out_prepare,
	.trigger = spinal_lib_audio_out_trigger,
	.pointer = spinal_lib_audio_out_pointer,
};

static int
spinal_lib_audio_out_register_pcm(struct spinal_lib_audio_out_device *priv)
{
	struct snd_pcm_ops *ops;
	int err;
	//printk("spinal_lib_audio_out_register_pcm\n");
	err = snd_pcm_new(priv->card, "Spinal lib audio", 0, 1, 0, &priv->pcm);
	if (err < 0)
		return err;
	ops = &spinal_lib_audio_out_ops;
	snd_pcm_set_ops(priv->pcm, SNDRV_PCM_STREAM_PLAYBACK, ops);
	snd_pcm_set_ops(priv->pcm, SNDRV_PCM_STREAM_CAPTURE, ops);
	priv->pcm->private_data = priv;
	priv->pcm->info_flags = 0;
	strcpy(priv->pcm->name, "Spinal lib audio");
	snd_pcm_lib_preallocate_pages_for_all(priv->pcm, SNDRV_DMA_TYPE_DEV,
					      priv->dev, 64 * 1024, 64 * 1024);
	return 0;
}
static int spinal_lib_audio_out_probe(struct platform_device *pdev)
{
	struct spinal_lib_audio_out_device *priv;
	struct snd_card *card;
	struct clk *clk;
	int err;

	//printk("spinal_lib_audio_out_probe\n");

	err = snd_card_new(&pdev->dev, 0, "Spinal lib audio out", THIS_MODULE,
			   sizeof(struct spinal_lib_audio_out_device), &card);
	if (err < 0)
		return err;

	priv = card->private_data;
	priv->card = card;
	priv->dev = &pdev->dev;

	priv->dma = dma_request_chan(priv->dev, "tx");

	spinal_lib_audio_out_register_pcm(priv);

	clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(clk)) {
		dev_info(&pdev->dev, "No peripheral clock\n");
		err = -1;
		goto error;
	}
	priv->hz = clk_get_rate(clk);
	if (!priv->hz) {
		dev_info(&pdev->dev, "Bad frequancy\n");
		err = -1;
		goto error;
	}

	/* Request and map I/O memory */
	priv->regs = devm_ioremap_resource(
		&pdev->dev, platform_get_resource(pdev, IORESOURCE_MEM, 0));
	if (IS_ERR(priv->regs))
		return PTR_ERR(priv->regs);

	err = snd_card_register(card);
	if (err) {
		goto error;
	}

	platform_set_drvdata(pdev, card);

	dev_info(&pdev->dev, "Probe success\n");
	return 0;

error:
	dev_info(&pdev->dev, "Probe failure :(\n");

	return err;
}

static int spinal_lib_audio_out_remove(struct platform_device *pdev)
{
	//printk("spinal_lib_audio_out_remove\n");

	return 0;
}

static const struct of_device_id spinal_lib_audio_out_of_ids[] = {
	{ .compatible = "spinal,lib-audio-out" },
	{ .compatible = "spinalhdl,audio_out-1.0" },
	{}
};
MODULE_DEVICE_TABLE(of, spinal_lib_audio_out_of_ids);

static struct platform_driver spinal_lib_vdma_driver = {
    .driver = {
        .name = "spinal,lib-audio-out",
        .of_match_table = spinal_lib_audio_out_of_ids,
    },
    .probe = spinal_lib_audio_out_probe,
    .remove = spinal_lib_audio_out_remove,
};

module_platform_driver(spinal_lib_vdma_driver);

MODULE_AUTHOR("Spinal");
MODULE_DESCRIPTION("SpinalHDL audio_out driver");
MODULE_LICENSE("GPL v2");

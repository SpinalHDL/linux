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

#define AUDIO_OUT_STATUS_RUN 1
#define AUDIO_OUT_STATUS 0x10
#define AUDIO_OUT_RATE 0x14

//devm_snd_dmaengine_pcm_register

struct spinal_lib_audio_out_device {
    void __iomem *regs;
    struct device *dev;
    struct snd_card *card;
    struct snd_pcm *pcm;
    struct dma_chan* dma;
    int pointer;
    u32 hz;
};

static const struct snd_pcm_hardware sdma_pcm_hardware = {
    .info           = SNDRV_PCM_INFO_MMAP |
                  SNDRV_PCM_INFO_MMAP_VALID |
                  SNDRV_PCM_INFO_PAUSE | SNDRV_PCM_INFO_RESUME |
                  SNDRV_PCM_INFO_NO_PERIOD_WAKEUP |
                  SNDRV_PCM_INFO_INTERLEAVED,

    .formats =      (SNDRV_PCM_FMTBIT_S16_LE),
    .rates =        (SNDRV_PCM_RATE_CONTINUOUS | SNDRV_PCM_RATE_8000_48000),
    .rate_min =     5500,
    .rate_max =     48000,
    .channels_min =     1,
    .channels_max =     2,
    .period_bytes_min   = 1 * 1024,
    .period_bytes_max   = 64 * 1024,
    .buffer_bytes_max   = 128 * 1024,
//    .period_bytes_min   = 64*1024,
//    .period_bytes_max   = 256*1024,
//    .buffer_bytes_max   = 1024 * 1024,
    .periods_min        = 2,
    .periods_max        = 255,
    .fifo_size =        0,
};



static int dummy_pcm_open(struct snd_pcm_substream *substream)
{
    struct spinal_lib_audio_out_device *priv = snd_pcm_substream_chip(substream);
//    struct dummy_model *model = dummy->model;
    struct snd_pcm_runtime *runtime = substream->runtime;
//    const struct dummy_timer_ops *ops;

    //printk("dummy_pcm_open\n");

//    ops = &dummy_systimer_ops;
//#ifdef CONFIG_HIGH_RES_TIMERS
//    if (hrtimer)
//        ops = &dummy_hrtimer_ops;
//#endif
//
//    err = ops->create(substream);
//    if (err < 0)
//        return err;
//    get_dummy_ops(substream) = ops;

    runtime->hw = sdma_pcm_hardware;
    runtime->private_data = priv;
    substream->private_data = priv;

//    if (substream->pcm->device & 1) {
//        runtime->hw.info &= ~SNDRV_PCM_INFO_INTERLEAVED;
//        runtime->hw.info |= SNDRV_PCM_INFO_NONINTERLEAVED;
//    }
//    if (substream->pcm->device & 2)
//        runtime->hw.info &= ~(SNDRV_PCM_INFO_MMAP |
//                      SNDRV_PCM_INFO_MMAP_VALID);


//    if (err < 0) {
//        get_dummy_ops(substream)->free(substream);
//        return err;
//    }
    return 0;
}

static int dummy_pcm_close(struct snd_pcm_substream *substream)
{
    struct spinal_lib_audio_out_device *priv = snd_pcm_substream_chip(substream);
    //printk("dummy_pcm_close\n");
    dmaengine_synchronize(priv->dma);
    return 0;
}

static int dummy_pcm_hw_params(struct snd_pcm_substream *substream,
                   struct snd_pcm_hw_params *hw_params)
{
    int ret;
    //printk("dummy_pcm_hw_params\n");
//    if (fake_buffer) {
//        /* runtime->dma_bytes has to be set manually to allow mmap */
//        substream->runtime->dma_bytes = params_buffer_bytes(hw_params);
//        return 0;
//    }
    ret =  snd_pcm_lib_malloc_pages(substream, params_buffer_bytes(hw_params));


    //printk("dummy_pcm_hw_params done %d\n", ret);
    return ret;
}

static int dummy_pcm_hw_free(struct snd_pcm_substream *substream)
{
    //printk("dummy_pcm_hw_free\n");
//    if (fake_buffer)
//        return 0;
    return snd_pcm_lib_free_pages(substream);
}

static void spinal_lib_audio_out_complete(void *arg)
{
    struct snd_pcm_substream *substream = arg;
    struct spinal_lib_audio_out_device *priv = snd_pcm_substream_chip(substream);
    //printk("spinal_lib_audio_out_complete\n");

    priv->pointer += snd_pcm_lib_period_bytes(substream);
    if (priv->pointer >= snd_pcm_lib_buffer_bytes(substream))
        priv->pointer = 0;

    snd_pcm_period_elapsed(substream);
}


static int dummy_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
    struct spinal_lib_audio_out_device *priv = snd_pcm_substream_chip(substream);
    //printk("dummy_pcm_trigger\n");
    switch (cmd) {
    case SNDRV_PCM_TRIGGER_START:{
        struct dma_async_tx_descriptor *desc;
        unsigned long flags = DMA_CTRL_ACK;
        dma_cookie_t cookie;

        //printk("dummy_pcm_trigger START %x\n", (u32)priv->regs);
        printk("dummy_pcm_trigger START %d %d %d\n", priv->hz, substream->runtime->rate, priv->hz/substream->runtime->rate);
        writel(priv->hz/substream->runtime->rate, priv->regs + AUDIO_OUT_RATE);
        writel(AUDIO_OUT_STATUS_RUN, priv->regs + AUDIO_OUT_STATUS);

//        if (!substream->runtime->no_period_wakeup)
//            flags |= DMA_PREP_INTERRUPT;

        priv->pointer = 0;

        //printk("dummy_pcm_trigger A %llx %x %x\n", substream->runtime->dma_addr, snd_pcm_lib_buffer_bytes(substream), snd_pcm_lib_period_bytes(substream));
        desc = dmaengine_prep_dma_cyclic(priv->dma,
            substream->runtime->dma_addr,
            snd_pcm_lib_buffer_bytes(substream),
            snd_pcm_lib_period_bytes(substream), DMA_MEM_TO_DEV, flags);

        //printk("dummy_pcm_trigger B\n");
        if (!desc)
            return -ENOMEM;

        desc->callback = spinal_lib_audio_out_complete;
        desc->callback_param = substream;
        cookie = desc->tx_submit(desc);
        if(dma_submit_error(cookie))
            return cookie;

        //printk("dummy_pcm_trigger C\n");
        dma_async_issue_pending(priv->dma);

        //printk("dummy_pcm_trigger D\n");
        return 0;
    }
    case SNDRV_PCM_TRIGGER_STOP:
        //printk("dummy_pcm_trigger STOP\n");
        dmaengine_terminate_async(priv->dma);
        writel(0, priv->regs + AUDIO_OUT_STATUS);
        return 0;

    case SNDRV_PCM_TRIGGER_RESUME:
        //printk("dummy_pcm_trigger RESUME\n");
        return -EINVAL;
    case SNDRV_PCM_TRIGGER_SUSPEND:
        //printk("dummy_pcm_trigger SUSSPEND\n");
        return -EINVAL;
    }
    //printk("dummy_pcm_trigger ??? %d\n", cmd);
    return -EINVAL;
}

static int dummy_pcm_prepare(struct snd_pcm_substream *substream)
{
    struct spinal_lib_audio_out_device *priv = snd_pcm_substream_chip(substream);
    //printk("dummy_pcm_prepare\n");

    priv->pointer = 0;

    if (IS_ERR(priv->dma)) {
        return -EBUSY;
    }
    //printk("dummy_pcm_prepare success :D\n");
    return 0;
}

static snd_pcm_uframes_t dummy_pcm_pointer(struct snd_pcm_substream *substream)
{
    struct spinal_lib_audio_out_device *priv = snd_pcm_substream_chip(substream);
    //printk("dummy_pcm_pointer %d\n", priv->pointer);
//    priv->pointer += substream->runtime->period_size;
//    priv->pointer %= substream->runtime->buffer_size;

    return bytes_to_frames(substream->runtime, priv->pointer);
//    return get_dummy_ops(substream)->pointer(substream);
}

static struct snd_pcm_ops dummy_pcm_ops = {
    .open =     dummy_pcm_open,
    .close =    dummy_pcm_close,
    .ioctl =    snd_pcm_lib_ioctl,
    .hw_params =    dummy_pcm_hw_params,
    .hw_free =  dummy_pcm_hw_free,
    .prepare =  dummy_pcm_prepare,
    .trigger =  dummy_pcm_trigger,
    .pointer =  dummy_pcm_pointer,
};

static int spinal_lib_audio_out_register_pcm(struct spinal_lib_audio_out_device *priv){
    struct snd_pcm_ops *ops;
    int err;
    //printk("spinal_lib_audio_out_register_pcm\n");
    err = snd_pcm_new(priv->card, "r PCM", 0,
                   1, 0, &priv->pcm);
    if (err < 0)
        return err;
    ops = &dummy_pcm_ops;
    snd_pcm_set_ops(priv->pcm, SNDRV_PCM_STREAM_PLAYBACK, ops);
    snd_pcm_set_ops(priv->pcm, SNDRV_PCM_STREAM_CAPTURE, ops);
    priv->pcm->private_data = priv;
    priv->pcm->info_flags = 0;
    strcpy(priv->pcm->name, "rawrr PCM");
    snd_pcm_lib_preallocate_pages_for_all(priv->pcm,
            SNDRV_DMA_TYPE_DEV,
        priv->dev,
        64*1024, 64*1024);
    //printk("spinal_lib_audio_out_register_pcm done\n");
    return 0;
}
static int spinal_lib_audio_out_probe(struct platform_device *pdev)
{
    struct spinal_lib_audio_out_device *priv;
    struct snd_card *card;
    struct clk      *clk;
    int err;

    //printk("spinal_lib_audio_out_probe\n");

    err = snd_card_new(&pdev->dev, 0, "miaou", THIS_MODULE,
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
    if(!priv->hz){
        dev_info(&pdev->dev, "Bad frequancy\n");
        err = -1;
        goto error;
    }

    /* Request and map I/O memory */
    priv->regs = devm_ioremap_resource(&pdev->dev, platform_get_resource(pdev, IORESOURCE_MEM, 0));
    if (IS_ERR(priv->regs))
        return PTR_ERR(priv->regs);



    err = snd_card_register(card);
    if (err) {
        goto error;
    }


    platform_set_drvdata(pdev, card);


/*
    err = devm_snd_soc_register_component(&pdev->dev, &stm32_i2s_component,
            &jz4780_i2s_dai, 1);
    if (err)
        goto error;*/

    //snd_dmaengine_pcm_register devm_snd_dmaengine_pcm_register
  /*  err = devm_snd_dmaengine_pcm_register(&pdev->dev,
                    &sdma_dmaengine_pcm_config,
                    flags);
    if(err){
        goto error;
    }*/

    dev_info(&pdev->dev, "SpinalHDL lib audio_out Driver Probed %x!!\n", (u32)priv->regs);
    return 0;

error:
    dev_info(&pdev->dev, "SpinalHDL lib audio_out Driver errored :(\n");


    return err;
}


static int spinal_lib_audio_out_remove(struct platform_device *pdev)
{
    //printk("spinal_lib_audio_out_remove\n");

    return 0;
}


static const struct of_device_id spinal_lib_audio_out_of_ids[] = {
    { .compatible = "spinal,lib-audio-out"},
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

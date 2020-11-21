/*
 * spinal_lib_mac.c --  the Simple Network Utility
 *
 * Copyright (C) 2001 Alessandro Rubini and Jonathan Corbet
 * Copyright (C) 2001 O'Reilly & Associates
 *
 * The source code in this file can be freely used, adapted,
 * and redistributed in source or binary form, so long as an
 * acknowledgment appears in derived source files.  The citation
 * should list that the code comes from the book "Linux Device
 * Drivers" by Alessandro Rubini and Jonathan Corbet, published
 * by O'Reilly & Associates.   No warranty is attached;
 * we cannot take responsibility for errors or fitness for use.
 *
 * $Id: spinal_lib_mac.c,v 1.21 2004/11/05 02:36:03 rubini Exp $
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/moduleparam.h>

#include <linux/sched.h>
#include <linux/kernel.h> /* printk() */
#include <linux/slab.h> /* kmalloc() */
#include <linux/errno.h>  /* error codes */
#include <linux/types.h>  /* size_t */
#include <linux/interrupt.h> /* mark_bh */

#include <linux/in.h>
#include <linux/netdevice.h>   /* struct device, and other headers */
#include <linux/etherdevice.h> /* eth_type_trans */
#include <linux/ip.h>          /* struct iphdr */
#include <linux/tcp.h>         /* struct tcphdr */
#include <linux/skbuff.h>
#include <linux/platform_device.h>

#include <linux/in6.h>
#include <asm/checksum.h>
#include <linux/phy.h>

#include <linux/of.h>
#include <linux/of_net.h>
#include <linux/of_address.h>


#define DRV_NAME    "spinal_lib_mac"
#define DRV_VERSION "0.1"


#define SPINAL_LIB_MAC_PULLING_PERIOD_MS 10
#define SPINAL_LIB_MAC_FRAME_SIZE_MAX 2000

#define SPINAL_LIB_MAC_CTRL 0x00
#define SPINAL_LIB_MAC_TX   0x10
#define SPINAL_LIB_MAC_TX_AVAILABILITY   0x14
#define SPINAL_LIB_MAC_RX   0x20
#define SPINAL_LIB_MAC_RX_STATS   0x2C

#define SPINAL_LIB_MAC_CTRL_TX_RESET 0x00000001
#define SPINAL_LIB_MAC_CTRL_TX_READY 0x00000002
#define SPINAL_LIB_MAC_CTRL_TX_ALIGN 0x00000004

#define SPINAL_LIB_MAC_CTRL_RX_RESET   0x00000010
#define SPINAL_LIB_MAC_CTRL_RX_PENDING 0x00000020
#define SPINAL_LIB_MAC_CTRL_RX_ALIGN 0x00000040




struct spinal_lib_mac_priv {
    void __iomem *base;
    struct net_device_stats stats;
    spinlock_t lock;
    struct net_device *ndev;
    struct device *dev;

    int use_polling;
    struct timer_list poll_timer;
};


static u32 spinal_lib_mac_rx_stats(void __iomem *base){
    return readl_relaxed(base + SPINAL_LIB_MAC_RX_STATS);
}

static u32 spinal_lib_mac_tx_availability(void __iomem *base){
    return readl_relaxed(base + SPINAL_LIB_MAC_TX_AVAILABILITY);
}

static u32 spinal_lib_mac_tx_ready(void __iomem *base){
    return readl_relaxed(base + SPINAL_LIB_MAC_CTRL) & SPINAL_LIB_MAC_CTRL_TX_READY;
}

static u32 spinal_lib_mac_rx_pending(void __iomem *base){
    return readl_relaxed(base + SPINAL_LIB_MAC_CTRL) & SPINAL_LIB_MAC_CTRL_RX_PENDING;
}

static u32 spinal_lib_mac_rx_u32(void __iomem *base){
    return readl_relaxed(base + SPINAL_LIB_MAC_RX);
}

static void spinal_lib_mac_tx_u32(void __iomem *base, u32 data){
    writel_relaxed(data, base + SPINAL_LIB_MAC_TX);
}

static void spinal_lib_mac_reset_set(void __iomem *base){
    writel_relaxed(SPINAL_LIB_MAC_CTRL_TX_RESET | SPINAL_LIB_MAC_CTRL_RX_RESET | SPINAL_LIB_MAC_CTRL_TX_ALIGN | SPINAL_LIB_MAC_CTRL_RX_ALIGN, base + SPINAL_LIB_MAC_CTRL);
}

static void spinal_lib_mac_reset_clear(void __iomem *base){
    writel_relaxed(SPINAL_LIB_MAC_CTRL_TX_ALIGN | SPINAL_LIB_MAC_CTRL_RX_ALIGN, base + SPINAL_LIB_MAC_CTRL);
}




//TODO spin_lock_irqsave

static int spinal_lib_mac_rx(struct net_device *ndev)
{
    struct sk_buff *skb;
    struct spinal_lib_mac_priv *priv = netdev_priv(ndev);
    void __iomem *base = priv->base;

    u32 bits = spinal_lib_mac_rx_u32(base);
    u32 len = (bits-16)/8;
    u32 word_count = (bits+31)/32;
    u32 *ptr;


    if (len >= 2048)
        printk("spinal_lib_mac_rx %d\n", len);


    /*
     * The packet has been retrieved from the transmission
     * medium. Build an skb around it, so upper layers can handle it
     */
    skb = dev_alloc_skb(word_count*4);
    if (!skb) {
        if (printk_ratelimit())
            printk(KERN_NOTICE "spinal_lib_mac rx: low on mem - packet dropped\n");

        while(word_count--){
            spinal_lib_mac_rx_u32(base);
        }
        return NET_RX_DROP;
    }
    skb_reserve(skb, 2); /* align IP on 16B boundary */
    ptr = (u32*)(skb_put(skb, len)-2);
    while(word_count >= 4){
        ptr[0] = spinal_lib_mac_rx_u32(base);
        ptr[1] = spinal_lib_mac_rx_u32(base);
        ptr[2] = spinal_lib_mac_rx_u32(base);
        ptr[3] = spinal_lib_mac_rx_u32(base);
        ptr += 4;
        word_count -= 4;
    }
    while(word_count--){
        *ptr++ = spinal_lib_mac_rx_u32(base);
    }

    /* Write metadata, and then pass to the receive level */
    skb->dev = ndev;
    skb->protocol = eth_type_trans(skb, ndev);
    skb->ip_summed = CHECKSUM_UNNECESSARY; /* don't check it */
    priv->stats.rx_packets++;
    priv->stats.rx_bytes += len;

    return netif_rx(skb);
}

static irqreturn_t spinal_lib_mac_interrupt(int irq, void *dev_id)
{
    struct net_device *ndev = dev_id;
    struct spinal_lib_mac_priv *priv = netdev_priv(ndev);

    u32 rx_stats = spinal_lib_mac_rx_stats(priv->base);
    priv->stats.rx_errors  += (rx_stats >> 0) & 0xFF;
    priv->stats.rx_dropped += (rx_stats >> 8) & 0xFF;

    while (spinal_lib_mac_rx_pending(priv->base)) {
        switch(spinal_lib_mac_rx(ndev)){
        case NET_RX_DROP : priv->stats.rx_dropped++; break;
        }
    }

    return IRQ_HANDLED;
}


static void spinal_lib_mac_timeout(struct timer_list *t)
{
    struct spinal_lib_mac_priv *priv = from_timer(priv, t, poll_timer);

    spinal_lib_mac_interrupt(0, priv->ndev);
    mod_timer(&priv->poll_timer, jiffies + msecs_to_jiffies(SPINAL_LIB_MAC_PULLING_PERIOD_MS));
}


/*
 * Open and close
 */

int spinal_lib_mac_open(struct net_device *ndev)
{
    struct spinal_lib_mac_priv *priv = netdev_priv(ndev);
    int err;

    netif_carrier_on(ndev);


    netif_start_queue(ndev);

    spinal_lib_mac_reset_set(priv->base);
    udelay(10);
    spinal_lib_mac_reset_clear(priv->base);

    if (priv->use_polling) {
        timer_setup(&priv->poll_timer, spinal_lib_mac_timeout, 0);
        mod_timer(&priv->poll_timer, jiffies + msecs_to_jiffies(SPINAL_LIB_MAC_PULLING_PERIOD_MS));
    } else {
        err = request_irq(ndev->irq, spinal_lib_mac_interrupt, 0, ndev->name, ndev);
        if (err) {
            netdev_err(ndev, "failed to request irq %d\n", ndev->irq);
            goto err_irq;
        }
    }

    return 0;
err_irq:
    netif_carrier_off(ndev);
    return err;
}

int spinal_lib_mac_stop(struct net_device *ndev)
{
    struct spinal_lib_mac_priv *priv = netdev_priv(ndev);
    del_timer_sync(&priv->poll_timer);
    if (!priv->use_polling) {
        free_irq(ndev->irq, ndev);
    }
    netif_stop_queue(ndev); /* can't transmit any more */
    return 0;
}

/*
 * Configuration changes (passed on by ifconfig)
 */
int spinal_lib_mac_config(struct net_device *dev, struct ifmap *map)
{
    if (dev->flags & IFF_UP) /* can't act on a running interface */
        return -EBUSY;

    /* Don't allow changing the I/O address */
    if (map->base_addr != dev->base_addr) {
        printk(KERN_WARNING "spinal_lib_mac: Can't change I/O address\n");
        return -EOPNOTSUPP;
    }



    /* ignore other fields */
    return 0;
}


/*
 * Transmit a packet (called by the kernel)
 */
int spinal_lib_mac_tx(struct sk_buff *skb, struct net_device *ndev)
{
    struct spinal_lib_mac_priv *priv = netdev_priv(ndev);
    u32 bits = skb->len*8+16;
    u32 word_count = (bits+31)/32;
    u32 *ptr;
    void __iomem *base = priv->base;

    /* Reject oversize packets */
    if (unlikely(skb->len > SPINAL_LIB_MAC_FRAME_SIZE_MAX)) {
        if (net_ratelimit())
            netdev_dbg(ndev, "tx packet too big\n");
        goto drop;
    }

    while(!spinal_lib_mac_tx_ready(base));
    spinal_lib_mac_tx_u32(base, bits);
    ptr = (u32*)(skb->data-2);

    while(word_count){
        u32 tockens = spinal_lib_mac_tx_availability(base);
        if(tockens > word_count) tockens = word_count;
        word_count -= tockens;
        while(tockens >= 4){
            spinal_lib_mac_tx_u32(base, ptr[0]);
            spinal_lib_mac_tx_u32(base, ptr[1]);
            spinal_lib_mac_tx_u32(base, ptr[2]);
            spinal_lib_mac_tx_u32(base, ptr[3]);
            ptr += 4;
            tockens -=4;
        }
        while(tockens--){
            spinal_lib_mac_tx_u32(base, *ptr++);
        }
    }

    priv->stats.tx_packets++;
    priv->stats.tx_bytes += skb->len;
    dev_kfree_skb_any(skb);

    return NETDEV_TX_OK;
drop:
    dev_kfree_skb_any(skb);
    ndev->stats.tx_dropped++;
    return NETDEV_TX_OK;
}


/*
 * Return statistics to the caller
 */
struct net_device_stats *spinal_lib_mac_stats(struct net_device *ndev)
{
    struct spinal_lib_mac_priv *priv = netdev_priv(ndev);
    return &priv->stats;
}

/*
 * This function is called to fill up an eth header, since arp is not
 * available on the interface
 */
int spinal_lib_mac_rebuild_header(struct sk_buff *skb)
{
    struct ethhdr *eth = (struct ethhdr *) skb->data;
    struct net_device *dev = skb->dev;

    memcpy(eth->h_source, dev->dev_addr, dev->addr_len);
    memcpy(eth->h_dest, dev->dev_addr, dev->addr_len);
    eth->h_dest[ETH_ALEN-1]   ^= 0x01;   /* dest is us xor 1 */
    return 0;
}





static void spinal_lib_mac_get_drvinfo(struct net_device *ndev,
                struct ethtool_drvinfo *info)
{
    strlcpy(info->driver, DRV_NAME, sizeof(info->driver));
    strlcpy(info->version, DRV_VERSION, sizeof(info->version));
    strlcpy(info->bus_info, dev_name(&ndev->dev), sizeof(info->bus_info));
}

static const struct net_device_ops spinal_lib_mac_netdev_ops = {
    .ndo_open            = spinal_lib_mac_open,
    .ndo_stop            = spinal_lib_mac_stop,
    .ndo_start_xmit      = spinal_lib_mac_tx,
    .ndo_set_config      = spinal_lib_mac_config,
    .ndo_get_stats       = spinal_lib_mac_stats,
};

static const struct ethtool_ops spinal_lib_mac_ethtool_ops = {
    .get_drvinfo        = spinal_lib_mac_get_drvinfo,
    .get_link           = ethtool_op_get_link,
    .get_link_ksettings = phy_ethtool_get_link_ksettings,
    .set_link_ksettings = phy_ethtool_set_link_ksettings,
    .nway_reset         = phy_ethtool_nway_reset,
};


/*
 * Finally, the module stuff
 */

static int spinal_lib_mac_remove(struct platform_device *pdev)
{
    struct net_device *dev;
    struct spinal_lib_mac *priv;

    dev = platform_get_drvdata(pdev);
    priv = netdev_priv(dev);

    unregister_netdev(dev);

    free_netdev(dev);

    return 0;
}


int spinal_lib_mac_probe(struct platform_device *pdev)
{
    struct device_node *np = pdev->dev.of_node;
    struct net_device *ndev;
    struct spinal_lib_mac_priv *priv;
    struct resource *res;
    const char *mac_addr;
    int irq, err;

    ndev = alloc_etherdev(sizeof(*priv));

    if (!ndev)
        return -ENOMEM;

    priv = netdev_priv(ndev);
    priv->ndev = ndev;
    priv->dev = &pdev->dev;

    priv->use_polling = 0;
    irq = platform_get_irq(pdev, 0);
    if (irq < 0) {
        dev_err(&pdev->dev, "Failed to get IRQ, using polling\n");
        priv->use_polling = 1;
        irq = 0;
    }

    res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
    priv->base = devm_ioremap_resource(&pdev->dev, res);
    if (IS_ERR(priv->base)) {
        err = PTR_ERR(priv->base);
        goto err;
    }


    mac_addr = of_get_mac_address(np);
    if (mac_addr && is_valid_ether_addr(mac_addr))
        memcpy(ndev->dev_addr, mac_addr, ETH_ALEN);
    else
        eth_hw_addr_random(ndev);

    SET_NETDEV_DEV(ndev, &pdev->dev);
    platform_set_drvdata(pdev, ndev);

    ndev->netdev_ops = &spinal_lib_mac_netdev_ops;
    ndev->ethtool_ops = &spinal_lib_mac_ethtool_ops;
    ndev->irq = irq;
//    ndev->features        |= NETIF_F_HW_CSUM; //TODO



    err = register_netdev(ndev);
    if (err) {
        dev_err(&pdev->dev, "Failed to register ndev\n");
        goto err;
    }

    netdev_info(ndev, "irq %d, mapped at %px\n", ndev->irq, priv->base);

    return 0;
err:
    free_netdev(ndev);
    return err;
}


static const struct of_device_id spinal_lib_mac_of_match[] = {
    {.compatible = "spinal,lib_mac"},
    {}
};

MODULE_DEVICE_TABLE(of, spinal_lib_mac_of_match);

static struct platform_driver spinal_lib_mac_driver = {
    .probe = spinal_lib_mac_probe,
    .remove = spinal_lib_mac_remove,
    .driver = {
        .name = DRV_NAME,
        .of_match_table = spinal_lib_mac_of_match,
    },
};
module_platform_driver(spinal_lib_mac_driver);

MODULE_AUTHOR("Charles Papon");
MODULE_LICENSE("Dual GPL");

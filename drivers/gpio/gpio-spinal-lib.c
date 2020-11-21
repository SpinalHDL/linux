





#include <linux/bitops.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/of_irq.h>
#include <linux/gpio/driver.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/init.h>
#include <linux/of.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/spinlock.h>

#define GPIO_INPUT		    0x0
#define GPIO_OUTPUT			0x4
#define GPIO_OUTPUT_ENABLE	0x8
#define GPIO_RISE_IE		0x20
#define GPIO_FALL_IE		0x24
#define GPIO_HIGH_IE		0x28
#define GPIO_LOW_IE			0x2C

#define MAX_GPIO	32

struct spinal_lib_gpio {
	raw_spinlock_t		lock;
	void __iomem		*base;
	struct gpio_chip	gc;
	unsigned long		enabled;
	unsigned		trigger[MAX_GPIO];
	unsigned int		irq_parent[MAX_GPIO];
	struct spinal_lib_gpio	*self_ptr[MAX_GPIO];

	u32 interrupt_mask;
};

static void spinal_lib_gpio_assign_bit(void __iomem *ptr, int offset, int value)
{
	// It's frustrating that we are not allowed to use the device atomics
	// which are GUARANTEED to be supported by this device on RISC-V
	u32 bit = BIT(offset), old = ioread32(ptr);
	if (value)
		iowrite32(old | bit, ptr);
	else
		iowrite32(old & ~bit, ptr);
}

static int spinal_lib_gpio_direction_input(struct gpio_chip *gc, unsigned offset)
{
	struct spinal_lib_gpio *chip = gpiochip_get_data(gc);
	unsigned long flags;

	if (offset >= gc->ngpio)
		return -EINVAL;

	raw_spin_lock_irqsave(&chip->lock, flags);
	spinal_lib_gpio_assign_bit(chip->base + GPIO_OUTPUT_ENABLE, offset, 0);
	raw_spin_unlock_irqrestore(&chip->lock, flags);


	return 0;
}

static int spinal_lib_gpio_direction_output(struct gpio_chip *gc, unsigned offset, int value)
{
	struct spinal_lib_gpio *chip = gpiochip_get_data(gc);
	unsigned long flags;

	if (offset >= gc->ngpio)
		return -EINVAL;

	raw_spin_lock_irqsave(&chip->lock, flags);
	spinal_lib_gpio_assign_bit(chip->base + GPIO_OUTPUT, offset, value);
	spinal_lib_gpio_assign_bit(chip->base + GPIO_OUTPUT_ENABLE,  offset, 1);
	raw_spin_unlock_irqrestore(&chip->lock, flags);

	return 0;
}

static int spinal_lib_gpio_get_direction(struct gpio_chip *gc, unsigned offset)
{
	struct spinal_lib_gpio *chip = gpiochip_get_data(gc);

	if (offset >= gc->ngpio)
		return -EINVAL;

	return !(ioread32(chip->base + GPIO_OUTPUT_ENABLE) & BIT(offset));
}

static int spinal_lib_gpio_get_value(struct gpio_chip *gc, unsigned offset)
{
	struct spinal_lib_gpio *chip = gpiochip_get_data(gc);

	if (offset >= gc->ngpio)
		return -EINVAL;

	return !!(ioread32(chip->base + GPIO_INPUT) & BIT(offset));
}

static void spinal_lib_gpio_set_value(struct gpio_chip *gc, unsigned offset, int value)
{
	struct spinal_lib_gpio *chip = gpiochip_get_data(gc);
	unsigned long flags;

	if (offset >= gc->ngpio)
		return;

	raw_spin_lock_irqsave(&chip->lock, flags);
	spinal_lib_gpio_assign_bit(chip->base + GPIO_OUTPUT, offset, value);
	raw_spin_unlock_irqrestore(&chip->lock, flags);
}

static void spinal_lib_gpio_set_ie(struct spinal_lib_gpio *chip, int offset)
{
	unsigned long flags;
	unsigned trigger;
	raw_spin_lock_irqsave(&chip->lock, flags);
	trigger = (chip->enabled & chip->interrupt_mask & BIT(offset)) ? chip->trigger[offset] : 0;
	spinal_lib_gpio_assign_bit(chip->base + GPIO_RISE_IE, offset, trigger & IRQ_TYPE_EDGE_RISING);
	spinal_lib_gpio_assign_bit(chip->base + GPIO_FALL_IE, offset, trigger & IRQ_TYPE_EDGE_FALLING);
	spinal_lib_gpio_assign_bit(chip->base + GPIO_HIGH_IE, offset, trigger & IRQ_TYPE_LEVEL_HIGH);
	spinal_lib_gpio_assign_bit(chip->base + GPIO_LOW_IE,  offset, trigger & IRQ_TYPE_LEVEL_LOW);
	raw_spin_unlock_irqrestore(&chip->lock, flags);
}

static int spinal_lib_gpio_irq_set_type(struct irq_data *d, unsigned trigger)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct spinal_lib_gpio *chip = gpiochip_get_data(gc);
	int offset = irqd_to_hwirq(d);

	if (offset < 0 || offset >= gc->ngpio)
		return -EINVAL;

	chip->trigger[offset] = trigger;
	spinal_lib_gpio_set_ie(chip, offset);
	return 0;
}

//static void spinal_lib_gpio_update_interrupt_enable(struct spinal_lib_gpio *chip){
//	iowrite32(chip->irq_rise_enable & chip->interrupt_mask, chip->base + GPIO_RISE_IE);
//	iowrite32(chip->irq_fall_enable & chip->interrupt_mask, chip->base + GPIO_FALL_IE);
//	iowrite32(chip->irq_high_enable & chip->interrupt_mask, chip->base + GPIO_HIGH_IE);
//	iowrite32(chip->irq_low_enable & chip->interrupt_mask, chip->base + GPIO_LOW_IE);
//}

/* chained_irq_{enter,exit} already mask the parent */
static void spinal_lib_gpio_irq_mask(struct irq_data *d) {
	unsigned long flags;
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct spinal_lib_gpio *chip = gpiochip_get_data(gc);
	int offset = irqd_to_hwirq(d) % MAX_GPIO; // must not fail

	raw_spin_lock_irqsave(&chip->lock, flags);
	chip->interrupt_mask &= ~BIT(offset);
	raw_spin_unlock_irqrestore(&chip->lock, flags);

	spinal_lib_gpio_set_ie(chip, offset);

}
static void spinal_lib_gpio_irq_unmask(struct irq_data *d) {
	unsigned long flags;
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct spinal_lib_gpio *chip = gpiochip_get_data(gc);
	int offset = irqd_to_hwirq(d) % MAX_GPIO; // must not fail

	raw_spin_lock_irqsave(&chip->lock, flags);
	chip->interrupt_mask |= BIT(offset);
	raw_spin_unlock_irqrestore(&chip->lock, flags);

	spinal_lib_gpio_set_ie(chip, offset);
}

static void spinal_lib_gpio_irq_enable(struct irq_data *d)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct spinal_lib_gpio *chip = gpiochip_get_data(gc);
	int offset = irqd_to_hwirq(d) % MAX_GPIO; // must not fail
//	u32 bit = BIT(offset);

	/* Switch to input */
	spinal_lib_gpio_direction_input(gc, offset);

	/* Clear any sticky pending interrupts */
//	iowrite32(bit, chip->base + GPIO_RISE_IP);
//	iowrite32(bit, chip->base + GPIO_FALL_IP);
//	iowrite32(bit, chip->base + GPIO_HIGH_IP);
//	iowrite32(bit, chip->base + GPIO_LOW_IP);

	/* Enable interrupts */
	assign_bit(offset, &chip->enabled, 1);
	spinal_lib_gpio_set_ie(chip, offset);
    spinal_lib_gpio_irq_unmask(d);
}

static void spinal_lib_gpio_irq_disable(struct irq_data *d)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct spinal_lib_gpio *chip = gpiochip_get_data(gc);
	int offset = irqd_to_hwirq(d) % MAX_GPIO; // must not fail

	assign_bit(offset, &chip->enabled, 0);
	spinal_lib_gpio_set_ie(chip, offset);
}

static struct irq_chip spinal_lib_gpio_irqchip = {
	.name		= "spinal_lib_gpio-gpio",
	.irq_set_type	= spinal_lib_gpio_irq_set_type,
	.irq_mask	= spinal_lib_gpio_irq_mask,
	.irq_unmask	= spinal_lib_gpio_irq_unmask,
	.irq_enable	= spinal_lib_gpio_irq_enable,
	.irq_disable	= spinal_lib_gpio_irq_disable,
};

static void spinal_lib_gpio_irq_handler(struct irq_desc *desc)
{
	struct irq_chip *irqchip = irq_desc_get_chip(desc);
	struct spinal_lib_gpio **self_ptr = irq_desc_get_handler_data(desc);
	struct spinal_lib_gpio *chip = *self_ptr;
	int offset = self_ptr - &chip->self_ptr[0];
//	u32 bit = BIT(offset);

	chained_irq_enter(irqchip, desc);

	/* Re-arm the edge triggers so don't miss the next one */
//	iowrite32(bit, chip->base + GPIO_RISE_IP);
//	iowrite32(bit, chip->base + GPIO_FALL_IP);

	generic_handle_irq(irq_find_mapping(chip->gc.irq.domain, offset));

	/* Re-arm the level triggers after handling to prevent spurious refire */
//	iowrite32(bit, chip->base + GPIO_HIGH_IP);
//	iowrite32(bit, chip->base + GPIO_LOW_IP);

	chained_irq_exit(irqchip, desc);
}

static int spinal_lib_gpio_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *node = pdev->dev.of_node;
	struct spinal_lib_gpio *chip;
	struct resource *res;
	int gpio, irq, ret, ngpio;
    int i;

	chip = devm_kzalloc(dev, sizeof(*chip), GFP_KERNEL);
	if (!chip) {
		dev_err(dev, "out of memory\n");
		return -ENOMEM;
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	chip->base = devm_ioremap_resource(dev, res);
	if (IS_ERR(chip->base)) {
		dev_err(dev, "failed to allocate device memory\n");
		return PTR_ERR(chip->base);
	}

	if (of_property_read_u32(node, "ngpio", &ngpio))
		ngpio = MAX_GPIO;


	if (ngpio > MAX_GPIO) {
		dev_err(dev, "too many GPIO\n");
		return -EINVAL;
	}

	raw_spin_lock_init(&chip->lock);
	chip->gc.direction_input = spinal_lib_gpio_direction_input;
	chip->gc.direction_output = spinal_lib_gpio_direction_output;
	chip->gc.get_direction = spinal_lib_gpio_get_direction;
	chip->gc.get = spinal_lib_gpio_get_value;
	chip->gc.set = spinal_lib_gpio_set_value;
	chip->gc.base = -1;
	chip->gc.ngpio = ngpio;
	chip->gc.label = dev_name(dev);
	chip->gc.parent = dev;
	chip->gc.owner = THIS_MODULE;

	ret = gpiochip_add_data(&chip->gc, chip);
	if (ret)
		return ret;

	/* Disable all GPIO interrupts before enabling parent interrupts */
	iowrite32(0, chip->base + GPIO_OUTPUT_ENABLE);
	iowrite32(0, chip->base + GPIO_RISE_IE);
	iowrite32(0, chip->base + GPIO_FALL_IE);
	iowrite32(0, chip->base + GPIO_HIGH_IE);
	iowrite32(0, chip->base + GPIO_LOW_IE);
	chip->interrupt_mask = 0xFFFFFFFF;
	chip->enabled = 0;



	ret = gpiochip_irqchip_add(&chip->gc, &spinal_lib_gpio_irqchip, 0, handle_level_irq, IRQ_TYPE_NONE);
	if (ret) {
		dev_err(dev, "could not add irqchip\n");
		gpiochip_remove(&chip->gc);
		return ret;
	}

	chip->gc.irq.num_parents = ngpio;
	chip->gc.irq.parents = &chip->irq_parent[0];
	chip->gc.irq.map = &chip->irq_parent[0];

	for (gpio = 0; gpio < ngpio; ++gpio) {
		chip->irq_parent[gpio] = -1;
		chip->self_ptr[gpio] = chip;
		chip->trigger[gpio] = IRQ_TYPE_NONE;
	}

	if(of_find_property(pdev->dev.of_node, "interrupts-pin", NULL)){
		for(i = 0;;i++){
			if (of_property_read_u32_index(pdev->dev.of_node, "interrupts-pin", i, &gpio) != 0) break;
			irq = platform_get_irq(pdev, i);
			chip->irq_parent[gpio] = irq;
		}
	} else {
		for (gpio = 0; gpio < ngpio; ++gpio) {
			irq = platform_get_irq(pdev, gpio);
			chip->irq_parent[gpio] = irq;
		}
	}

	for (gpio = 0; gpio < ngpio; ++gpio) {
		irq = chip->irq_parent[gpio];
		if(irq < 0) continue;

		irq_set_chained_handler_and_data(irq, spinal_lib_gpio_irq_handler, &chip->self_ptr[gpio]);
		irq_set_parent(irq_find_mapping(chip->gc.irq.domain, gpio), irq);
	}

	platform_set_drvdata(pdev, chip);
	dev_info(dev, "Probe success\n");

	return 0;
}

static const struct of_device_id spinal_lib_gpio_match[] = {
	{
		.compatible = "spinal-lib,gpio-1.0",
	},
	{ },
};

static struct platform_driver spinal_lib_gpio_driver = {
	.probe		= spinal_lib_gpio_probe,
	.driver = {
		.name	= "spinal_lib_gpio",
		.of_match_table = of_match_ptr(spinal_lib_gpio_match),
	},
};
builtin_platform_driver(spinal_lib_gpio_driver)

/*
 * arch/arm/mach-tegra/gpio.c
 *
 * Copyright (c) 2010 Google, Inc
 * Copyright (c) 2011-2021, NVIDIA CORPORATION.  All rights reserved.
 *
 * Author:
 *	Erik Gilling <konkers@google.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/err.h>
#include <linux/init.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/gpio.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/irqdomain.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/pinctrl/consumer.h>
#include <linux/pm.h>
#include <linux/syscore_ops.h>

#define GPIO_BANK(x)		((x) >> 5)
#define GPIO_PORT(x)		(((x) >> 3) & 0x3)
#define GPIO_BIT(x)		((x) & 0x7)

#define GPIO_REG(tgi, x)	(GPIO_BANK(x) * tgi->soc->bank_stride + \
					GPIO_PORT(x) * 4)

#define GPIO_CNF(t, x)		(GPIO_REG(t, x) + 0x00)
#define GPIO_OE(t, x)		(GPIO_REG(t, x) + 0x10)
#define GPIO_OUT(t, x)		(GPIO_REG(t, x) + 0X20)
#define GPIO_IN(t, x)		(GPIO_REG(t, x) + 0x30)
#define GPIO_INT_STA(t, x)	(GPIO_REG(t, x) + 0x40)
#define GPIO_INT_ENB(t, x)	(GPIO_REG(t, x) + 0x50)
#define GPIO_INT_LVL(t, x)	(GPIO_REG(t, x) + 0x60)
#define GPIO_INT_CLR(t, x)	(GPIO_REG(t, x) + 0x70)
#define GPIO_DBC_CNT(t, x)	(GPIO_REG(t, x) + 0xF0)


#define GPIO_MSK_CNF(t, x)	(GPIO_REG(t, x) + t->soc->upper_offset + 0x00)
#define GPIO_MSK_OE(t, x)	(GPIO_REG(t, x) + t->soc->upper_offset + 0x10)
#define GPIO_MSK_OUT(t, x)	(GPIO_REG(t, x) + t->soc->upper_offset + 0X20)
#define GPIO_MSK_DBC_EN(t, x)	(GPIO_REG(t, x) + t->soc->upper_offset + 0x30)
#define GPIO_MSK_INT_STA(t, x)	(GPIO_REG(t, x) + t->soc->upper_offset + 0x40)
#define GPIO_MSK_INT_ENB(t, x)	(GPIO_REG(t, x) + t->soc->upper_offset + 0x50)
#define GPIO_MSK_INT_LVL(t, x)	(GPIO_REG(t, x) + t->soc->upper_offset + 0x60)

#define GPIO_INT_LVL_MASK		0x010101
#define GPIO_INT_LVL_EDGE_RISING	0x000101
#define GPIO_INT_LVL_EDGE_FALLING	0x000100
#define GPIO_INT_LVL_EDGE_BOTH		0x010100
#define GPIO_INT_LVL_LEVEL_HIGH		0x000001
#define GPIO_INT_LVL_LEVEL_LOW		0x000000

struct tegra_gpio_info;

struct tegra_gpio_bank {
	int bank;
	int irq;
	spinlock_t gpio_lock[4];
#ifdef CONFIG_PM_SLEEP
	u32 cnf[4];
	u32 out[4];
	u32 oe[4];
	u32 int_enb[4];
	u32 int_lvl[4];
	u32 wake_enb[4];
	u32 dbc_enb[4];
#endif
	u32 dbc_cnt[4];
	u32 cnf_init[4];
	u32 out_init[4];
	u32 oe_init[4];
	struct tegra_gpio_info *tgi;
};

struct tegra_gpio_soc_config {
	bool debounce_supported;
	u32 bank_stride;
	u32 upper_offset;
};

struct tegra_gpio_info {
	struct device				*dev;
	void __iomem				*regs;
	struct irq_domain			*irq_domain;
	struct tegra_gpio_bank			*bank_info;
	const struct tegra_gpio_soc_config	*soc;
	struct gpio_chip			gc;
	struct irq_chip				ic;
	u32					bank_count;
};
static struct tegra_gpio_info *gpio_info;

static bool tegra_gpio_is_gpio_force_disabled(unsigned int gpio)
{
	return	(gpio >= 12 && gpio <= 20) || gpio == 232 || // SPI 1 & 2
			(gpio >= 76 && gpio <= 79); // Pins for I2S

	/*
	This disables the following gpio pins:
	 c 12
	 d 13
	 e 14
	 f 15
	e8 232
	10 16
	11 17
	12 18
	13 19
	14 20

	4C 76
	4D 77
	4E 78
	4F 79
	*/
}

static inline void tegra_gpio_writel(struct tegra_gpio_info *tgi,
				     u32 val, u32 reg)
{
	__raw_writel(val, tgi->regs + reg);
}

static inline u32 tegra_gpio_readl(struct tegra_gpio_info *tgi, u32 reg)
{
	return __raw_readl(tgi->regs + reg);
}

static int tegra_gpio_compose(int bank, int port, int bit)
{
	return (bank << 5) | ((port & 0x3) << 3) | (bit & 0x7);
}

static void tegra_gpio_mask_write(struct tegra_gpio_info *tgi, u32 reg,
				  int gpio, int value)
{
	u32 val;

	val = 0x100 << GPIO_BIT(gpio);
	if (value)
		val |= 1 << GPIO_BIT(gpio);
	tegra_gpio_writel(tgi, val, reg);
}

static void tegra_gpio_save_gpio_state(unsigned int gpio)
{
	struct tegra_gpio_info *tgi = gpio_info;
	int b = GPIO_BANK(gpio);
	int p = GPIO_PORT(gpio);
	struct tegra_gpio_bank *bank = &tgi->bank_info[b];
	u32 mask = BIT(GPIO_BIT(gpio));
	unsigned long flags;

	spin_lock_irqsave(&bank->gpio_lock[p], flags);

	bank->cnf_init[p] &= ~mask;
	//if (!tegra_gpio_is_gpio_force_disabled(gpio))
	bank->cnf_init[p] |= tegra_gpio_readl(tgi, GPIO_CNF(tgi, gpio)) & mask;

	bank->out_init[p] &= ~mask;
	bank->out_init[p] |= tegra_gpio_readl(tgi, GPIO_OUT(tgi, gpio)) & mask;

	bank->oe_init[p] &= ~mask;
	bank->oe_init[p] |= tegra_gpio_readl(tgi, GPIO_OE(tgi, gpio)) & mask;

	spin_unlock_irqrestore(&bank->gpio_lock[p], flags);
}

static void tegra_gpio_restore_gpio_state(unsigned int gpio)
{
	struct tegra_gpio_info *tgi = gpio_info;
	int b = GPIO_BANK(gpio);
	int p = GPIO_PORT(gpio);
	int bpos = GPIO_BIT(gpio);
	struct tegra_gpio_bank *bank = &tgi->bank_info[b];
	int was_gpio, was_output, was_out_val;
	unsigned long flags;

	spin_lock_irqsave(&bank->gpio_lock[p], flags);

	was_gpio = (bank->cnf_init[p] >> bpos) & 0x1;
	was_output = (bank->oe_init[p] >> bpos) & 0x1;
	was_out_val = (bank->out_init[p] >> bpos) & 0x1;
	
	//if (tegra_gpio_is_gpio_force_disabled(gpio))
	//	was_gpio = 0;

	/*
	 * If pin was GPIO and pin direction was OUT then restore GPIO_OUT first
	 * then GPIO_OE and then GPIO_CNF
	 */
	if (was_gpio & was_output) {
		tegra_gpio_mask_write(tgi, GPIO_MSK_OUT(tgi, gpio), gpio,
								was_out_val);
		tegra_gpio_mask_write(tgi, GPIO_MSK_OE(tgi, gpio), gpio,
								was_output);
		tegra_gpio_mask_write(tgi, GPIO_MSK_CNF(tgi, gpio), gpio,
								was_gpio);
	}
	/*
	 * If pin was GPIO and pin direction was IN then restore GPIO_OE,
	 * then GPIO_OUT and then GPIO_CNF
	 */
	else if (was_gpio) {
		tegra_gpio_mask_write(tgi, GPIO_MSK_OE(tgi, gpio), gpio,
								was_output);
		tegra_gpio_mask_write(tgi, GPIO_MSK_OUT(tgi, gpio), gpio,
								was_out_val);
		tegra_gpio_mask_write(tgi, GPIO_MSK_CNF(tgi, gpio), gpio,
								was_gpio);
	}
	/*
	 * If pin is SFIO then restore GPIO_CNF, then GPIO_OE and then GPIO_OUT
	 */
	else {
		tegra_gpio_mask_write(tgi, GPIO_MSK_CNF(tgi, gpio), gpio,
								was_gpio);
		tegra_gpio_mask_write(tgi, GPIO_MSK_OE(tgi, gpio), gpio,
								was_output);
		tegra_gpio_mask_write(tgi, GPIO_MSK_OUT(tgi, gpio), gpio,
								was_out_val);
	}

	spin_unlock_irqrestore(&bank->gpio_lock[p], flags);
}

static void tegra_gpio_enable(struct tegra_gpio_info *tgi, int gpio)
{
	tegra_gpio_mask_write(tgi, GPIO_MSK_CNF(tgi, gpio), gpio, 1);
}

static void tegra_gpio_disable(struct tegra_gpio_info *tgi, int gpio)
{
	tegra_gpio_mask_write(tgi, GPIO_MSK_CNF(tgi, gpio), gpio, 0);
}

static int tegra_gpio_request(struct gpio_chip *chip, unsigned offset)
{
	tegra_gpio_save_gpio_state(offset);
	return pinctrl_request_gpio(chip->base + offset);
}

static void tegra_gpio_free(struct gpio_chip *chip, unsigned offset)
{
	struct tegra_gpio_info *tgi = gpiochip_get_data(chip);
	pinctrl_free_gpio(chip->base + offset);
	tegra_gpio_restore_gpio_state(offset);
	tegra_gpio_disable(tgi, offset);
}

static void tegra_gpio_set(struct gpio_chip *chip, unsigned offset, int value)
{
	struct tegra_gpio_info *tgi = gpiochip_get_data(chip);

	tegra_gpio_mask_write(tgi, GPIO_MSK_OUT(tgi, offset), offset, value);
}

static int tegra_gpio_get(struct gpio_chip *chip, unsigned offset)
{
	struct tegra_gpio_info *tgi = gpiochip_get_data(chip);
	int bval = BIT(GPIO_BIT(offset));

	/* If gpio is in output mode then read from the out value */
	if (tegra_gpio_readl(tgi, GPIO_OE(tgi, offset)) & bval)
		return !!(tegra_gpio_readl(tgi, GPIO_OUT(tgi, offset)) & bval);

	return !!(tegra_gpio_readl(tgi, GPIO_IN(tgi, offset)) & bval);
}

static int tegra_gpio_direction_input(struct gpio_chip *chip, unsigned offset)
{
	int ret;
	struct tegra_gpio_info *tgi = gpiochip_get_data(chip);

	if (tegra_gpio_is_gpio_force_disabled(offset)) {
		dev_err(tgi->dev,
			"Tried to set force disabled GPIO %d %d as input",
			 chip->base + offset, offset);
		//return -1;
	}

	tegra_gpio_mask_write(tgi, GPIO_MSK_OE(tgi, offset), offset, 0);
	tegra_gpio_enable(tgi, offset);

	ret = pinctrl_gpio_direction_input(chip->base + offset);
	if (ret < 0)
		dev_err(tgi->dev,
			"Failed to set pinctrl input direction of GPIO %d: %d",
			 chip->base + offset, ret);

	return ret;
}

static int tegra_gpio_direction_output(struct gpio_chip *chip, unsigned offset,
					int value)
{
	struct tegra_gpio_info *tgi = gpiochip_get_data(chip);
	int ret;

	if (tegra_gpio_is_gpio_force_disabled(offset)) {
		dev_err(tgi->dev,
			"Tried to set force disabled GPIO %d %d as output",
			 chip->base + offset, offset);
		//return -1;
	}

	tegra_gpio_set(chip, offset, value);
	tegra_gpio_mask_write(tgi, GPIO_MSK_OE(tgi, offset), offset, 1);
	tegra_gpio_enable(tgi, offset);

	ret = pinctrl_gpio_direction_output(chip->base + offset);
	if (ret < 0)
		dev_err(tgi->dev,
			"Failed to set pinctrl output direction of GPIO %d: %d",
			 chip->base + offset, ret);

	return ret;
}

static int tegra_gpio_get_direction(struct gpio_chip *chip, unsigned offset)
{
	struct tegra_gpio_info *tgi = gpiochip_get_data(chip);
	u32 pin_mask = BIT(GPIO_BIT(offset));
	u32 cnf, oe;

	cnf = tegra_gpio_readl(tgi, GPIO_CNF(tgi, offset));
	if (!(cnf & pin_mask))
		return -EINVAL;

	oe = tegra_gpio_readl(tgi, GPIO_OE(tgi, offset));

	return (oe & pin_mask) ? GPIOF_DIR_OUT : GPIOF_DIR_IN;
}

static int tegra_gpio_set_debounce(struct gpio_chip *chip, unsigned int offset,
				   unsigned int debounce)
{
	struct tegra_gpio_info *tgi = gpiochip_get_data(chip);
	struct tegra_gpio_bank *bank = &tgi->bank_info[GPIO_BANK(offset)];
	unsigned int debounce_ms = DIV_ROUND_UP(debounce, 1000);
	unsigned long flags;
	int port;

	if (!debounce_ms) {
		tegra_gpio_mask_write(tgi, GPIO_MSK_DBC_EN(tgi, offset),
				      offset, 0);
		return 0;
	}

	debounce_ms = min(debounce_ms, 255U);
	port = GPIO_PORT(offset);

	/*
	 * There is only one debounce count register per port and hence
	 * set the maximum of current and requested debounce time.
	 */
	spin_lock_irqsave(&bank->gpio_lock[port], flags);
	if (bank->dbc_cnt[port] < debounce_ms) {
		tegra_gpio_writel(tgi, debounce_ms, GPIO_DBC_CNT(tgi, offset));
		bank->dbc_cnt[port] = debounce_ms;
	}
	spin_unlock_irqrestore(&bank->gpio_lock[port], flags);

	tegra_gpio_mask_write(tgi, GPIO_MSK_DBC_EN(tgi, offset), offset, 1);

	return 0;
}

static int tegra_gpio_to_irq(struct gpio_chip *chip, unsigned offset)
{
	struct tegra_gpio_info *tgi = gpiochip_get_data(chip);

	return irq_find_mapping(tgi->irq_domain, offset);
}

static void tegra_gpio_irq_ack(struct irq_data *d)
{
	struct tegra_gpio_bank *bank = irq_data_get_irq_chip_data(d);
	struct tegra_gpio_info *tgi = bank->tgi;
	int gpio = d->hwirq;

	tegra_gpio_writel(tgi, 1 << GPIO_BIT(gpio), GPIO_INT_CLR(tgi, gpio));
}

static void tegra_gpio_irq_mask(struct irq_data *d)
{
	struct tegra_gpio_bank *bank = irq_data_get_irq_chip_data(d);
	struct tegra_gpio_info *tgi = bank->tgi;
	int gpio = d->hwirq;

	tegra_gpio_mask_write(tgi, GPIO_MSK_INT_ENB(tgi, gpio), gpio, 0);
}

static void tegra_gpio_irq_unmask(struct irq_data *d)
{
	struct tegra_gpio_bank *bank = irq_data_get_irq_chip_data(d);
	struct tegra_gpio_info *tgi = bank->tgi;
	int gpio = d->hwirq;

	tegra_gpio_mask_write(tgi, GPIO_MSK_INT_ENB(tgi, gpio), gpio, 1);
}

static int tegra_gpio_irq_set_type(struct irq_data *d, unsigned int type)
{
	int gpio = d->hwirq;
	struct tegra_gpio_bank *bank = irq_data_get_irq_chip_data(d);
	struct tegra_gpio_info *tgi = bank->tgi;
	int port = GPIO_PORT(gpio);
	int lvl_type;
	int val;
	unsigned long flags;
	int ret;

	switch (type & IRQ_TYPE_SENSE_MASK) {
	case IRQ_TYPE_EDGE_RISING:
		lvl_type = GPIO_INT_LVL_EDGE_RISING;
		break;

	case IRQ_TYPE_EDGE_FALLING:
		lvl_type = GPIO_INT_LVL_EDGE_FALLING;
		break;

	case IRQ_TYPE_EDGE_BOTH:
		lvl_type = GPIO_INT_LVL_EDGE_BOTH;
		break;

	case IRQ_TYPE_LEVEL_HIGH:
		lvl_type = GPIO_INT_LVL_LEVEL_HIGH;
		break;

	case IRQ_TYPE_LEVEL_LOW:
		lvl_type = GPIO_INT_LVL_LEVEL_LOW;
		break;

	default:
		return -EINVAL;
	}

	ret = gpiochip_lock_as_irq(&tgi->gc, gpio);
	if (ret) {
		dev_err(tgi->dev,
			"unable to lock Tegra GPIO %d as IRQ\n", gpio);
		return ret;
	}

	spin_lock_irqsave(&bank->gpio_lock[port], flags);

	val = tegra_gpio_readl(tgi, GPIO_INT_LVL(tgi, gpio));
	val &= ~(GPIO_INT_LVL_MASK << GPIO_BIT(gpio));
	val |= lvl_type << GPIO_BIT(gpio);
	tegra_gpio_writel(tgi, val, GPIO_INT_LVL(tgi, gpio));

	spin_unlock_irqrestore(&bank->gpio_lock[port], flags);

	if (type & (IRQ_TYPE_LEVEL_LOW | IRQ_TYPE_LEVEL_HIGH))
		irq_set_handler_locked(d, handle_level_irq);
	else if (type & (IRQ_TYPE_EDGE_FALLING | IRQ_TYPE_EDGE_RISING))
		irq_set_handler_locked(d, handle_edge_irq);

	return 0;
}

static void tegra_gpio_irq_shutdown(struct irq_data *d)
{
	struct tegra_gpio_bank *bank = irq_data_get_irq_chip_data(d);
	struct tegra_gpio_info *tgi = bank->tgi;
	int gpio = d->hwirq;

	tegra_gpio_irq_mask(d);
	gpiochip_unlock_as_irq(&tgi->gc, gpio);
}

static void tegra_gpio_irq_handler(struct irq_desc *desc)
{
	int port;
	int pin;
	int unmasked = 0;
	int gpio;
	u32 lvl;
	unsigned long sta;
	struct irq_chip *chip = irq_desc_get_chip(desc);
	struct tegra_gpio_bank *bank = irq_desc_get_handler_data(desc);
	struct tegra_gpio_info *tgi = bank->tgi;

	chained_irq_enter(chip, desc);

	for (port = 0; port < 4; port++) {
		gpio = tegra_gpio_compose(bank->bank, port, 0);
		sta = tegra_gpio_readl(tgi, GPIO_INT_STA(tgi, gpio)) &
			tegra_gpio_readl(tgi, GPIO_INT_ENB(tgi, gpio));
		lvl = tegra_gpio_readl(tgi, GPIO_INT_LVL(tgi, gpio));

		for_each_set_bit(pin, &sta, 8) {
			tegra_gpio_writel(tgi, 1 << pin,
					  GPIO_INT_CLR(tgi, gpio));

			/*
			 * If gpio is edge triggered, clear condition
			 * before executing the handler so that we don't
			 * miss edges
			 */
			if (lvl & (0x100 << pin)) {
				unmasked = 1;
				chained_irq_exit(chip, desc);
			}

			generic_handle_irq(gpio_to_irq(gpio + pin));
		}
	}

	if (!unmasked)
		chained_irq_exit(chip, desc);

}

#ifdef CONFIG_PM_SLEEP
static void tegra_gpio_resume(void)
{
	struct tegra_gpio_info *tgi = gpio_info;
	int b;
	int p;

	for (b = 0; b < tgi->bank_count; b++) {
		struct tegra_gpio_bank *bank = &tgi->bank_info[b];

		for (p = 0; p < ARRAY_SIZE(bank->oe); p++) {
			unsigned int gpio = (b<<5) | (p<<3);
			tegra_gpio_writel(tgi, bank->cnf[p],
					  GPIO_CNF(tgi, gpio));

			if (tgi->soc->debounce_supported) {
				tegra_gpio_writel(tgi, bank->dbc_cnt[p],
						  GPIO_DBC_CNT(tgi, gpio));
				tegra_gpio_writel(tgi, bank->dbc_enb[p],
						  GPIO_MSK_DBC_EN(tgi, gpio));
			}

			tegra_gpio_writel(tgi, bank->out[p],
					  GPIO_OUT(tgi, gpio));
			tegra_gpio_writel(tgi, bank->oe[p],
					  GPIO_OE(tgi, gpio));
			tegra_gpio_writel(tgi, bank->int_lvl[p],
					  GPIO_INT_LVL(tgi, gpio));
			tegra_gpio_writel(tgi, bank->int_enb[p],
					  GPIO_INT_ENB(tgi, gpio));
		}
	}
}

static int tegra_gpio_suspend(void)
{
	struct tegra_gpio_info *tgi = gpio_info;
	int b;
	int p;

	for (b = 0; b < tgi->bank_count; b++) {
		struct tegra_gpio_bank *bank = &tgi->bank_info[b];

		for (p = 0; p < ARRAY_SIZE(bank->oe); p++) {
			unsigned int gpio = (b<<5) | (p<<3);
			bank->cnf[p] = tegra_gpio_readl(tgi,
							GPIO_CNF(tgi, gpio));
			bank->out[p] = tegra_gpio_readl(tgi,
							GPIO_OUT(tgi, gpio));
			bank->oe[p] = tegra_gpio_readl(tgi,
						       GPIO_OE(tgi, gpio));
			if (tgi->soc->debounce_supported) {
				bank->dbc_enb[p] = tegra_gpio_readl(tgi,
						GPIO_MSK_DBC_EN(tgi, gpio));
				bank->dbc_enb[p] = (bank->dbc_enb[p] << 8) |
							bank->dbc_enb[p];
			}

			bank->int_enb[p] = tegra_gpio_readl(tgi,
						GPIO_INT_ENB(tgi, gpio));
			bank->int_lvl[p] = tegra_gpio_readl(tgi,
						GPIO_INT_LVL(tgi, gpio));

			/* Enable gpio irq for wake up source */
			tegra_gpio_writel(tgi, bank->wake_enb[p],
					  GPIO_INT_ENB(tgi, gpio));
		}
	}

	of_gpiochip_suspend(&tgi->gc);
	return 0;
}

static int tegra_gpio_irq_set_wake(struct irq_data *d, unsigned int enable)
{
	struct tegra_gpio_bank *bank = irq_data_get_irq_chip_data(d);
	int gpio = d->hwirq;
	u32 port, bit, mask;

	port = GPIO_PORT(gpio);
	bit = GPIO_BIT(gpio);
	mask = BIT(bit);

	if (enable)
		bank->wake_enb[port] |= mask;
	else
		bank->wake_enb[port] &= ~mask;

	return irq_set_irq_wake(bank->irq, enable);
}
#else
#define tegra_gpio_suspend NULL
#define tegra_gpio_resume NULL
#endif

static struct syscore_ops tegra_gpio_syscore_ops = {
	.suspend = tegra_gpio_suspend,
	.early_resume = tegra_gpio_resume,
	.save = tegra_gpio_suspend,
	.restore = tegra_gpio_resume,
};

static int tegra_gpio_irq_request_resources(struct irq_data *d)
{
	struct tegra_gpio_bank *bank = irq_data_get_irq_chip_data(d);

	return tegra_gpio_direction_input(&bank->tgi->gc, d->hwirq);
}

#ifdef	CONFIG_DEBUG_FS

#include <linux/debugfs.h>
#include <linux/seq_file.h>

static int dbg_gpio_show(struct seq_file *s, void *unused)
{
	struct tegra_gpio_info *tgi = s->private;
	int i;
	int j;
	char x, y;

	x = ' ';
	y = 'A';

	seq_printf(s, "Name:Bank:Port CNF OE OUT IN INT_STA INT_ENB INT_LVL\n");
	for (i = 0; i < tgi->bank_count; i++) {
		for (j = 0; j < 4; j++) {
			int gpio = tegra_gpio_compose(i, j, 0);

			seq_printf(s,
			     "%c%c: %d:%d %02x %02x %02x %02x %02x %02x %06x\n",
				x, y, i, j,
				tegra_gpio_readl(tgi, GPIO_CNF(tgi, gpio)),
				tegra_gpio_readl(tgi, GPIO_OE(tgi, gpio)),
				tegra_gpio_readl(tgi, GPIO_OUT(tgi, gpio)),
				tegra_gpio_readl(tgi, GPIO_IN(tgi, gpio)),
				tegra_gpio_readl(tgi, GPIO_INT_STA(tgi, gpio)),
				tegra_gpio_readl(tgi, GPIO_INT_ENB(tgi, gpio)),
				tegra_gpio_readl(tgi, GPIO_INT_LVL(tgi, gpio)));

			if (x != ' ')
				x++;
			if (y == 'Z') {
				y = 'A';
				x = 'A';
			} else {
				y++;
			};
		}
	}
	return 0;
}

static int dbg_gpio_open(struct inode *inode, struct file *file)
{
	return single_open(file, dbg_gpio_show, inode->i_private);
}

static const struct file_operations debug_fops = {
	.open		= dbg_gpio_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static void tegra_gpio_debuginit(struct tegra_gpio_info *tgi)
{
	(void) debugfs_create_file("tegra_gpio", S_IRUGO,
					NULL, tgi, &debug_fops);
}

#else

static inline void tegra_gpio_debuginit(struct tegra_gpio_info *tgi)
{
}

#endif

/*
 * This lock class tells lockdep that GPIO irqs are in a different category
 * than their parents, so it won't report false recursion.
 */
static struct lock_class_key gpio_lock_class;

static int tegra_gpio_probe(struct platform_device *pdev)
{
	const struct tegra_gpio_soc_config *config;
	struct tegra_gpio_info *tgi;
	struct resource *res;
	struct tegra_gpio_bank *bank;
	int ret;
	int gpio;
	int i;
	int j;

	config = of_device_get_match_data(&pdev->dev);
	if (!config) {
		dev_err(&pdev->dev, "Error: No device match found\n");
		return -ENODEV;
	}

	tgi = devm_kzalloc(&pdev->dev, sizeof(*tgi), GFP_KERNEL);
	if (!tgi)
		return -ENODEV;
	gpio_info = tgi;

	tgi->soc = config;
	tgi->dev = &pdev->dev;

	for (;;) {
		res = platform_get_resource(pdev, IORESOURCE_IRQ,
					    tgi->bank_count);
		if (!res)
			break;
		tgi->bank_count++;
	}
	if (!tgi->bank_count) {
		dev_err(&pdev->dev, "Missing IRQ resource\n");
		return -ENODEV;
	}

	tgi->gc.label			= "tegra-gpio";
	tgi->gc.request			= tegra_gpio_request;
	tgi->gc.free			= tegra_gpio_free;
	tgi->gc.direction_input		= tegra_gpio_direction_input;
	tgi->gc.get			= tegra_gpio_get;
	tgi->gc.direction_output	= tegra_gpio_direction_output;
	tgi->gc.set			= tegra_gpio_set;
	tgi->gc.get_direction		= tegra_gpio_get_direction;
	tgi->gc.to_irq			= tegra_gpio_to_irq;
	tgi->gc.base			= 0;
	tgi->gc.ngpio			= tgi->bank_count * 32;
	tgi->gc.parent			= &pdev->dev;
	tgi->gc.of_node			= pdev->dev.of_node;

	tgi->ic.name			= "GPIO";
	tgi->ic.irq_ack			= tegra_gpio_irq_ack;
	tgi->ic.irq_mask		= tegra_gpio_irq_mask;
	tgi->ic.irq_unmask		= tegra_gpio_irq_unmask;
	tgi->ic.irq_set_type		= tegra_gpio_irq_set_type;
	tgi->ic.irq_shutdown		= tegra_gpio_irq_shutdown;
#ifdef CONFIG_PM_SLEEP
	tgi->ic.irq_set_wake		= tegra_gpio_irq_set_wake;
#endif
	tgi->ic.irq_request_resources   = tegra_gpio_irq_request_resources;

	platform_set_drvdata(pdev, tgi);

	if (config->debounce_supported)
		tgi->gc.set_debounce = tegra_gpio_set_debounce;

	tgi->bank_info = devm_kzalloc(&pdev->dev, tgi->bank_count *
				      sizeof(*tgi->bank_info), GFP_KERNEL);
	if (!tgi->bank_info)
		return -ENODEV;

	tgi->irq_domain = irq_domain_add_linear(pdev->dev.of_node,
						tgi->gc.ngpio,
						&irq_domain_simple_ops, NULL);
	if (!tgi->irq_domain)
		return -ENODEV;

	for (i = 0; i < tgi->bank_count; i++) {
		res = platform_get_resource(pdev, IORESOURCE_IRQ, i);
		if (!res) {
			dev_err(&pdev->dev, "Missing IRQ resource\n");
			return -ENODEV;
		}

		bank = &tgi->bank_info[i];
		bank->bank = i;
		bank->irq = res->start;
		bank->tgi = tgi;
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	tgi->regs = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(tgi->regs))
		return PTR_ERR(tgi->regs);

	for (i = 0; i < tgi->bank_count; i++) {
		for (j = 0; j < 4; j++) {
			int gpio = tegra_gpio_compose(i, j, 0);
			tegra_gpio_writel(tgi, 0x00, GPIO_INT_ENB(tgi, gpio));
		}
	}

	for (i = 0; i < tgi->bank_count; i++) {
		bank = &tgi->bank_info[i];

		irq_set_chained_handler_and_data(bank->irq,
				tegra_gpio_irq_handler, bank);

		for (j = 0; j < 4; j++)
			spin_lock_init(&bank->gpio_lock[j]);
	}

	ret = devm_gpiochip_add_data(&pdev->dev, &tgi->gc, tgi);
	if (ret < 0) {
		irq_domain_remove(tgi->irq_domain);
		return ret;
	}

	for (gpio = 0; gpio < tgi->gc.ngpio; gpio++) {
		int irq = irq_create_mapping(tgi->irq_domain, gpio);
		/* No validity check; all Tegra GPIOs are valid IRQs */

		bank = &tgi->bank_info[GPIO_BANK(gpio)];

		irq_set_lockdep_class(irq, &gpio_lock_class);
		irq_set_chip_data(irq, bank);
		irq_set_chip_and_handler(irq, &tgi->ic, handle_simple_irq);
	}

	// Disable gpios that might block SPI or I2S.
	// These somehow get enabled in the early stages of booting and if we don't
	// disable them now, SPI becomes unusable.
	for (gpio = 0; gpio < tgi->gc.ngpio; gpio++) {
		int b = GPIO_BANK(gpio);
		int p = GPIO_PORT(gpio);
		u32 mask = BIT(GPIO_BIT(gpio));
		unsigned long flags;
		u32 enabled;

		if (!tegra_gpio_is_gpio_force_disabled(gpio))
			continue;

		bank = &tgi->bank_info[b];
		spin_lock_irqsave(&bank->gpio_lock[p], flags);
		enabled = tegra_gpio_readl(tgi, GPIO_CNF(tgi, gpio)) & mask;
		spin_unlock_irqrestore(&bank->gpio_lock[p], flags);

		if (enabled) {
			dev_err(&pdev->dev, "Disable GPIO %d", gpio);
			tegra_gpio_disable(tgi, gpio);
		}
	}

	tegra_gpio_debuginit(tgi);

	register_syscore_ops(&tegra_gpio_syscore_ops);

	return 0;
}

static const struct tegra_gpio_soc_config tegra20_gpio_config = {
	.bank_stride = 0x80,
	.upper_offset = 0x800,
};

static const struct tegra_gpio_soc_config tegra30_gpio_config = {
	.bank_stride = 0x100,
	.upper_offset = 0x80,
};

static const struct tegra_gpio_soc_config tegra210_gpio_config = {
	.debounce_supported = true,
	.bank_stride = 0x100,
	.upper_offset = 0x80,
};

static const struct of_device_id tegra_gpio_of_match[] = {
	{ .compatible = "nvidia,tegra210-gpio", .data = &tegra210_gpio_config },
	{ .compatible = "nvidia,tegra30-gpio", .data = &tegra30_gpio_config },
	{ .compatible = "nvidia,tegra20-gpio", .data = &tegra20_gpio_config },
	{ },
};

static struct platform_driver tegra_gpio_driver = {
	.driver		= {
		.name	= "tegra-gpio",
		.of_match_table = tegra_gpio_of_match,
	},
	.probe		= tegra_gpio_probe,
};

static int __init tegra_gpio_init(void)
{
	return platform_driver_register(&tegra_gpio_driver);
}
subsys_initcall(tegra_gpio_init);

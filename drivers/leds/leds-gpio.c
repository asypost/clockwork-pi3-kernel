// SPDX-License-Identifier: GPL-2.0-only
/*
 * LEDs driver for GPIOs
 *
 * Copyright (C) 2007 8D Technologies inc.
 * Raphael Assenat <raph@8d.com>
 * Copyright (C) 2008 Freescale Semiconductor, Inc.
 */
#include <linux/err.h>
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/kernel.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/slab.h>

struct gpio_led_data {
	struct led_classdev cdev;
	struct gpio_desc *gpiod;
	u8 can_sleep;
	u8 blinking;
	gpio_blink_set_t platform_gpio_blink_set;
};

static int leds_gpio_proc_init(const char *name);

static inline struct gpio_led_data *
			cdev_to_gpio_led_data(struct led_classdev *led_cdev)
{
	return container_of(led_cdev, struct gpio_led_data, cdev);
}

static void gpio_led_set(struct led_classdev *led_cdev,
	enum led_brightness value)
{
	struct gpio_led_data *led_dat = cdev_to_gpio_led_data(led_cdev);
	int level;

	if (value == LED_OFF)
		level = 0;
	else
		level = 1;

	if (led_dat->blinking) {
		led_dat->platform_gpio_blink_set(led_dat->gpiod, level,
						 NULL, NULL);
		led_dat->blinking = 0;
	} else {
		if (led_dat->can_sleep)
			gpiod_set_value_cansleep(led_dat->gpiod, level);
		else
			gpiod_set_value(led_dat->gpiod, level);
	}
}

static int gpio_led_set_blocking(struct led_classdev *led_cdev,
	enum led_brightness value)
{
	gpio_led_set(led_cdev, value);
	return 0;
}

static int gpio_blink_set(struct led_classdev *led_cdev,
	unsigned long *delay_on, unsigned long *delay_off)
{
	struct gpio_led_data *led_dat = cdev_to_gpio_led_data(led_cdev);

	led_dat->blinking = 1;
	return led_dat->platform_gpio_blink_set(led_dat->gpiod, GPIO_LED_BLINK,
						delay_on, delay_off);
}

static int create_gpio_led(const struct gpio_led *template,
	struct gpio_led_data *led_dat, struct device *parent,
	struct fwnode_handle *fwnode, gpio_blink_set_t blink_set)
{
	struct led_init_data init_data = {};
	int ret, state;

	led_dat->cdev.default_trigger = template->default_trigger;
	led_dat->can_sleep = gpiod_cansleep(led_dat->gpiod);
	if (!led_dat->can_sleep)
		led_dat->cdev.brightness_set = gpio_led_set;
	else
		led_dat->cdev.brightness_set_blocking = gpio_led_set_blocking;
	led_dat->blinking = 0;
	if (blink_set) {
		led_dat->platform_gpio_blink_set = blink_set;
		led_dat->cdev.blink_set = gpio_blink_set;
	}
	if (template->default_state == LEDS_GPIO_DEFSTATE_KEEP) {
		state = gpiod_get_value_cansleep(led_dat->gpiod);
		if (state < 0)
			return state;
	} else {
		state = (template->default_state == LEDS_GPIO_DEFSTATE_ON);
	}
	led_dat->cdev.brightness = state ? LED_FULL : LED_OFF;
	if (!template->retain_state_suspended)
		led_dat->cdev.flags |= LED_CORE_SUSPENDRESUME;
	if (template->panic_indicator)
		led_dat->cdev.flags |= LED_PANIC_INDICATOR;
	if (template->retain_state_shutdown)
		led_dat->cdev.flags |= LED_RETAIN_AT_SHUTDOWN;

	ret = gpiod_direction_output(led_dat->gpiod, state);
	if (ret < 0)
		return ret;

    leds_gpio_proc_init(led_dat->cdev.name);
	if (template->name) {
		led_dat->cdev.name = template->name;
		ret = devm_led_classdev_register(parent, &led_dat->cdev);
	} else {
		init_data.fwnode = fwnode;
		ret = devm_led_classdev_register_ext(parent, &led_dat->cdev,
						     &init_data);
	}

	return ret;
}

struct gpio_leds_priv {
	int num_leds;
	struct gpio_led_data leds[];
};

static struct gpio_leds_priv *leds_priv;

static inline int sizeof_gpio_leds_priv(int num_leds)
{
	return sizeof(struct gpio_leds_priv) +
		(sizeof(struct gpio_led_data) * num_leds);
}

static struct gpio_leds_priv *gpio_leds_create(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct fwnode_handle *child;
	struct gpio_leds_priv *priv;
	int count, ret;

	count = device_get_child_node_count(dev);
	if (!count)
		return ERR_PTR(-ENODEV);

	priv = devm_kzalloc(dev, sizeof_gpio_leds_priv(count), GFP_KERNEL);
	if (!priv)
		return ERR_PTR(-ENOMEM);

	device_for_each_child_node(dev, child) {
		struct gpio_led_data *led_dat = &priv->leds[priv->num_leds];
		struct gpio_led led = {};
		const char *state = NULL;

		led.gpiod = devm_fwnode_get_gpiod_from_child(dev, NULL, child,
							     GPIOD_ASIS,
							     led.name);
		if (IS_ERR(led.gpiod)) {
			fwnode_handle_put(child);
			return ERR_CAST(led.gpiod);
		}

		led_dat->gpiod = led.gpiod;

		fwnode_property_read_string(child, "linux,default-trigger",
					    &led.default_trigger);

		if (!fwnode_property_read_string(child, "default-state",
						 &state)) {
			if (!strcmp(state, "keep"))
				led.default_state = LEDS_GPIO_DEFSTATE_KEEP;
			else if (!strcmp(state, "on"))
				led.default_state = LEDS_GPIO_DEFSTATE_ON;
			else
				led.default_state = LEDS_GPIO_DEFSTATE_OFF;
		}

		if (fwnode_property_present(child, "retain-state-suspended"))
			led.retain_state_suspended = 1;
		if (fwnode_property_present(child, "retain-state-shutdown"))
			led.retain_state_shutdown = 1;
		if (fwnode_property_present(child, "panic-indicator"))
			led.panic_indicator = 1;

		ret = create_gpio_led(&led, led_dat, dev, child, NULL);
		if (ret < 0) {
			fwnode_handle_put(child);
			return ERR_PTR(ret);
		}
		priv->num_leds++;
	}
    leds_priv = priv;
	return priv;
}

static const struct of_device_id of_gpio_leds_match[] = {
	{ .compatible = "gpio-leds", },
	{},
};

MODULE_DEVICE_TABLE(of, of_gpio_leds_match);

static struct gpio_desc *gpio_led_get_gpiod(struct device *dev, int idx,
					    const struct gpio_led *template)
{
	struct gpio_desc *gpiod;
	unsigned long flags = GPIOF_OUT_INIT_LOW;
	int ret;

	/*
	 * This means the LED does not come from the device tree
	 * or ACPI, so let's try just getting it by index from the
	 * device, this will hit the board file, if any and get
	 * the GPIO from there.
	 */
	gpiod = devm_gpiod_get_index(dev, NULL, idx, flags);
	if (!IS_ERR(gpiod)) {
		gpiod_set_consumer_name(gpiod, template->name);
		return gpiod;
	}
	if (PTR_ERR(gpiod) != -ENOENT)
		return gpiod;

	/*
	 * This is the legacy code path for platform code that
	 * still uses GPIO numbers. Ultimately we would like to get
	 * rid of this block completely.
	 */

	/* skip leds that aren't available */
	if (!gpio_is_valid(template->gpio))
		return ERR_PTR(-ENOENT);

	if (template->active_low)
		flags |= GPIOF_ACTIVE_LOW;

	ret = devm_gpio_request_one(dev, template->gpio, flags,
				    template->name);
	if (ret < 0)
		return ERR_PTR(ret);

	gpiod = gpio_to_desc(template->gpio);
	if (!gpiod)
		return ERR_PTR(-EINVAL);

	return gpiod;
}

static int gpio_led_probe(struct platform_device *pdev)
{
	struct gpio_led_platform_data *pdata = dev_get_platdata(&pdev->dev);
	struct gpio_leds_priv *priv;
	int i, ret = 0;

	if (pdata && pdata->num_leds) {
		priv = devm_kzalloc(&pdev->dev,
				sizeof_gpio_leds_priv(pdata->num_leds),
					GFP_KERNEL);
		if (!priv)
			return -ENOMEM;

		priv->num_leds = pdata->num_leds;
		for (i = 0; i < priv->num_leds; i++) {
			const struct gpio_led *template = &pdata->leds[i];
			struct gpio_led_data *led_dat = &priv->leds[i];

			if (template->gpiod)
				led_dat->gpiod = template->gpiod;
			else
				led_dat->gpiod =
					gpio_led_get_gpiod(&pdev->dev,
							   i, template);
			if (IS_ERR(led_dat->gpiod)) {
				dev_info(&pdev->dev, "Skipping unavailable LED gpio %d (%s)\n",
					 template->gpio, template->name);
				continue;
			}

			ret = create_gpio_led(template, led_dat,
					      &pdev->dev, NULL,
					      pdata->gpio_blink_set);
			if (ret < 0)
				return ret;
		}
	} else {
		priv = gpio_leds_create(pdev);
		if (IS_ERR(priv))
			return PTR_ERR(priv);
	}

	platform_set_drvdata(pdev, priv);

	return 0;
}

static void gpio_led_shutdown(struct platform_device *pdev)
{
	struct gpio_leds_priv *priv = platform_get_drvdata(pdev);
	int i;

	for (i = 0; i < priv->num_leds; i++) {
		struct gpio_led_data *led = &priv->leds[i];

		if (!(led->cdev.flags & LED_RETAIN_AT_SHUTDOWN))
			gpio_led_set(&led->cdev, LED_OFF);
	}
}

static struct platform_driver gpio_led_driver = {
	.probe		= gpio_led_probe,
	.shutdown	= gpio_led_shutdown,
	.driver		= {
		.name	= "leds-gpio",
		.of_match_table = of_gpio_leds_match,
	},
};

module_platform_driver(gpio_led_driver);

#ifdef CONFIG_PROC_FS

#include <linux/proc_fs.h>
#include <linux/uaccess.h>

static char global_buffer[64];

static int leds_gpio_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "leds_gpio\n");
	return 0;
}

static int leds_gpio_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, leds_gpio_proc_show, NULL);
}

static int leds_gpio_proc_read(struct file * file, char __user * buf, size_t size, loff_t * loff)
{
	int value, len, i;

	for(i = 0; i < leds_priv->num_leds; i++) {
		if(strcmp(leds_priv->leds[i].cdev.name, file->f_path.dentry->d_iname) == 0) {
			value = leds_priv->leds[i].cdev.brightness;
			len = snprintf(global_buffer, sizeof(global_buffer), "%d\n", value);
			return simple_read_from_buffer(buf, size, loff, global_buffer, len);
		}
	}

	return 0;
}

static int leds_gpio_proc_write(struct file * file, const char __user * buf, size_t size, loff_t * loff)
{
	int value, i;

	if(copy_from_user(global_buffer, buf, size))
		return -EFAULT;

	global_buffer[size] = 0;
	for(i = 0; i < leds_priv->num_leds; i++) {
		if(strcmp(leds_priv->leds[i].cdev.name, file->f_path.dentry->d_iname) == 0) {
			value = simple_strtol(global_buffer, 0, 10);
			gpio_led_set(&leds_priv->leds[i].cdev, value);
			leds_priv->leds[i].cdev.brightness = value;
			break;
		}
	}

	return size;
}

static const struct file_operations leds_gpio_proc_fops = {
	.open		= leds_gpio_proc_open,
	.read		= leds_gpio_proc_read,
	.write		= leds_gpio_proc_write,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int leds_gpio_proc_init(const char *name)
{
	struct proc_dir_entry *r;
	char buf[50];

	sprintf(buf, "driver/%s", name);
	r = proc_create(buf, S_IRWXUGO, NULL, &leds_gpio_proc_fops);
	if (!r)
		return -ENOMEM;
	return 0;
}
#else
static inline int leds_gpio_proc_init(const char *name) { return 0; }
#endif /* CONFIG_PROC_FS */

MODULE_AUTHOR("Raphael Assenat <raph@8d.com>, Trent Piepho <tpiepho@freescale.com>");
MODULE_DESCRIPTION("GPIO LED driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:leds-gpio");

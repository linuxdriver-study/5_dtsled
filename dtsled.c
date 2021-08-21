#include <linux/module.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/io.h>
#include <linux/ide.h>
#include <linux/device.h>
#include <linux/of.h>
#include <linux/of_address.h>

#define DTSLEDDEV_COUNT         1
#define DTSLEDDEV_NAME          "dtsled"

#define LED_ON          1
#define LED_OFF         0

/* 映射后的虚拟地址 */
static void __iomem *CCM_CCGR1;
static void __iomem *SW_MUX_GPIO1_IO03;
static void __iomem *SW_PAD_GPIO1_IO03;
static void __iomem *GPIO1_DR;
static void __iomem *GPIO1_DIR;

static int dtsled_open(struct inode *inode, struct file *file);
static ssize_t dtsled_write(struct file *file, const char __user *user, size_t size, loff_t *loff);
static int dtsled_release(struct inode *inode, struct file *file);

struct dtsled_dev {
        struct cdev led_cdev;
        struct class *class;    /* 类 */
        struct device *device;  /* 设备 */
        struct device_node *nd;
        dev_t devid;
        int major;
        int minor;
};
struct dtsled_dev dtsled;

static const struct file_operations led_ops = {
        .owner = THIS_MODULE,
        .open = dtsled_open,
        .write = dtsled_write,
        .release = dtsled_release,
};

static void led_switch(unsigned char led_status)
{
        unsigned int val = 0;
        if (led_status == LED_ON) {
                val = readl(GPIO1_DR);
                val &= ~(1<<3);
                writel(val, GPIO1_DR);
        } else {
                val = readl(GPIO1_DR);
                val |= (1<<3);
                writel(val, GPIO1_DR);
        }
}

static int led_gpio_init(void)
{
        unsigned int val = 0, i = 0;
        int ret = 0;
        u32 reg_value[10] = {0};

        /* 获取设备树内容 */
        dtsled.nd = of_find_node_by_path("/alphaled");
        if (dtsled.nd == NULL) {
                ret = -EINVAL;
                goto find_node_err;
        }
#if 0
        ret = of_property_read_u32_array(dtsled.nd, "reg", reg_value, 10);
        if (ret != 0) {
                printk("read u32 array error!\n");
                ret = -EINVAL;
                goto fail_read_array;
        } else {
                printk("reg:");
                for (i = 0; i < 10; i++)
                        printk("%#x ", reg_value[i]);
                printk("\n");
        }

        /* 完成内存地址映射 */
        CCM_CCGR1 = ioremap(reg_value[0], reg_value[1]);
        SW_MUX_GPIO1_IO03 = ioremap(reg_value[2], reg_value[3]);
        SW_PAD_GPIO1_IO03 = ioremap(reg_value[4], reg_value[5]);
        GPIO1_DR = ioremap(reg_value[6], reg_value[7]);
        GPIO1_DIR = ioremap(reg_value[8], reg_value[9]);
#else
        CCM_CCGR1 = of_iomap(dtsled.nd, 0);
        SW_MUX_GPIO1_IO03 = of_iomap(dtsled.nd, 1);
        SW_PAD_GPIO1_IO03  = of_iomap(dtsled.nd, 2);
        GPIO1_DR = of_iomap(dtsled.nd, 3);
        GPIO1_DIR = of_iomap(dtsled.nd, 4);
#endif

        /* 完成初始化 */
        /* 配置时钟*/
        val = readl(CCM_CCGR1);
        val |= (3<<26);
        writel(val, CCM_CCGR1);

        /* 初始化IO复用 */
        writel(0x05, SW_MUX_GPIO1_IO03);

        /* 配置IO属性 */
        writel(0x10B0, SW_PAD_GPIO1_IO03);
        
        /* 配置为输出模式 */
        val = readl(GPIO1_DIR);
        val |= (1<<3);
        writel(val, GPIO1_DIR);

        led_switch(LED_OFF);

fail_read_array:
find_node_err:
        return ret;
}

static int dtsled_open(struct inode *inode, struct file *file)
{
        printk("led open!\n");
        file->private_data = &dtsled;

        return 0;
}

static ssize_t dtsled_write(struct file *file, const char __user *user, size_t size, loff_t *loff)
{
        int ret = 0;
        unsigned char buf[1];
        struct dtsled_dev *led_dev = file->private_data;

        ret = copy_from_user(buf, user, 1);
        if (ret < 0) {
                printk("kernel write failed!\n");
                ret = -EFAULT;
                goto error;
        }

        if((buf[0] != LED_OFF) && (buf[0] != LED_ON)) {
                ret = -1;
                goto error;
        }
        led_switch(buf[0]);

error:
        return ret;
}

static int dtsled_release(struct inode *inode, struct file *file)
{
        printk("led release!\n");
        file->private_data = NULL;
        return 0;
}


static int __init dtsled_init(void)
{
        int ret = 0;

        /* 1注册设备号 */
        if (dtsled.major == 0) {
                ret = alloc_chrdev_region(&dtsled.devid, 0, DTSLEDDEV_COUNT, DTSLEDDEV_NAME);
                dtsled.major = MAJOR(dtsled.devid);
                dtsled.minor = MINOR(dtsled.devid);
        } else {
                dtsled.devid = MKDEV(dtsled.major, 0);
                ret = register_chrdev_region(dtsled.devid, DTSLEDDEV_COUNT, DTSLEDDEV_NAME);
        }
        if (ret != 0) {
                printk("chrdev region error");
                goto fail_chrdev_region;
        }
        printk("devid major:%d minor:%d\n", dtsled.major, dtsled.minor);

        /* 2.注册字符设备 */
        dtsled.led_cdev.owner = THIS_MODULE;
        //dtsled.led_cdev.ops = &led_ops;
        cdev_init(&dtsled.led_cdev, &led_ops);
        ret = cdev_add(&dtsled.led_cdev, dtsled.devid, DTSLEDDEV_COUNT);
        if (ret != 0) {
                printk("cdev add error!\n");
                goto fail_cdev_add;
        }

        /* 3.自动创建设备节点 */
        /* 3.1 创建类 */
        dtsled.class = class_create(THIS_MODULE, DTSLEDDEV_NAME);
        if (IS_ERR(dtsled.class)) {
                ret = PTR_ERR(dtsled.class);
                goto class_create_error;
        }
        /* 3.2 创建设备 */
        dtsled.device = device_create(dtsled.class, NULL, dtsled.devid, NULL, DTSLEDDEV_NAME);
        if (IS_ERR(dtsled.device)) {
                ret = PTR_ERR(dtsled.device);
                goto device_create_error;
        };

        ret = led_gpio_init();
        if (ret < 0) {
                ret = -EINVAL;
                printk("led gpio init error!\n");
                goto led_init_err;
        }

        goto success;

led_init_err:
device_create_error:
        class_destroy(dtsled.class);
class_create_error:
fail_cdev_add:
        cdev_del(&dtsled.led_cdev);
        unregister_chrdev_region(dtsled.devid, DTSLEDDEV_COUNT);
fail_chrdev_region:
success:
        return ret;
}

static void __exit dtsled_exit(void)
{
        led_switch(LED_OFF);

        iounmap(CCM_CCGR1);
        iounmap(SW_MUX_GPIO1_IO03);
        iounmap(SW_PAD_GPIO1_IO03);
        iounmap(GPIO1_DR);
        iounmap(GPIO1_DIR);

        device_destroy(dtsled.class, dtsled.devid);
        class_destroy(dtsled.class);
        cdev_del(&dtsled.led_cdev);
        unregister_chrdev_region(dtsled.devid, DTSLEDDEV_COUNT);
}

module_init(dtsled_init);
module_exit(dtsled_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("wanglei");

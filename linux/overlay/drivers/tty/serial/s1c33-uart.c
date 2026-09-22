// SPDX-License-Identifier: GPL-2.0-only
/* Epson S1C33 asynchronous serial controller */
#include <linux/clk.h>
#include <linux/console.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/serial.h>
#include <linux/serial_core.h>
#include <linux/tty_flip.h>

#define S1C33_UART_TXD		0
#define S1C33_UART_RXD		1
#define S1C33_UART_STATUS	2
#define S1C33_UART_CTL		3
#define S1C33_UART_IRDA		4
#define S1C33_UART_BRTRUN	5
#define S1C33_UART_BRTRDL	6
#define S1C33_UART_BRTRDM	7

#define S1C33_UART_RX_READY	BIT(0)
#define S1C33_UART_TX_READY	BIT(1)
#define S1C33_UART_ERRORS	0x1c
#define S1C33_UART_TX_ENABLE	BIT(7)
#define S1C33_UART_RX_ENABLE	BIT(6)
#define S1C33_UART_ONE_STOP_BIT	BIT(3)
#define S1C33_UART_EIGHT_BIT_ASYNC 0x03
#define S1C33_UART_8N1		(S1C33_UART_RX_ENABLE | \
				 S1C33_UART_ONE_STOP_BIT | \
				 S1C33_UART_EIGHT_BIT_ASYNC)
#define S1C33_UART_DEFAULT_BAUD	115200
#define S1C33_UART_NR		2

struct s1c33_uart {
	struct uart_port port;
	int error_irq;
	u8 control;
};

static struct uart_port *s1c33_uart_ports[S1C33_UART_NR];

#ifdef CONFIG_SERIAL_S1C33_CONSOLE
static struct console s1c33_uart_console;
#endif

static struct uart_driver s1c33_uart_driver = {
	.owner		= THIS_MODULE,
	.driver_name	= "s1c33-uart",
	.dev_name	= "ttyC",
	.major		= 0,
	.minor		= 0,
	.nr		= S1C33_UART_NR,
#ifdef CONFIG_SERIAL_S1C33_CONSOLE
	.cons		= &s1c33_uart_console,
#endif
};

static void s1c33_uart_set_baud(struct uart_port *port, unsigned int baud)
{
	struct s1c33_uart *uart = container_of(port, struct s1c33_uart, port);
	unsigned long divisor;

	divisor = DIV_ROUND_CLOSEST(port->uartclk, baud * 16) - 1;
	writeb(uart->control, port->membase + S1C33_UART_CTL);
	writeb(0x10, port->membase + S1C33_UART_IRDA);
	writeb(0, port->membase + S1C33_UART_BRTRUN);
	writeb(divisor >> 8, port->membase + S1C33_UART_BRTRDM);
	writeb(divisor, port->membase + S1C33_UART_BRTRDL);
	writeb(1, port->membase + S1C33_UART_BRTRUN);
}

static void s1c33_uart_putchar(struct uart_port *port, unsigned char ch)
{
	while (!(readb(port->membase + S1C33_UART_STATUS) &
		 S1C33_UART_TX_READY))
		cpu_relax();
	writeb(ch, port->membase + S1C33_UART_TXD);
}

static unsigned int s1c33_uart_tx_empty(struct uart_port *port)
{
	return readb(port->membase + S1C33_UART_STATUS) &
		S1C33_UART_TX_READY ? TIOCSER_TEMT : 0;
}

static void s1c33_uart_set_mctrl(struct uart_port *port, unsigned int mctrl)
{
}

static unsigned int s1c33_uart_get_mctrl(struct uart_port *port)
{
	return TIOCM_CTS | TIOCM_DSR | TIOCM_CAR;
}

static void s1c33_uart_stop_tx(struct uart_port *port)
{
}

static void s1c33_uart_start_tx(struct uart_port *port)
{
	u8 ch;

	uart_port_tx(port, ch, true, s1c33_uart_putchar(port, ch));
}

static void s1c33_uart_stop_rx(struct uart_port *port)
{
}

static irqreturn_t s1c33_uart_rx_interrupt(int irq, void *data)
{
	struct uart_port *port = data;
	unsigned long flags;
	bool inserted = false;
	int limit = 16;
	u8 ch;

	uart_port_lock_irqsave(port, &flags);
	while ((readb(port->membase + S1C33_UART_STATUS) &
		S1C33_UART_RX_READY) && limit--) {
		ch = readb(port->membase + S1C33_UART_RXD);
		port->icount.rx++;
		if (!uart_handle_sysrq_char(port, ch)) {
			uart_insert_char(port, 0, 0, ch, TTY_NORMAL);
			inserted = true;
		}
	}
	if (inserted)
		tty_flip_buffer_push(&port->state->port);
	uart_port_unlock_irqrestore(port, flags);

	return IRQ_HANDLED;
}

static irqreturn_t s1c33_uart_error_interrupt(int irq, void *data)
{
	struct uart_port *port = data;
	int limit = 16;

	if (!(readb(port->membase + S1C33_UART_STATUS) &
	      S1C33_UART_ERRORS))
		return IRQ_NONE;
	while ((readb(port->membase + S1C33_UART_STATUS) &
		S1C33_UART_RX_READY) && limit--)
		readb(port->membase + S1C33_UART_RXD);
	writeb(0, port->membase + S1C33_UART_STATUS);
	return IRQ_HANDLED;
}

static int s1c33_uart_startup(struct uart_port *port)
{
	struct s1c33_uart *uart = container_of(port, struct s1c33_uart, port);
	static const char * const rx_names[] = {
		"s1c33-uart0-rx", "s1c33-uart1-rx",
	};
	int limit = 16;
	int ret;

	while ((readb(port->membase + S1C33_UART_STATUS) &
		S1C33_UART_RX_READY) && limit--)
		readb(port->membase + S1C33_UART_RXD);
	writeb(0, port->membase + S1C33_UART_STATUS);

	ret = request_irq(port->irq, s1c33_uart_rx_interrupt, 0,
			  rx_names[port->line], port);
	if (ret || uart->error_irq < 0)
		return ret;
	ret = request_irq(uart->error_irq, s1c33_uart_error_interrupt, 0,
			  "s1c33-uart1-error", port);
	if (ret)
		free_irq(port->irq, port);
	return ret;
}

static void s1c33_uart_shutdown(struct uart_port *port)
{
	struct s1c33_uart *uart = container_of(port, struct s1c33_uart, port);

	if (uart->error_irq >= 0)
		free_irq(uart->error_irq, port);
	free_irq(port->irq, port);
}

static void s1c33_uart_set_termios(struct uart_port *port,
				   struct ktermios *new,
				   const struct ktermios *old)
{
	unsigned long flags;
	unsigned int baud;

	new->c_cflag &= ~(CSIZE | CSTOPB | PARENB | PARODD | CRTSCTS);
	new->c_cflag |= CS8 | CLOCAL;
	baud = uart_get_baud_rate(port, new, old, 300, 230400);

	uart_port_lock_irqsave(port, &flags);
	s1c33_uart_set_baud(port, baud);
	uart_update_timeout(port, new->c_cflag, baud);
	uart_port_unlock_irqrestore(port, flags);
	tty_termios_encode_baud_rate(new, baud, baud);
}

static const char *s1c33_uart_type(struct uart_port *port)
{
	return "s1c33-uart";
}

static void s1c33_uart_config_port(struct uart_port *port, int flags)
{
	/* A non-zero type marks a configured port to serial_core. */
	port->type = 1;
}

static int s1c33_uart_verify_port(struct uart_port *port,
				  struct serial_struct *serial)
{
	return serial->type == PORT_UNKNOWN || serial->type == port->type ? 0 :
		-EINVAL;
}

static const struct uart_ops s1c33_uart_ops = {
	.tx_empty	= s1c33_uart_tx_empty,
	.set_mctrl	= s1c33_uart_set_mctrl,
	.get_mctrl	= s1c33_uart_get_mctrl,
	.stop_tx	= s1c33_uart_stop_tx,
	.start_tx	= s1c33_uart_start_tx,
	.stop_rx	= s1c33_uart_stop_rx,
	.startup	= s1c33_uart_startup,
	.shutdown	= s1c33_uart_shutdown,
	.set_termios	= s1c33_uart_set_termios,
	.type		= s1c33_uart_type,
	.config_port	= s1c33_uart_config_port,
	.verify_port	= s1c33_uart_verify_port,
};

static int s1c33_uart_probe(struct platform_device *pdev)
{
	struct s1c33_uart *uart;
	struct uart_port *port;
	struct resource *resource;
	struct clk *clk;
	unsigned long clock_rate;
	unsigned int baud;
	int line = pdev->id;
	int ret;

	if (line < 0 || line >= S1C33_UART_NR)
		return -EINVAL;
	clk = devm_clk_get_enabled(&pdev->dev, NULL);
	if (IS_ERR(clk))
		return dev_err_probe(&pdev->dev, PTR_ERR(clk),
				     "cannot enable input clock\n");
	clock_rate = clk_get_rate(clk);
	if (!clock_rate)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "input clock has no rate\n");
	ret = device_property_read_u32(&pdev->dev, "current-speed", &baud);
	if (ret)
		baud = S1C33_UART_DEFAULT_BAUD;
	if (!baud)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "invalid current-speed property\n");

	uart = devm_kzalloc(&pdev->dev, sizeof(*uart), GFP_KERNEL);
	if (!uart)
		return -ENOMEM;
	port = &uart->port;
	resource = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!resource)
		return -EINVAL;
	port->membase = devm_ioremap_resource(&pdev->dev, resource);
	if (IS_ERR(port->membase))
		return PTR_ERR(port->membase);
	ret = platform_get_irq_byname(pdev, "rx");
	if (ret < 0)
		return ret;
	uart->error_irq = platform_get_irq_byname_optional(pdev, "error");
	if (uart->error_irq == -EPROBE_DEFER)
		return -EPROBE_DEFER;
	uart->control = S1C33_UART_8N1;
	if (!device_property_read_bool(&pdev->dev, "epson,rx-only"))
		uart->control |= S1C33_UART_TX_ENABLE;

	port->dev = &pdev->dev;
	port->mapbase = resource->start;
	port->irq = ret;
	port->uartclk = clock_rate;
	port->iotype = UPIO_MEM;
	port->flags = UPF_BOOT_AUTOCONF | UPF_FIXED_PORT | UPF_FIXED_TYPE;
	port->ops = &s1c33_uart_ops;
	port->fifosize = 1;
	port->type = 1;
	port->line = line;
	spin_lock_init(&port->lock);
	s1c33_uart_set_baud(port, baud);

	/*
	 * A port whose firmware node says it can wake the system keeps its
	 * receiver armed through suspend; that is how a touch on the panel
	 * gets the machine back.
	 */
	device_init_wakeup(&pdev->dev,
			   device_property_read_bool(&pdev->dev,
						     "wakeup-source"));
	platform_set_drvdata(pdev, port);
	WRITE_ONCE(s1c33_uart_ports[line], port);
	ret = uart_add_one_port(&s1c33_uart_driver, port);
	if (ret) {
		WRITE_ONCE(s1c33_uart_ports[line], NULL);
		return ret;
	}
	if (!line) {
		dev_info(&pdev->dev,
			 "registered /dev/ttyC0 through serial_core\n");
	} else {
		dev_info(&pdev->dev,
			 "registered UART1 as a tty-backed serdev controller\n");
	}
	return 0;
}

static void s1c33_uart_remove(struct platform_device *pdev)
{
	struct uart_port *port = platform_get_drvdata(pdev);

	WRITE_ONCE(s1c33_uart_ports[port->line], NULL);
	uart_remove_one_port(&s1c33_uart_driver, port);
}

static int s1c33_uart_suspend(struct device *dev)
{
	struct uart_port *port = dev_get_drvdata(dev);

	if (device_may_wakeup(dev)) {
		int ret = enable_irq_wake(port->irq);

		dev_dbg(dev, "IRQ %d armed as a wake source: %d\n",
			port->irq, ret);
		return ret;
	}
	dev_dbg(dev, "suspending the port\n");
	return uart_suspend_port(&s1c33_uart_driver, port);
}

static int s1c33_uart_resume(struct device *dev)
{
	struct uart_port *port = dev_get_drvdata(dev);

	if (device_may_wakeup(dev))
		return disable_irq_wake(port->irq);
	return uart_resume_port(&s1c33_uart_driver, port);
}

static DEFINE_SIMPLE_DEV_PM_OPS(s1c33_uart_pm_ops, s1c33_uart_suspend,
				s1c33_uart_resume);

static struct platform_driver s1c33_uart_platform_driver = {
	.probe = s1c33_uart_probe,
	.remove = s1c33_uart_remove,
	.driver = {
		.name = "s1c33-uart",
		.pm = pm_sleep_ptr(&s1c33_uart_pm_ops),
	},
};

#ifdef CONFIG_SERIAL_S1C33_CONSOLE
static void s1c33_uart_console_write(struct console *console, const char *s,
				     unsigned int count)
{
	struct uart_port *port = READ_ONCE(s1c33_uart_ports[0]);
	unsigned long flags;

	if (!port)
		return;
	uart_port_lock_irqsave(port, &flags);
	uart_console_write(port, s, count, s1c33_uart_putchar);
	uart_port_unlock_irqrestore(port, flags);
}

static int s1c33_uart_console_setup(struct console *console, char *options)
{
	struct uart_port *port = READ_ONCE(s1c33_uart_ports[0]);
	int baud = S1C33_UART_DEFAULT_BAUD;
	int parity = 'n';
	int bits = 8;
	int flow = 'n';

	if (!port)
		return -ENODEV;
	if (options)
		uart_parse_options(options, &baud, &parity, &bits, &flow);
	return uart_set_options(port, console, baud, parity, bits, flow);
}

static struct console s1c33_uart_console = {
	.name	= "ttyC",
	.write	= s1c33_uart_console_write,
	.device	= uart_console_device,
	.setup	= s1c33_uart_console_setup,
	.flags	= CON_PRINTBUFFER,
	.index	= -1,
	.data	= &s1c33_uart_driver,
};

static int __init s1c33_uart_console_init(void)
{
	register_console(&s1c33_uart_console);
	return 0;
}
console_initcall(s1c33_uart_console_init);

/*
 * Before the platform device exists there is no port to lock or clock to
 * query, so the early console writes through whatever the loader left
 * programmed: "earlycon=s1c33,mmio,0x300b00" on the kernel command line.
 */
static void __init s1c33_uart_early_write(struct console *console,
					  const char *s, unsigned int count)
{
	struct earlycon_device *device = console->data;

	uart_console_write(&device->port, s, count, s1c33_uart_putchar);
}

static int __init s1c33_uart_early_setup(struct earlycon_device *device,
					 const char *options)
{
	if (!device->port.membase)
		return -ENODEV;
	device->con->write = s1c33_uart_early_write;
	return 0;
}
EARLYCON_DECLARE(s1c33, s1c33_uart_early_setup);
#endif

static int __init s1c33_uart_init(void)
{
	int ret;

	ret = uart_register_driver(&s1c33_uart_driver);
	if (ret)
		return ret;
	ret = platform_driver_register(&s1c33_uart_platform_driver);
	if (ret)
		uart_unregister_driver(&s1c33_uart_driver);
	return ret;
}
module_init(s1c33_uart_init);

static void __exit s1c33_uart_exit(void)
{
	platform_driver_unregister(&s1c33_uart_platform_driver);
	uart_unregister_driver(&s1c33_uart_driver);
}
module_exit(s1c33_uart_exit);

MODULE_DESCRIPTION("Epson S1C33 UART driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:s1c33-uart");

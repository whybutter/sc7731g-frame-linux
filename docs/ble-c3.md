# BLE on the VT10101 through an ESP32-C3 on its UART (plan, 2026-10-29)

Our choice for BLE (the WCN firmware is BR/EDR 2.1 only,
docs/bluetooth.md): an ESP32-C3 Super Mini wired to the
board as an HCI-over-UART controller (ESP-IDF controller_hci_uart, H4),
attached with btattach as an hci1 that BlueZ and its users see as they
would a USB dongle. Written as a plan; sections 5 and 6 say what was built.

## 1. Freeing ttyS1 (uart1, 0x70100000: the GND/TX/RX pads)

- Our DT's /chosen bootargs carry `console=ttyS1,115200n8 ... earlycon` and
  stdout-path = serial1. cboot appends only lcd_id, lcd_base, mem_cs,
  sysdump_magic and androidboot.serialno, never a console. Drop
  console=ttyS1, earlycon and stdout-path; keep console=tty1.
- Our users of the port: FRAMEBOOT/frame-fix.sh (a bring-up root shell on
  ttyS1: delete it) and initramfs/init's fallback shell when frameboot fails
  (on /dev/ttyS1: move it to tty1). The root filesystem's inittab has no
  ttyS getty.
- **The vendor SPL/U-Boot print on uart1 at every power-on and reset
  (115200), and U-Boot's bootdelay is 0: a byte in its RX FIFO at that
  moment stops autoboot.** The C3 must stay silent during the frame's boot:
  its HCI UART on non-console pins (its ROM log goes to U0TXD/GPIO21, left
  unconnected), the IDF console on USB-Serial-JTAG, and ideally held in
  reset until Linux releases it (section 3). U-Boot's banner reaches the
  C3's RX as noise; the C3 is reset before btattach anyway.
- Baud: uart1 has no clocks in our DT, so sprd_serial runs it from its 26
  MHz default with an integer divisor (26 MHz / baud). **1,000,000 is exact
  on both sides** (26M/26; the C3's 80 MHz/80); 2,000,000 too (26M/13,
  80M/40). 1 Mbaud without flow control has margin for BLE scan and GATT.
  Faster needs a clocks entry for uart1 (a 96 MHz parent exists).
- RTS/CTS: not on the 3-pin header; whether U1CTS/U1RTS reach any pad is
  unknown. Plan without them.
- Cost: the UART console is gone. Recovery stays kernel-trial.sh + a power
  cycle; a U-Boot catch means unplugging the C3's TX/RX -- a 2-pin connector
  or jumper on that link keeps that easy.

## 2. Power (the C3's 5V pin; BLE peaks ~100-150 mA)

Nothing in the vendor dumps names these; to meter with the frame on,
against the UART's GND pad:
- the two oval through-holes at the top edge by the wide pour: most likely
  the DC input, ~5 V -- the preferred feed;
- V-IR on the empty IR footprint: 3.3 V or 5 V, and whether it sags under a
  ~150 mA load (a PIR feed may be resistor-limited);
- the 3-pin header top right: unknown, all three pins;
- not USB VBUS: 5 V only while the port is host (boost on GPIO 69).

## 3. IR-IN as the C3's reset

Almost certainly the vendor's presence line, GPIO 217 = pad IIS1CLK = sysfs
729 (gpiochip512), which reads 1 with nothing fitted (pulled up). Confirm by
shorting IR-IN to GND briefly while gpio729 is read: it must go to 0. Its
pad voltage is unknown (meter it idle: ~1.8 V or ~3.3 V), so connect it
through a **Schottky diode, anode on the C3's EN, cathode on IR-IN**: GPIO
low pulls EN to ~0.3 V (reset); GPIO high or input leaves the diode off and
the C3's own EN pull-up releases it, without the pad ever seeing 3.3 V.
(A 2N7002 with its gate on IR-IN works too.) Whether the pad drives out at
all is a quick test.

## 4. Software

- **Kernel:** CONFIG_BT_HCIUART + BT_HCIUART_H4; the bootargs,
  stdout-path and initramfs fallback changes above; one kernel trial.
- **`sc7731g-bt-ext` (rootfs/etc/init.d):** gpio 729 low 100 ms, released,
  ~400 ms for the C3 to boot; `btattach -B /dev/ttyS1 -P h4 -S 1000000`
  under supervise-daemon; after sc7731g-bt, before bluetooth; board vars
  (BT_EXT_UART, BT_EXT_RESET_GPIO, BT_EXT_BAUD) in /etc/conf.d/sc7731g.
- **Coexistence:** the WCN (BR/EDR, classic profiles) and the C3 (LE only)
  are two adapters BlueZ runs side by side. Their indexes aren't stable, so
  BLE software should pick the LE-capable adapter by address or capability,
  never by hciN.
- **C3 firmware (ESP-IDF 5.x):** bluetooth/hci/controller_hci_uart_esp32c3_
  and_esp32s3; H4 on UART1, TX GPIO5, RX GPIO6 (avoiding strapping 2/8/9,
  UART0's 20/21, USB 18/19); CONFIG_BT_CTRL_HCI_UART_BAUDRATE=1000000, no
  flow control, console on USB-Serial-JTAG.
- Wiring: frame TX pad -> C3 GPIO6; frame RX pad <- C3 GPIO5; GND-GND;
  5 V -> C3 5V; IR-IN -> diode -> C3 EN.

## 5. Built (2026-10-29)

- **C3 firmware: `c3-hci/`** (ESP-IDF 5.5.5). It is IDF's controller_hci_uart example with H4
  on UART1, TX GPIO5 / RX GPIO6, 1 Mbaud and no flow control, plus a
  **silent start**:
  - every log goes to USB-Serial-JTAG;
  - GPIO5 stays an undriven, pulled-up input;
  - UART1 listens RX-only, dropping U-Boot's banner and any noise, until
    the host's first `HCI_Reset`;
  - TX then goes live (high first), the controller starts, and the caught
    reset is replayed into it.

  The example drove TX low as a GPIO before routing it, which would itself
  be a byte at U-Boot.
- **Bench test: `c3-hci/h4bench.py`** (pyserial, a 1-Mbaud-exact
  adapter). It checks, in order:
  - silence across a reset;
  - silence after 115200 U-Boot-like noise;
  - Reset / Version / BD_ADDR;
  - an LE scan.

  **Not run yet:** it needs the adapter and four wires.
- **`rootfs/etc/init.d/sc7731g-bt-ext`**, with `BT_EXT=0` in
  /etc/conf.d/sc7731g for now.
  - It resets the C3 through gpio 729 (low 100 ms, then input), waits
    400 ms, then runs `btattach -B /dev/ttyS1 -P h4 -S 1000000` under
    supervise-daemon.
  - It runs after sc7731g-bt and before bluetooth.
  - **It stays out while `console=ttyS1` is on the kernel command line**,
    so it can't take the console before the bootargs change.
  - Checked on the frame (read-only): the guard sees the console on ttyS1;
    gpio729 exports as an input reading 1, as the plan predicted; btattach
    is in Alpine's bluez 5.82.
- **BLE software** that hard-codes `/org/bluez/hci0` gets the WCN's BR/EDR
  adapter on this board. It has to pick the LE adapter instead: the
  `org.bluez.Adapter1` that has a `Roles` property (BlueZ sets it only on
  LE-capable adapters), or one chosen by `Address`.

## 6. State (2026-10-29)

The kernel side (CONFIG_BT_HCIUART + H4) and the ttyS1 console removal went
on together as kernel #49, so uart1 is free for the C3 in the kernel built
here. An earlier plan kept the console until the C3 was soldered on; the C3
was then put off for later, and the ways back without a console are a
uboot-cmd.py debug boot (with its own console=ttyS1) or a one-off
kernel-trial of an older image. Not done yet: the bench test, metering the
power points, and the soldering.

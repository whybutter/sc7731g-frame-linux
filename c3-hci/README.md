# c3-hci: the ESP32-C3 as the VT10101's BLE controller

ESP-IDF 5.5 firmware for an ESP32-C3 Super Mini wired to the frame's uart1
pads: HCI H4 on UART1 (TX GPIO5, RX GPIO6), 1 Mbaud, no flow control. The
frame attaches it with btattach (`rootfs/etc/init.d/sc7731g-bt-ext`).
The plan is `../docs/ble-c3.md`. Built from IDF's
`examples/bluetooth/hci/controller_hci_uart_esp32c3_and_esp32s3`.

## Why it differs from the example: it stays silent until the host speaks

The frame's vendor U-Boot uses that same UART at every boot with bootdelay 0,
so one byte into its RX stops autoboot. The C3 boots while U-Boot runs and
hears U-Boot's 115200 banner as noise. So:

- **Logs:** every log (ROM, bootloader, app) goes to USB-Serial-JTAG. No UART
  console exists (`CONFIG_ESP_CONSOLE_UART_NUM=-1`).
- **TX pin:** GPIO5 stays a pulled-up input (undriven) until the host's first
  `HCI_Reset` (`01 03 0c 00`) arrives. The example made it a GPIO output,
  low, first.
- **Before that reset:** UART1 runs RX-only and drops everything it hears.
  The USB log counts what it dropped.
- **After it:**
  1. TX goes high as a GPIO;
  2. it is handed to the configured UART;
  3. the controller starts;
  4. the caught reset is replayed into it, so the controller answers the
     host's reset itself.
- **Reset:** resetting the C3 (the frame does it through GPIO 217, sysfs 729) starts
  the whole thing over.

## Build and flash

With ESP-IDF 5.5 installed and its environment loaded (`. $IDF_PATH/export.sh`):

    idf.py set-target esp32c3    # once
    idf.py build
    idf.py -p /dev/ttyACM0 flash monitor     # macOS: /dev/cu.usbmodem*

## Bench test, before soldering

The C3 will talk to the frame over its uart1 pads, which are the frame's
debug console until the C3 is fitted and the kernel's console moves off
ttyS1. A USB-serial adapter on a computer plays the frame's part exactly:
the same two wires at the same 1 Mbaud. It proves the two things that would
hurt on the real frame:

- **Silence at boot.** If the C3 sends anything while the frame boots,
  U-Boot stops at its prompt instead of starting Linux. On the bench that is
  a failed test line; on the frame, recovering means pulling the C3's wires.
- **The controller works over the UART.** It answers HCI and sees
  advertisers, so the soldering happens only once the firmware is known good.

The adapter must really do 1,000,000 baud: CH340, FT232R, FT231X and
CP2102N do; the old CP2102 (not N) rounds to 921600 and fails. The CH340
cable used for the frame's UART console works.

1. Wire the adapter to the C3: adapter **TX** → C3 **GPIO6**, adapter **RX**
   ← C3 **GPIO5**, **GND** – **GND**. Leave the adapter's 3V3/5V pin
   unconnected: the C3 is powered from its own USB.
2. Plug the C3 in by USB-C; that cable flashes it, powers it and carries
   its log. Flash c3-hci (above).
3. Run `python3 h4bench.py <adapter port> 15 [aa:bb:cc:dd:ee:ff]` (pyserial;
   the optional address is any BLE device of yours that should show up in
   the scan). Press the C3's RESET when it says so. It checks, in order:
   1. silence across a reset;
   2. silence after a U-Boot-like banner at 115200;
   3. HCI Reset, Read_Local_Version and Read_BD_ADDR at 1 Mbaud;
   4. an LE scan.

   `PASS` means the firmware is ready for the frame.

Without an adapter, flash the C3 and read only its USB log
(`idf.py monitor`): it should print `silent: waiting for the host's
HCI_Reset ...` and nothing else. That proves it boots and waits, not that the
wire stays quiet or that HCI works over the UART. A 2-pin connector on the
TX/RX link keeps it easy to pull if the first boot with it fitted fails.

## On the frame

1. Meter the pads and solder the C3 (power, reset and wiring:
   `../docs/ble-c3.md` §2-3).
2. Move the kernel's console off ttyS1 (`console=` in the DT's bootargs).
3. Enable `rootfs/etc/init.d/sc7731g-bt-ext`: it resets the C3 and runs
   btattach, and the C3 appears as a second HCI adapter, the LE one.

## Status

The firmware builds and its USB log shows the silent wait. The bench test
and the fitted C3 have not been run yet.

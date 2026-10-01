"""Wireless uploader for env:uno_bluetooth -- see HC05_BRINGUP.md.

Talks to the Uno's Optiboot bootloader over the HC-05 directly instead of
through avrdude. avrdude proved unreliable over Bluetooth here (most uploads
died part-way with the bootloader resetting), while this client, sending the
same STK500 commands, read the full flash cleanly on every fresh connection
tested. It also retries a failed page instead of abandoning the upload
half-written.

Usage (PlatformIO runs this as the upload command):
    python bt_upload.py /dev/rfcomm0 firmware.hex
"""
import os
import re
import subprocess
import sys
import time

import serial

PAGE_SIZE = 128                  # ATmega328P flash page, bytes
SIGNATURE = b"\x1e\x95\x0f"      # ATmega328P

# STK500v1 bytes Optiboot understands
SYNC_CRC_EOP = 0x20
INSYNC, OK = 0x14, 0x10
GET_SYNC, LOAD_ADDRESS, PROG_PAGE, READ_PAGE, READ_SIGN, LEAVE_PROGMODE = (
    0x30, 0x55, 0x64, 0x74, 0x75, 0x51)
SYNC = bytes([GET_SYNC, SYNC_CRC_EOP])

LINK_TIMEOUT_S = 10
BOOTLOADER_START_S = 0.6     # reset + Optiboot's LED flashes (measured ~0.45 s)
RESET_BUTTON_TIMEOUT_S = 20
REPLY_TIMEOUT_S = 1.0
MAX_RECOVERIES = 5


class BootloaderLost(Exception):
    pass


def read_hex(path):
    """Intel HEX file -> flash image bytes, padded to whole pages with 0xFF."""
    image = bytearray()
    base = 0
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line.startswith(":"):
                continue
            rec = bytes.fromhex(line[1:])
            count, addr, rtype, data = rec[0], (rec[1] << 8) | rec[2], rec[3], rec[4:4 + rec[0]]
            if rtype == 0x00:
                start = base + addr
                if len(image) < start + count:
                    image.extend(b"\xff" * (start + count - len(image)))
                image[start:start + count] = data
            elif rtype == 0x02:
                base = ((data[0] << 8) | data[1]) << 4
            elif rtype == 0x04:
                base = ((data[0] << 8) | data[1]) << 16
            elif rtype == 0x01:
                break
    if len(image) % PAGE_SIZE:
        image.extend(b"\xff" * (PAGE_SIZE - len(image) % PAGE_SIZE))
    return bytes(image)


def rfcomm_connected(port):
    try:
        out = subprocess.run(["rfcomm", "show", os.path.basename(port)],
                             capture_output=True, text=True).stdout
    except FileNotFoundError:
        return None  # no rfcomm tool -- can't tell, fall back to a fixed wait
    return re.search(r"\bconnected\b", out) is not None


def wait_for_link(port):
    # opening /dev/rfcomm0 only starts the Bluetooth connection (~3 s)
    deadline = time.time() + LINK_TIMEOUT_S
    while time.time() < deadline:
        state = rfcomm_connected(port)
        if state is None:
            time.sleep(5)
            return True
        if state:
            time.sleep(0.5)  # let the link settle
            return True
        time.sleep(0.2)
    return False


class Bootloader:
    def __init__(self, link):
        self.link = link

    def _read(self, n):
        buf = b""
        deadline = time.time() + REPLY_TIMEOUT_S
        while len(buf) < n and time.time() < deadline:
            buf += self.link.read(n - len(buf))
            if len(buf) < n:
                time.sleep(0.002)
        return buf

    def command(self, cmd, reply_len=0):
        """Send one STK500 command; return its reply payload."""
        self.link.write(bytes(cmd) + bytes([SYNC_CRC_EOP]))
        r = self._read(reply_len + 2)
        if len(r) != reply_len + 2 or r[0] != INSYNC or r[-1] != OK:
            raise BootloaderLost()
        return r[1:-1]

    def _answering(self, attempts, gap):
        self.link.reset_input_buffer()
        replies = b""
        for _ in range(attempts):
            self.link.write(SYNC)
            time.sleep(gap)
            replies += self.link.read(64)
            if bytes([INSYNC, OK]) in replies:
                time.sleep(0.1)
                self.link.reset_input_buffer()  # drop any late duplicate replies
                return True
        return False

    def enter(self):
        """Reset into the bootloader via the sketch's OTA listener, falling
        back to asking for the RESET button if there's no working listener."""
        self.link.reset_input_buffer()
        self.link.write(SYNC)  # sketch's OTA listener resets the board
        time.sleep(BOOTLOADER_START_S)
        if self._answering(3, 0.15):
            return True
        print("The board didn't respond to the reset trigger -- the sketch "
              "on it may not have a working OTA listener.")
        print("*** Press the Uno's RESET button now (waiting %d s) ***"
              % RESET_BUTTON_TIMEOUT_S, flush=True)
        # syncs spaced wider than Optiboot's startup so no more than one pair
        # piles up in its UART while it starts -- an overflow there makes it
        # give up and jump straight to the sketch
        return self._answering(int(RESET_BUTTON_TIMEOUT_S / 0.5), 0.5)

    def write_page(self, addr, data):
        word = addr // 2
        self.command([LOAD_ADDRESS, word & 0xff, word >> 8])
        self.command([PROG_PAGE, 0, len(data), ord("F")] + list(data))

    def read_page(self, addr):
        word = addr // 2
        self.command([LOAD_ADDRESS, word & 0xff, word >> 8])
        return self.command([READ_PAGE, 0, PAGE_SIZE, ord("F")], PAGE_SIZE)


def progress(label, done, total):
    bar = "#" * (50 * done // total)
    print("\r%-8s | %-50s | %3d%%" % (label, bar, 100 * done // total),
          end="", flush=True)
    if done == total:
        print()


def run_pages(bl, label, pages, action):
    """Run action(page_index) over every page, re-entering the bootloader and
    resuming from the failed page if the link or bootloader drops out."""
    recoveries = 0
    i = 0
    while i < len(pages):
        try:
            action(i)
            i += 1
            progress(label, i, len(pages))
        except BootloaderLost:
            recoveries += 1
            print("\nLost the bootloader at 0x%04x -- recovering (%d/%d)..."
                  % (pages[i], recoveries, MAX_RECOVERIES))
            if recoveries > MAX_RECOVERIES or not bl.enter():
                raise


def main():
    port, hex_path = sys.argv[1], sys.argv[2]
    image = read_hex(hex_path)
    pages = list(range(0, len(image), PAGE_SIZE))
    print("Uploading %s (%d bytes) over %s" % (hex_path, len(image), port))

    link = serial.Serial(port, 115200, timeout=0)  # raw mode, no echo
    if not wait_for_link(port):
        sys.exit("Bluetooth link didn't come up after %d s -- is the pedal "
                 "powered and in range?" % LINK_TIMEOUT_S)
    bl = Bootloader(link)
    if not bl.enter():
        sys.exit("Bootloader never answered.")

    try:
        sig = bl.command([READ_SIGN], 3)
        if sig != SIGNATURE:
            sys.exit("Unexpected device signature %s (expected ATmega328P %s)"
                     % (sig.hex(), SIGNATURE.hex()))

        run_pages(bl, "Writing", pages,
                  lambda i: bl.write_page(pages[i], image[pages[i]:pages[i] + PAGE_SIZE]))

        def verify(i):
            addr = pages[i]
            want = image[addr:addr + PAGE_SIZE]
            if bl.read_page(addr) != want:
                print("\nMismatch at 0x%04x -- rewriting" % addr)
                bl.write_page(addr, want)
                if bl.read_page(addr) != want:
                    sys.exit("\nVerify failed at 0x%04x" % addr)
        run_pages(bl, "Verifying", pages, verify)

        bl.command([LEAVE_PROGMODE])  # bootloader starts the new sketch
    except BootloaderLost:
        sys.exit("\nUpload failed: lost contact with the bootloader. If the "
                 "pedal no longer responds, run the upload again and press "
                 "RESET when asked.")
    print("%d bytes written and verified." % len(image))


if __name__ == "__main__":
    main()

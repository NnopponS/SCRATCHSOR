"""Flash existing verified build images in one session; preserve both NVS partitions."""
import os
from pathlib import Path
import subprocess
import sys

from usb_dashboard import select_port


def flash_command(root, core, port):
    build = root / '.pio/build/esp32dev'
    packages = core / 'packages'
    esptool = packages / 'tool-esptoolpy/esptool.py'
    images = [
        ('0x1000', build / 'bootloader.bin'),
        ('0x8000', build / 'partitions.bin'),
        ('0xe000', packages / 'framework-arduinoespressif32/tools/partitions/boot_app0.bin'),
        ('0x10000', build / 'firmware.bin'),
        ('0x290000', build / 'littlefs.bin'),
    ]
    for file in [esptool, *(path for _, path in images)]:
        if not file.is_file() or not file.stat().st_size:
            raise FileNotFoundError(f'Missing or empty file: {file}. Build firmware and LittleFS first.')
    return [sys.executable, str(esptool), '--chip', 'esp32', '--port', port,
            '--baud', '460800', '--before', 'default_reset', '--after', 'hard_reset',
            'write_flash', '-z', '--flash_mode', 'dio', '--flash_freq', '40m',
            '--flash_size', '4MB', *[str(value) for pair in images for value in pair]]


def main():
    try:
        from serial.tools.list_ports import comports
        if len(sys.argv) > 2:
            raise ValueError('Usage: Flash Board.bat [COM3]')
        detected = list(comports())
        ports = [p.device for p in detected]
        explicit = sys.argv[1].upper() if len(sys.argv) == 2 else None
        if explicit and explicit not in ports:
            raise ValueError(f'{explicit} is not connected. Detected: {ports}')
        usb_ports = [p.device for p in detected if p.vid is not None]
        port = select_port(explicit, usb_ports if usb_ports else ports)
        root = Path(__file__).resolve().parents[1]
        core = Path(os.environ.get('PLATFORMIO_CORE_DIR', Path.home() / '.platformio'))
        command = flash_command(root, core, port)
        print(f'Flashing {port}. Please wait for all hash verifications to finish.', flush=True)
        return subprocess.run(command, cwd=root).returncode
    except (ImportError, OSError, ValueError) as error:
        print(f'Cannot flash: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())

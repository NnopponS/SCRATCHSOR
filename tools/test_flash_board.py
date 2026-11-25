from pathlib import Path
import tempfile
import unittest

from flash_board import flash_command


class FlashTests(unittest.TestCase):
    def test_command_requires_every_image_and_flashes_once_with_automatic_reset(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            core = root / 'platformio'
            with self.assertRaises(FileNotFoundError):
                flash_command(root, core, 'COM3')
            files = [core / 'packages/tool-esptoolpy/esptool.py',
                     core / 'packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin']
            files += [root / '.pio/build/esp32dev' / name for name in
                      ['bootloader.bin', 'partitions.bin', 'firmware.bin', 'littlefs.bin']]
            for file in files:
                file.parent.mkdir(parents=True, exist_ok=True)
                file.write_bytes(b'test')
            command = flash_command(root, core, 'COM3')
            self.assertEqual(command[command.index('--port') + 1], 'COM3')
            self.assertEqual(command[command.index('--before') + 1], 'default_reset')
            self.assertEqual(command.count('write_flash'), 1)
            self.assertNotIn('erase_flash', command)
            for offset in ['0x1000', '0x8000', '0xe000', '0x10000', '0x290000']:
                self.assertIn(offset, command)
            self.assertNotIn('0x9000', command)  # settings NVS preserved
            self.assertNotIn('0x150000', command)  # fingerprint NVS preserved


if __name__ == '__main__':
    unittest.main()

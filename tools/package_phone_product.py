"""Bundle the existing phone firmware build without touching recordings or NVS."""
import hashlib
import json
import os
import re
from pathlib import Path
import zipfile

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / '.pio/build/esp32dev'
CORE = Path(os.environ.get('PLATFORMIO_CORE_DIR', Path.home() / '.platformio'))
IMAGES = {
    'bootloader.bin': ('0x1000', BUILD / 'bootloader.bin'),
    'partitions.bin': ('0x8000', BUILD / 'partitions.bin'),
    'boot_app0.bin': ('0xe000', CORE / 'packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin'),
    'firmware.bin': ('0x10000', BUILD / 'firmware.bin'),
    'littlefs.bin': ('0x290000', BUILD / 'littlefs.bin'),
}
FLASH = '''"""Run from this extracted bundle: python flash.py COM7. Automatic USB reset."""
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
if len(sys.argv) != 2 or not re.fullmatch(r'COM[0-9]+', sys.argv[1], re.I):
    sys.exit('Usage: python flash.py COM7 (use the actual board port)')
root = Path(__file__).resolve().parent
core = Path(os.environ.get('PLATFORMIO_CORE_DIR', Path.home() / '.platformio'))
tool = core / 'packages/tool-esptoolpy/esptool.py'
if not tool.is_file():
    sys.exit('PlatformIO esptool is missing. Install/build the project with PlatformIO first.')
images = [('0x1000','bootloader.bin'),('0x8000','partitions.bin'),('0xe000','boot_app0.bin'),('0x10000','firmware.bin'),('0x290000','littlefs.bin')]
manifest = json.loads((root/'manifest.json').read_text())
for _, name in images:
    if not (root / name).is_file(): sys.exit('Missing bundle file: '+name)
    if hashlib.sha256((root/name).read_bytes()).hexdigest() != manifest['files'][name]['sha256']:
        sys.exit('Bundle checksum mismatch: '+name)
cmd = [sys.executable,str(tool),'--chip','esp32','--port',sys.argv[1],'--baud','460800','--before','default_reset','--after','hard_reset','write_flash','-z','--flash_mode','dio','--flash_freq','40m','--flash_size','4MB']
for address, name in images: cmd.extend([address,str(root/name)])
sys.exit(subprocess.run(cmd,cwd=root).returncode)
'''


def main():
    version = re.search(r'kFirmwareVer\s*=\s*"([^"]+)"', (ROOT / 'include/config.h').read_text()).group(1)
    manifest = {'firmware': version, 'nvs_preserved': True, 'files': {}}
    for name, (address, path) in IMAGES.items():
        data = path.read_bytes()
        if not data:
            raise ValueError(f'Empty build image: {path}')
        manifest['files'][name] = {'address': address, 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
    target = ROOT / f'releases/phone-v{version.split("-")[0]}.zip'
    target.parent.mkdir(exist_ok=True)
    with zipfile.ZipFile(target, 'w', zipfile.ZIP_DEFLATED) as bundle:
        for name, (_, path) in IMAGES.items():
            bundle.write(path, name)
        bundle.writestr('manifest.json', json.dumps(manifest, indent=2) + '\n')
        bundle.writestr('flash.py', FLASH)
        bundle.write(ROOT / 'docs/phone-product.md', 'phone-product.md')
        bundle.writestr('INSTALL.txt', 'Extract all files. Connect USB and close serial monitors. Reset and restart are automatic.\nRun: python flash.py COM7 (replace COM7 with the board port).\nNo erase-all operation is used; both NVS partitions are preserved.\nJoin Wi-Fi shown on OLED, then open http://192.168.4.1 in normal Safari/Chrome and tap Connect. No password entry inside the app.\nPatient records save on the phone. Export regularly. See phone-product.md.\nThis bundle has compiled successfully; fresh-session range accuracy and real-phone measurements need further validation.\n')
    (ROOT / '.pio/phone-build-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'Ready: {target} ({target.stat().st_size} bytes)')


if __name__ == '__main__':
    main()

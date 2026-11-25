"""Generate an OLED QR with a lowercase HTTP URI and a full quiet zone."""
from pathlib import Path
import qrcode

URL = 'http://192.168.4.1'


def main():
    qr = qrcode.QRCode(version=1, error_correction=qrcode.constants.ERROR_CORRECT_L, border=4)
    # Mixed byte/alphanumeric segments keep the lowercase URI inside version 1.
    qr.add_data('http', optimize=0)
    qr.add_data('://192.168.4.1', optimize=0)
    qr.make(fit=False)
    matrix = qr.get_matrix()
    assert len(matrix) == 29
    packed = [sum(int(row[x]) << (x % 8) for x in range(start, min(start+8,29)))
              for row in matrix for start in range(0,29,8)]
    target = Path(__file__).resolve().parents[1] / 'include/product/app_qr.h'
    target.write_text('#pragma once\n// Exact decoded URI: '+URL+'\nstatic constexpr unsigned char appQr[]={'+','.join(map(str,packed))+'};\n')
    print(f'Generated {URL} in a 29-module QR including quiet zone')


if __name__ == '__main__':
    main()

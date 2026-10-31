"""PC から Core2 にナースコールの呼び出しを送る (StickC と同じ UDP 電文)

使い方:
    py pc_call.py --ip 192.168.1.50
    py pc_call.py --ip 192.168.1.50 --name "リビング"
    py pc_call.py                      # IP 省略時はブロードキャスト
"""
import argparse
import random
import socket
import struct
import sys
import time

NC_MAGIC = 0x4E43414C  # "NCAL"
NC_CALL, NC_ACK = 1, 2
# uint32 magic, uint8 type, uint8 battery, uint16 seq, uint8[6] mac, char[32] name (リトルエンディアン, packed)
FMT = "<IBBH6s32s"
PC_MAC = b"\x02PCTST"  # PC からの送信を識別するためのダミー MAC


def encode_name(name: str) -> bytes:
    # 31 バイトに収まるよう UTF-8 の文字境界で切る (末尾は NUL)
    raw = name.encode("utf-8")[:31]
    return raw.decode("utf-8", errors="ignore").encode("utf-8")


def main() -> int:
    ap = argparse.ArgumentParser(description="PC から Core2 にナースコールを送る")
    ap.add_argument("--ip", default="255.255.255.255", help="Core2 の IP (省略時ブロードキャスト)")
    ap.add_argument("--port", type=int, default=50000)
    ap.add_argument("--name", default="PC", help="Core2 に表示される名前")
    ap.add_argument("--retries", type=int, default=10)
    args = ap.parse_args()

    seq = random.randint(1, 0xFFFF)  # 毎回変えて Core2 の重複判定に引っかからないようにする
    msg = struct.pack(FMT, NC_MAGIC, NC_CALL, 255, seq, PC_MAC, encode_name(args.name))

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    sock.settimeout(0.3)

    for i in range(args.retries):
        sock.sendto(msg, (args.ip, args.port))
        deadline = time.monotonic() + 0.3
        while time.monotonic() < deadline:
            try:
                data, addr = sock.recvfrom(256)
            except socket.timeout:
                break
            if len(data) != struct.calcsize(FMT):
                continue
            magic, typ, _, ack_seq, _, _ = struct.unpack(FMT, data)
            if magic == NC_MAGIC and typ == NC_ACK and ack_seq == seq:
                print(f"呼び出しました (Core2 {addr[0]} が受信, {i + 1}回目)")
                return 0
    print("届きません: Core2 の電源・IP・Wi-Fi を確認してください", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())

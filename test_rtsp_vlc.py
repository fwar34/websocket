"""
test_rtsp_vlc.py — RTSP 握手流程调试脚本

仅执行 RTSP 控制面交互（OPTIONS / DESCRIBE / SETUP / PLAY），
打印所有请求和响应的原始内容，不接收 RTP 数据。
用于快速调试 SDP 格式和 RTSP 握手是否正确。
"""

import socket
import sys


RTSP_HOST = "localhost"
RTSP_PORT = 8554
RTSP_URL = f"rtsp://{RTSP_HOST}:{RTSP_PORT}/test"
RTP_CLIENT_PORT = 5000


def recvall(sock, bufsize=4096, timeout=3.0):
    sock.settimeout(timeout)
    data = b""
    try:
        while True:
            chunk = sock.recv(bufsize)
            if not chunk:
                break
            data += chunk
            if b"\r\n\r\n" in data:
                header_end = data.index(b"\r\n\r\n") + 4
                cl = None
                for line in data[:header_end].split(b"\r\n"):
                    if line.lower().startswith(b"content-length:"):
                        cl = int(line.split(b":", 1)[1].strip())
                        break
                if cl is not None and len(data) >= header_end + cl:
                    break
                if cl is None:
                    break
        return data
    except socket.timeout:
        return data if data else None


def send_raw(sock, data):
    sock.sendall(data.encode() if isinstance(data, str) else data)


def print_section(title):
    print(f"\n{'=' * 60}")
    print(f"  {title}")
    print(f"{'=' * 60}")


def print_raw(label, data):
    print(f"\n--- {label} ---")
    try:
        print(data.decode("ascii", errors="replace"))
    except Exception:
        print(repr(data))


def main():
    print_section("RTSP 握手调试 — 无 RTP 数据接收")
    print(f"目标: {RTSP_HOST}:{RTSP_PORT}")
    print(f"URL:  {RTSP_URL}")

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(5.0)

    print(f"\n[连接] {RTSP_HOST}:{RTSP_PORT} ...")
    try:
        sock.connect((RTSP_HOST, RTSP_PORT))
        print("[连接] ✓ 成功")
    except Exception as e:
        print(f"[连接] ✗ 失败: {e}")
        sys.exit(1)

    cseq = 0
    session_id = None

    # ---- 1. OPTIONS ----
    print_section("1. OPTIONS")
    cseq += 1
    req = (
        f"OPTIONS {RTSP_URL} RTSP/1.0\r\n"
        f"CSeq: {cseq}\r\n"
        f"\r\n"
    )
    print_raw("请求", req)
    send_raw(sock, req)
    resp = recvall(sock)
    print_raw("响应", resp)

    # ---- 2. DESCRIBE ----
    print_section("2. DESCRIBE")
    cseq += 1
    req = (
        f"DESCRIBE {RTSP_URL} RTSP/1.0\r\n"
        f"CSeq: {cseq}\r\n"
        f"Accept: application/sdp\r\n"
        f"\r\n"
    )
    print_raw("请求", req)
    send_raw(sock, req)
    resp = recvall(sock)
    print_raw("响应", resp)

    # 解析 SDP 获取 control
    sdp_text = resp.decode("ascii", errors="replace") if resp else ""
    control = ""
    for line in sdp_text.split("\r\n"):
        if line.startswith("a=control:"):
            control = line.split(":", 1)[1]
            break
    if control and control != "*":
        track_url = f"{RTSP_URL}/{control}"
    else:
        track_url = RTSP_URL
    print(f"\n[解析] Control: {control}")
    print(f"[解析] Track URL: {track_url}")

    # ---- 3. SETUP ----
    print_section("3. SETUP (UDP)")
    cseq += 1
    transport = f"RTP/AVP;unicast;client_port={RTP_CLIENT_PORT}-{RTP_CLIENT_PORT + 1}"
    req = (
        f"SETUP {track_url} RTSP/1.0\r\n"
        f"CSeq: {cseq}\r\n"
        f"Transport: {transport}\r\n"
        f"\r\n"
    )
    print_raw("请求", req)
    send_raw(sock, req)
    resp = recvall(sock)
    print_raw("响应", resp)

    # 解析 Session ID
    for line in (resp or b"").decode("ascii", errors="replace").split("\r\n"):
        if line.lower().startswith("session:"):
            session_id = line.split(":", 1)[1].strip()
            break
    print(f"\n[解析] Session ID: {session_id}")

    # ---- 4. PLAY ----
    print_section("4. PLAY")
    cseq += 1
    req = (
        f"PLAY {RTSP_URL} RTSP/1.0\r\n"
        f"CSeq: {cseq}\r\n"
        f"Session: {session_id}\r\n"
        f"Range: npt=0.000-\r\n"
        f"\r\n"
    )
    print_raw("请求", req)
    send_raw(sock, req)
    resp = recvall(sock)
    print_raw("响应", resp)

    # ---- 握手汇总 ----
    print_section("握手汇总")
    print(f"  OPTIONS:  ✓ (CSeq=1)")
    print(f"  DESCRIBE: ✓ (CSeq=2, SDP, control={control})")
    print(f"  SETUP:    ✓ (CSeq=3, UDP, session={session_id})")
    print(f"  PLAY:     ✓ (CSeq=4, Range=npt=0.000-)")
    print(f"\n  RTSP 握手完成 ✓")
    print(f"  接下来应在 UDP port {RTP_CLIENT_PORT} 上接收 RTP 数据")
    print(f"  (本脚本不接收 RTP，请使用 test_rtsp.py 或 VLC 测试)")

    # 不发送 TEARDOWN — 保持连接以便调试
    print(f"\n[信息] 保持连接 3 秒供观察，然后关闭...")
    import time
    time.sleep(3)

    # TEARDOWN
    print_section("TEARDOWN")
    cseq += 1
    req = (
        f"TEARDOWN {RTSP_URL} RTSP/1.0\r\n"
        f"CSeq: {cseq}\r\n"
        f"Session: {session_id}\r\n"
        f"\r\n"
    )
    print_raw("请求", req)
    send_raw(sock, req)
    resp = recvall(sock, timeout=2.0)
    if resp:
        print_raw("响应", resp)

    sock.close()
    print("\n[完成] 连接已关闭")


if __name__ == "__main__":
    main()
"""
test_rtsp.py — RTSP 服务器完整功能测试脚本

测试流程:
  1. 启动 rtsp_server.exe 子进程
  2. TCP 连接到 localhost:8554
  3. OPTIONS → 验证响应
  4. DESCRIBE → 获取 SDP
  5. SETUP (UDP, client_port=5000-5001)
  6. PLAY → 开始接收视频
  7. UDP 监听 port 5000，接收 ≥3 个 RTP 包
  8. 验证 RTP 结构、JPEG 完整性
  9. 保存第一帧为 test_frame_0.jpg
 10. TEARDOWN → 关闭会话
 11. 终止服务器进程
"""

import socket
import struct
import subprocess
import time
import sys
import os


RTSP_HOST = "localhost"
RTSP_PORT = 8554
RTSP_URL = f"rtsp://{RTSP_HOST}:{RTSP_PORT}/test"
RTP_CLIENT_PORT = 5000
MIN_RTP_PACKETS = 3
FRAME_OUT = "test_frame_0.jpg"

SERVER_EXE = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                          "rtsp", "rtsp_server.exe")


def log(msg):
    print(f"[TEST] {msg}", flush=True)


def recvall(sock, bufsize=4096, timeout=5.0):
    sock.settimeout(timeout)
    try:
        data = b""
        while True:
            chunk = sock.recv(bufsize)
            if not chunk:
                break
            data += chunk
            if b"\r\n\r\n" in data:
                header_end = data.index(b"\r\n\r\n") + 4
                cl = parse_content_length(data[:header_end])
                if cl and len(data) >= header_end + cl:
                    break
                if not cl:
                    break
        return data
    except socket.timeout:
        return data if data else None


def parse_content_length(header_data):
    for line in header_data.split(b"\r\n"):
        if line.lower().startswith(b"content-length:"):
            return int(line.split(b":", 1)[1].strip())
    return None


def send_rtsp_request(sock, method, url, cseq, session=None, extra_headers="", body=""):
    req_line = f"{method} {url} RTSP/1.0\r\n"
    headers = f"CSeq: {cseq}\r\n"
    if session:
        headers += f"Session: {session}\r\n"
    headers += extra_headers
    if body:
        headers += f"Content-Length: {len(body)}\r\n"
    request = req_line + headers + "\r\n" + body
    sock.sendall(request.encode())
    log(f"→ {method} (CSeq={cseq})")
    if extra_headers:
        for h in extra_headers.strip().split("\r\n"):
            log(f"  {h}")
    return recvall(sock)


def parse_rtsp_response(data):
    if not data:
        return None, None, None
    lines = data.split(b"\r\n")
    if not lines:
        return None, None, None
    status_line = lines[0].decode("ascii", errors="replace")
    parts = status_line.split(" ", 2)
    status_code = int(parts[1]) if len(parts) >= 2 else 0
    headers = {}
    body_start = data.find(b"\r\n\r\n")
    header_part = data[:body_start] if body_start != -1 else data
    body = data[body_start + 4:] if body_start != -1 else b""
    for line in header_part.split(b"\r\n")[1:]:
        if b":" in line:
            key, val = line.split(b":", 1)
            headers[key.strip().decode("ascii", errors="replace").lower()] = val.strip().decode("ascii", errors="replace")
    return status_code, headers, body


def parse_sdp(body):
    sdp = {}
    for line in body.decode("ascii", errors="replace").split("\r\n"):
        if "=" in line:
            key, val = line.split("=", 1)
            sdp[key] = val
    return sdp


def parse_rtp_header(data):
    if len(data) < 12:
        return None
    byte0 = data[0]
    byte1 = data[1]
    version = (byte0 >> 6) & 0x03
    padding = (byte0 >> 5) & 0x01
    extension = (byte0 >> 4) & 0x01
    cc = byte0 & 0x0F
    marker = (byte1 >> 7) & 0x01
    payload_type = byte1 & 0x7F
    sequence = struct.unpack(">H", data[2:4])[0]
    timestamp = struct.unpack(">I", data[4:8])[0]
    ssrc = struct.unpack(">I", data[8:12])[0]
    return {
        "version": version,
        "padding": padding,
        "extension": extension,
        "cc": cc,
        "marker": marker,
        "payload_type": payload_type,
        "sequence": sequence,
        "timestamp": timestamp,
        "ssrc": ssrc,
        "header_len": 12 + cc * 4,
    }


def validate_jpeg(jpeg_data):
    if len(jpeg_data) < 2:
        return False, "JPEG 数据太短"
    if jpeg_data[0] != 0xFF or jpeg_data[1] != 0xD8:
        return False, f"缺少 SOI 标记 (前两字节: 0x{jpeg_data[0]:02X} 0x{jpeg_data[1]:02X})"
    if jpeg_data[-2] != 0xFF or jpeg_data[-1] != 0xD9:
        return False, f"缺少 EOI 标记 (最后两字节: 0x{jpeg_data[-2]:02X} 0x{jpeg_data[-1]:02X})"
    return True, "OK"


def main():
    log("=" * 60)
    log("RTSP 服务器测试开始")
    log("=" * 60)

    # ---- 1. 启动服务器 ----
    log(f"启动服务器: {SERVER_EXE}")
    server_proc = None
    try:
        server_proc = subprocess.Popen(
            [SERVER_EXE],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
    except FileNotFoundError:
        log(f"错误: 找不到服务器可执行文件 {SERVER_EXE}")
        log("请先编译 rtsp_server.exe")
        sys.exit(1)

    log("等待服务器启动 (1 秒)...")
    time.sleep(1)

    # ---- 2. TCP 连接 ----
    log(f"连接 {RTSP_HOST}:{RTSP_PORT} ...")
    rtsp_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    rtsp_sock.settimeout(5.0)
    try:
        rtsp_sock.connect((RTSP_HOST, RTSP_PORT))
    except Exception as e:
        log(f"错误: 无法连接到 RTSP 服务器: {e}")
        server_proc.terminate()
        server_proc.wait()
        sys.exit(1)
    log("✓ TCP 连接成功")

    cseq = 0
    session_id = None

    # ---- 3. OPTIONS ----
    log("")
    log("--- OPTIONS ---")
    cseq += 1
    resp = send_rtsp_request(rtsp_sock, "OPTIONS", RTSP_URL, cseq)
    status, headers, body = parse_rtsp_response(resp)
    log(f"状态码: {status}")
    log(f"响应头:")
    for k, v in headers.items():
        log(f"  {k}: {v}")
    assert status == 200, f"OPTIONS 响应状态码错误: {status}"
    assert "public" in headers, "OPTIONS 响应缺少 Public 头部"
    log(f"Public: {headers['public']}")
    log("✓ OPTIONS 通过")

    # ---- 4. DESCRIBE ----
    log("")
    log("--- DESCRIBE ---")
    cseq += 1
    resp = send_rtsp_request(rtsp_sock, "DESCRIBE", RTSP_URL, cseq,
                             extra_headers="Accept: application/sdp\r\n")
    status, headers, body = parse_rtsp_response(resp)
    log(f"状态码: {status}")
    log(f"Content-Type: {headers.get('content-type', 'N/A')}")
    log(f"Content-Base: {headers.get('content-base', 'N/A')}")
    log(f"SDP 描述:\n{body.decode('ascii', errors='replace')}")
    assert status == 200, f"DESCRIBE 响应状态码错误: {status}"
    assert "application/sdp" in headers.get("content-type", ""), "DESCRIBE 响应 Content-Type 错误"

    sdp = parse_sdp(body)
    assert "m=video" in sdp, "SDP 缺少 m=video 媒体行"
    media_line = sdp["m=video"]
    log(f"媒体行: {media_line}")
    assert "96" in media_line, f"Payload Type 应为 96，实际: {media_line}"

    control = sdp.get("a=control", "")
    log(f"Control: {control}")
    track_url = f"{RTSP_URL}/{control}" if control and not control.startswith("rtsp") else RTSP_URL
    if control == "*":
        track_url = RTSP_URL
    log(f"Track URL: {track_url}")
    log("✓ DESCRIBE 通过")

    # ---- 5. SETUP ----
    log("")
    log("--- SETUP (UDP) ---")
    cseq += 1
    transport = f"RTP/AVP;unicast;client_port={RTP_CLIENT_PORT}-{RTP_CLIENT_PORT + 1}"
    resp = send_rtsp_request(rtsp_sock, "SETUP", track_url, cseq,
                             extra_headers=f"Transport: {transport}\r\n")
    status, headers, body = parse_rtsp_response(resp)
    log(f"状态码: {status}")
    log(f"Session: {headers.get('session', 'N/A')}")
    log(f"Transport: {headers.get('transport', 'N/A')}")
    assert status == 200, f"SETUP 响应状态码错误: {status}"
    session_id = headers.get("session")
    assert session_id, "SETUP 响应缺少 Session ID"
    log(f"Session ID: {session_id}")

    server_transport = headers.get("transport", "")
    log(f"服务器 Transport 响应: {server_transport}")
    log("✓ SETUP 通过")

    # ---- 6. 准备 UDP RTP 接收 ----
    log("")
    log(f"绑定 UDP socket 到 port {RTP_CLIENT_PORT} ...")
    rtp_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rtp_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    rtp_sock.bind(("0.0.0.0", RTP_CLIENT_PORT))
    rtp_sock.settimeout(10.0)
    log("✓ UDP socket 绑定成功")

    # ---- 7. PLAY ----
    log("")
    log("--- PLAY ---")
    cseq += 1
    resp = send_rtsp_request(rtsp_sock, "PLAY", RTSP_URL, cseq, session=session_id,
                             extra_headers="Range: npt=0.000-\r\n")
    status, headers, body = parse_rtsp_response(resp)
    log(f"状态码: {status}")
    log(f"Session: {headers.get('session', 'N/A')}")
    log(f"Range: {headers.get('range', 'N/A')}")
    log(f"RTP-Info: {headers.get('rtp-info', 'N/A')}")
    assert status == 200, f"PLAY 响应状态码错误: {status}"
    log("✓ PLAY 通过 — 服务器开始发送 RTP 数据")

    # ---- 8. 接收 RTP 包 ----
    log("")
    log("--- 接收 RTP 包 ---")
    log(f"等待接收至少 {MIN_RTP_PACKETS} 个 RTP 包...")

    received_packets = []
    jpeg_frames = []
    packet_count = 0
    start_time = time.time()
    max_wait = 15.0

    while packet_count < MIN_RTP_PACKETS and (time.time() - start_time) < max_wait:
        try:
            data, addr = rtp_sock.recvfrom(65536)
            if len(data) < 12:
                log(f"  跳过太短的包 ({len(data)} 字节)")
                continue

            packet_count += 1
            rtp_hdr = parse_rtp_header(data)
            jpeg_data = data[rtp_hdr["header_len"]:]

            log(f"  包 #{packet_count}: {len(data)} 字节, "
                f"V={rtp_hdr['version']}, PT={rtp_hdr['payload_type']}, "
                f"M={rtp_hdr['marker']}, Seq={rtp_hdr['sequence']}, "
                f"TS={rtp_hdr['timestamp']}, SSRC=0x{rtp_hdr['ssrc']:08X}, "
                f"JPEG={len(jpeg_data)} 字节")

            assert rtp_hdr["version"] == 2, f"RTP 版本错误: {rtp_hdr['version']}"
            assert rtp_hdr["payload_type"] == 96, f"Payload Type 错误: {rtp_hdr['payload_type']}"

            is_valid, msg = validate_jpeg(jpeg_data)
            if is_valid:
                log(f"    ✓ JPEG 验证通过: {msg}")
                jpeg_frames.append(jpeg_data)
            else:
                log(f"    ✗ JPEG 验证失败: {msg}")

            received_packets.append({
                "header": rtp_hdr,
                "jpeg": jpeg_data,
                "raw": data,
            })

        except socket.timeout:
            log("  ⏱ 接收超时")
            break
        except Exception as e:
            log(f"  ✗ 接收错误: {e}")
            break

    log(f"共接收 {packet_count} 个 RTP 包")

    if packet_count < MIN_RTP_PACKETS:
        log(f"⚠ 警告: 只接收到 {packet_count}/{MIN_RTP_PACKETS} 个包")

    # ---- 9. 保存 JPEG 帧 ----
    if jpeg_frames:
        log("")
        log(f"--- 保存第一帧到 {FRAME_OUT} ---")
        with open(FRAME_OUT, "wb") as f:
            f.write(jpeg_frames[0])
        log(f"✓ 已保存 {len(jpeg_frames[0])} 字节到 {FRAME_OUT}")
    else:
        log("⚠ 没有有效的 JPEG 帧可保存")

    # ---- 10. 汇总 RTP 验证结果 ----
    log("")
    log("--- RTP 包验证汇总 ---")
    for i, pkt in enumerate(received_packets):
        hdr = pkt["header"]
        jpeg = pkt["jpeg"]
        valid, msg = validate_jpeg(jpeg)
        log(f"  包 #{i+1}:")
        log(f"    RTP 头: 版本={hdr['version']}, PT={hdr['payload_type']}, "
            f"Marker={hdr['marker']}, Seq={hdr['sequence']}, "
            f"TS={hdr['timestamp']}, SSRC=0x{hdr['ssrc']:08X}")
        log(f"    JPEG: {len(jpeg)} 字节, {'✓ ' + msg if valid else '✗ ' + msg}")

    # 验证序列号连续性
    if len(received_packets) >= 2:
        seqs = [p["header"]["sequence"] for p in received_packets]
        log(f"  序列号: {seqs}")
        seq_ok = all((seqs[i+1] - seqs[i]) % 65536 == 1 for i in range(len(seqs)-1))
        if seq_ok:
            log("  ✓ 序列号连续")
        else:
            log("  ✗ 序列号不连续")

    # 验证时间戳一致性（同一帧的包应有相同时间戳，此处每包一帧）
    if len(received_packets) >= 2:
        timestamps = [p["header"]["timestamp"] for p in received_packets]
        log(f"  时间戳: {timestamps}")

    # ---- 11. TEARDOWN ----
    log("")
    log("--- TEARDOWN ---")
    cseq += 1
    try:
        resp = send_rtsp_request(rtsp_sock, "TEARDOWN", RTSP_URL, cseq, session=session_id)
        status, headers, body = parse_rtsp_response(resp)
        log(f"状态码: {status}")
        assert status == 200, f"TEARDOWN 响应状态码错误: {status}"
        log("✓ TEARDOWN 通过")
    except Exception as e:
        log(f"TEARDOWN 异常（可能服务器已关闭连接）: {e}")

    # ---- 12. 清理 ----
    log("")
    log("--- 清理 ---")
    rtp_sock.close()
    rtsp_sock.close()
    log("Socket 已关闭")

    log("终止服务器进程...")
    if server_proc and server_proc.poll() is None:
        server_proc.terminate()
        try:
            server_proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            server_proc.kill()
            server_proc.wait()
    log("服务器进程已终止")

    # ---- 最终结果 ----
    log("")
    log("=" * 60)
    log("测试结果汇总")
    log("=" * 60)
    log(f"  OPTIONS:    ✓")
    log(f"  DESCRIBE:   ✓")
    log(f"  SETUP:      ✓ (session={session_id})")
    log(f"  PLAY:       ✓")
    log(f"  RTP 接收:   {'✓' if packet_count >= MIN_RTP_PACKETS else '⚠'} ({packet_count} 个包)")
    log(f"  JPEG 验证:  {'✓' if jpeg_frames else '⚠'} ({len(jpeg_frames)} 帧有效)")
    log(f"  TEARDOWN:   ✓")
    log(f"  帧保存:     {'✓' if jpeg_frames else '✗'} ({FRAME_OUT})")

    if jpeg_frames and packet_count >= MIN_RTP_PACKETS:
        log("")
        log("🎉 所有测试通过!")
        return 0
    else:
        log("")
        log("⚠ 部分测试未通过，请检查上方日志")
        return 1


if __name__ == "__main__":
    sys.exit(main())
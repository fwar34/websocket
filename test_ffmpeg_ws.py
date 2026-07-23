"""
测试 FFmpeg WebSocket 推流服务器
验证：
1. 能连接到 ws://localhost:8080
2. 收到 init 消息（包含正确的 width/height）
3. 收到视频二进制帧（数据大小 = width * height * 4）
"""
import socket
import hashlib
import base64
import struct
import os

def make_ws_key():
    return base64.b64encode(os.urandom(16)).decode('ascii')

def ws_handshake(sock):
    key = make_ws_key()
    request = (
        f"GET / HTTP/1.1\r\n"
        f"Host: localhost:8080\r\n"
        f"Upgrade: websocket\r\n"
        f"Connection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {key}\r\n"
        f"Sec-WebSocket-Version: 13\r\n"
        f"\r\n"
    )
    sock.sendall(request.encode('ascii'))
    
    response = b''
    while b'\r\n\r\n' not in response:
        data = sock.recv(4096)
        if not data:
            raise Exception("Connection closed during handshake")
        response += data
    
    if b'101' not in response.split(b'\r\n')[0]:
        raise Exception(f"Handshake failed: {response.split(b'\\r\\n')[0]}")
    
    return response

def recv_ws_frame(sock):
    """接收一个 WebSocket 帧，返回 (opcode, payload)"""
    header = sock.recv(2)
    if len(header) < 2:
        return None, None
    
    b0, b1 = header[0], header[1]
    opcode = b0 & 0x0F
    masked = (b1 & 0x80) != 0
    payload_len = b1 & 0x7F
    
    if payload_len == 126:
        ext = sock.recv(2)
        payload_len = struct.unpack('!H', ext)[0]
    elif payload_len == 127:
        ext = sock.recv(8)
        payload_len = struct.unpack('!Q', ext)[0]
    
    if masked:
        mask = sock.recv(4)
    
    payload = b''
    remaining = payload_len
    while remaining > 0:
        chunk = sock.recv(min(remaining, 65536))
        if not chunk:
            break
        payload += chunk
        remaining -= len(chunk)
    
    if masked:
        payload = bytes(payload[i] ^ mask[i % 4] for i in range(len(payload)))
    
    return opcode, payload

def main():
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(5)
    sock.connect(('localhost', 8080))
    
    print("[1] 握手...")
    ws_handshake(sock)
    print("[1] 握手成功")
    
    print("[2] 等待 init 消息...")
    for i in range(50):
        opcode, payload = recv_ws_frame(sock)
        if opcode is None:
            print("[!] 连接断开")
            return
        
        if opcode == 0x01:  # 文本帧
            text = payload.decode('utf-8')
            print(f"[2] 收到文本帧: {text}")
            
            if '"type":"init"' in text:
                import json
                info = json.loads(text)
                w = info.get('width', 0)
                h = info.get('height', 0)
                print(f"[2] 视频尺寸: {w}x{h}")
                expected_size = w * h * 4
                print(f"[2] 期望帧数据大小: {expected_size} 字节")
                break
        elif opcode == 0x02:  # 二进制帧
            print(f"[!] 先收到二进制帧（未收到 init）: {len(payload)} 字节")
            break
    
    print("[3] 等待视频帧...")
    received = 0
    for i in range(10):
        opcode, payload = recv_ws_frame(sock)
        if opcode is None:
            print("[!] 连接断开")
            break
        
        if opcode == 0x02:  # 二进制帧
            received += 1
            print(f"[3] 帧 {received}: {len(payload)} 字节 (期望 {expected_size})")
            
            if len(payload) == expected_size:
                print(f"[3] OK: frame size correct")
                
                # 检查前几个像素是否非零（不是全黑）
                sample = payload[:16]
                print(f"[3] first 4 pixels: {sample.hex()}")
            else:
                print(f"[3] ✗ 帧大小不匹配！差 {len(payload) - expected_size} 字节")
        elif opcode == 0x01:
            print(f"[3] 收到文本帧: {payload.decode('utf-8', errors='replace')}")
    
    sock.close()
    print("[4] 测试完成")

if __name__ == '__main__':
    main()

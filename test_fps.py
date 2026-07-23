"""测试帧率和 PTS 同步"""
import socket, struct, os, base64, time, json

def make_ws_key():
    return base64.b64encode(os.urandom(16)).decode('ascii')

def ws_handshake(sock):
    key = make_ws_key()
    request = f"GET / HTTP/1.1\r\nHost: localhost:8080\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n"
    sock.sendall(request.encode('ascii'))
    response = b''
    while b'\r\n\r\n' not in response:
        response += sock.recv(4096)

def recv_ws_frame(sock):
    header = sock.recv(2)
    if len(header) < 2: return None, None
    b0, b1 = header[0], header[1]
    opcode = b0 & 0x0F
    payload_len = b1 & 0x7F
    if payload_len == 126: payload_len = struct.unpack('!H', sock.recv(2))[0]
    elif payload_len == 127: payload_len = struct.unpack('!Q', sock.recv(8))[0]
    payload = b''
    while len(payload) < payload_len:
        payload += sock.recv(min(payload_len - len(payload), 65536))
    return opcode, payload

sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sock.settimeout(10)
sock.connect(('localhost', 8080))
ws_handshake(sock)
print("Handshake OK")

# 等待 init
for i in range(50):
    op, data = recv_ws_frame(sock)
    if op == 0x01:
        msg = json.loads(data.decode('utf-8'))
        if msg.get('type') == 'init':
            print(f"Init: {msg['width']}x{msg['height']}")
            expected = msg['width'] * msg['height'] * 4
            break

# 测量帧间隔
print("\nMeasuring frame intervals...")
timestamps = []
for i in range(20):
    op, data = recv_ws_frame(sock)
    if op == 0x02:
        timestamps.append(time.time())
        if len(timestamps) > 1:
            interval_ms = (timestamps[-1] - timestamps[-2]) * 1000
            print(f"Frame {len(timestamps)}: {len(data)} bytes, interval={interval_ms:.1f}ms")

if len(timestamps) > 1:
    intervals = [(timestamps[i+1]-timestamps[i])*1000 for i in range(len(timestamps)-1)]
    avg = sum(intervals)/len(intervals)
    fps = 1000.0/avg if avg > 0 else 0
    print(f"\nAverage interval: {avg:.1f}ms, FPS: {fps:.1f}")

sock.close()

"""Show the Xiaophone's logs over Wi-Fi (port 23). Usage: python netlog.py [host] [seconds]"""
import socket, sys, time

host = sys.argv[1] if len(sys.argv) > 1 else "xiaophone.local"
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 1e9
s = socket.create_connection((host, 23), timeout=5)
s.settimeout(1)
end = time.time() + secs
while time.time() < end:
    try:
        d = s.recv(4096)
    except socket.timeout:
        continue
    if not d:
        break
    sys.stdout.write(d.decode("utf-8", "replace"))
    sys.stdout.flush()

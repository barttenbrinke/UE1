import os, pty, select, sys, time
S = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.abspath(__file__))   # work dir holding pspsh.in / pspsh.log
fifo = S + '/pspsh.in'; log = S + '/pspsh.log'
if not os.path.exists(fifo): os.mkfifo(fifo)
pid, fd = pty.fork()
if pid == 0:
    os.execvp('pspsh', ['pspsh'])
fin = os.open(fifo, os.O_RDWR | os.O_NONBLOCK)
out = open(log, 'ab', 0)
while True:
    r, _, _ = select.select([fd, fin], [], [], 1.0)
    if fd in r:
        try: data = os.read(fd, 65536)
        except OSError: break
        if not data: break
        out.write(data.replace(b'\r', b''))
    if fin in r:
        try: d = os.read(fin, 4096)
        except BlockingIOError: d = b''
        if d: os.write(fd, d)
    p, _ = os.waitpid(pid, os.WNOHANG)
    if p: break
out.write(b'\n[pspsh exited]\n')

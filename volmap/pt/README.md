# pt — 브리핑 자료

## 구성

| 파일 | 내용 |
|---|---|
| `cub_volmap-briefing.html` | 6절 브리핑 — 문제 정의 / 실화면 2종 / 설계 원칙 / 아키텍처 / 배포·사용 |
| `ansi2svg.py` | **캡처 렌더러(브리핑용)** — pty 캡처를 SVG 로 변환 |
| `ansi2html.py` | 터미널 시뮬레이터. `ansi2svg.py` 가 이 모듈을 재사용한다 |

## 왜 SVG 인가

지도는 채움 램프에 점자 글리프(U+2800~28FF)를 쓰는데, 흔한 고정폭 폰트
(SF Mono·Menlo·Consolas)에 **점자가 없다.** HTML 텍스트로 넣으면 브라우저가
두부(`#`·`:`)로 대체해 화면이 깨진다.

SVG 는 점자의 점을 사각형으로 직접 그리므로 폰트에 의존하지 않는다. ASCII 텍스트만
`<text>` 로 남기는데, 그건 어느 고정폭 폰트에나 있다.

브리핑의 화면은 **현재 소스로 빌드한 바이너리를 실제로 띄워 캡처**한 것이다. 손으로 그린 목업이 아니다.

## 캡처 재생성

화면이 바뀌면 캡처도 다시 떠야 한다.

```sh
# 1) pty 로 띄워 키를 보내고 마지막 프레임을 파일로 받는다
#    (예: 오른쪽 6칸 이동 후 드릴다운 2단)
python3 - <<'EOF'
import os, pty, fcntl, termios, struct, time, select
pid, fd = pty.fork()
if pid == 0:
    os.environ.update(LANG='en_US.UTF-8', TERM='xterm-256color')
    os.execv('./cub_volmap', ['cub_volmap', '-i', 'cbench'])
fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack('HHHH', 44, 150, 0, 0))
buf = b''
def pump(sec):
    global buf
    t = time.time()
    while time.time() - t < sec:
        r, _, _ = select.select([fd], [], [], 0.1)
        if r:
            d = os.read(fd, 400000)
            if not d: return
            buf += d
pump(6)
for k in [b'\x1b[C'] * 6 + [b' ', b' ']:
    os.write(fd, k); pump(1.2)
buf = b''; os.write(fd, b'r'); pump(2.5)   # 마지막 프레임만 남긴다
os.kill(pid, 15)
open('cap.bin', 'wb').write(buf)
EOF

# 2) SVG 로 렌더
python3 ansi2svg.py cap.bin 44 150 cap.svg
```

생성된 `cap.svg` 를 브리핑의 `<div class="screen">` 안에 통째로 넣는다.

## 캡처 시 주의

- **마지막 프레임만** 써야 한다. 버퍼를 비우고 `r`(새로고침)을 한 번 보내면 그 뒤 출력이 한 화면이 된다.
- 터미널 크기는 `TIOCSWINSZ`로 명시한다. 렌더러에 넘기는 `ROWS COLS`와 같아야 한다.
- 점자 글리프(U+2800~)는 1칸이다. 폭 계산이 어긋나면 렌더가 밀린다.

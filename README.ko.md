# wine-dxgi-framelimiter

**[English README](README.md)**

**macOS에서 Wine + [DXMT](https://github.com/3Shain/dxmt)로 실행하는 Windows 게임**용 저지연 프레임 제한 도구예요.
고주사율(120/240Hz) 모니터에서 게임만 60fps(또는 원하는 값)로 제한해 전력과 발열을 줄입니다. 이때 DXMT 자체 제한처럼 **마우스가 늦게 따라오는 지연이 생기지 않고**, Steam 등 다른 프로그램에는 영향이 없어요.

```
./setup.sh
./flwine --fps 60 --exe Game.exe "C:\Program Files (x86)\Steam\Steam.exe"
```

## 왜 만들었나

| 방법 | 문제 |
|---|---|
| 게임 내 60fps 옵션 | 대개 VSync 기반이라, 240Hz 모니터에서는 제대로 제한되지 않거나 자원을 많이 씀 |
| DXMT `DXMT_CONFIG="d3d11.preferredMaxFrameRate=60"` | 파이프라인의 **맨 끝**(`presentDrawable:afterMinimumDuration:`)에서만 늦춤. 그동안 게임은 계속 앞서서 프레임을 만들어 DXMT 큐(3프레임)와 drawable 풀이 가득 참 → **입력 지연 약 50~80ms**, 마우스가 눈에 띄게 늦게 따라옴 |
| Metal 레이어 후킹 방식 (`-[CAMetalLayer nextDrawable]`) | DXMT 인코드 스레드에서 기다리는 거라 3프레임 큐는 그대로 남음 |

이 도구는 **게임 렌더 스레드가 `Present()`를 호출한 직후, 그 스레드에서** 다음 프레임 시각까지 기다립니다. DXVK의 `dxgi.maxFrameRate`나 RTSS와 같은 방식이에요. 게임은 깨어나자마자 최신 입력을 읽어 렌더링하고, 그 프레임이 곧바로 화면에 나갑니다. 큐가 쌓이지 않으니 입력 지연은 약 1프레임 수준이에요. macOS 마우스 커서는 WindowServer가 따로 그려서 영향이 없습니다.

## 동작 구조

```
Game.exe ──► system32\dxgi.dll (이 프록시) ──► system32\dxgi_orig.dll (원래 DXMT dxgi)
                 │
                 ├─ CreateDXGIFactory / 1 / 2 / DXGIGetDebugInterface1을 그대로 전달
                 ├─ factory의 CreateSwapChain* 메서드를 패치
                 └─ IDXGISwapChain::Present / Present1을 패치:
                        진짜 Present를 호출한 뒤, 다음 프레임 시각까지 대기
```

- `FL_FPS`가 설정돼 있고 **프로세스 이름이 `FL_EXE`와 맞을 때만** 동작해요. 그 외에는 아무것도 하지 않고 그대로 전달만 합니다.
- `NtDelayExecution`으로 잠들어서 바쁜 대기가 없어요. 대기 시각은 누적해서 계산하므로 시간이 밀리지 않고, 로딩이나 alt-tab 뒤에는 다시 맞춰서 밀린 프레임을 한꺼번에 몰아 그리지 않아요.

## 요구 사항

- macOS + Wine (테스트 환경: Wine Stable 11.0, Apple Silicon에서 Rosetta로 x86_64 실행)
- **prefix에 DXMT가 설치돼 있어야 함.** `system32\dxgi.dll`이 DXMT 것이고, `dxgi` DLL 오버라이드가 `native,builtin`이어야 해요. DXMT를 설치하면 보통 이렇게 설정됩니다.
- 64비트 D3D11 게임 (DXMT의 D3D12도 될 것으로 보이지만 테스트하지 않음)
- 선택: 소스에서 빌드하려면 `brew install mingw-w64`가 필요해요. 없으면 `setup.sh`가 [최신 릴리스](../../releases/latest)에서 미리 빌드된 `dxgi.dll`을 받아요.

## 테스트한 환경

| | |
|---|---|
| Mac | MacBook Pro, Apple M5 (Mac17,2) |
| macOS | 27.0.1 |
| 디스플레이 | 외장 1920×1080 @ **240Hz** (메인) |
| Wine | Wine Stable 11.0 (`/Applications/Wine Stable.app`, Rosetta로 x86_64 실행), prefix `~/.wine` |
| 그래픽 | DXMT (`d3d11`/`dxgi` = `native,builtin`) |
| Steam | Wine 안의 Windows용 Steam. Automator 앱에서 `-no-cef-sandbox -vgui -silent` 옵션으로 실행 |
| 게임 | **블루 아카이브** (Steam, Unity, D3D11), `BlueArchive.exe` |

블루 아카이브에서 `FL_FPS=60`으로 실행한 결과: 60fps로 안정적으로 고정되고, `d3d11.preferredMaxFrameRate=60`을 쓸 때와 달리 마우스가 지연 없이 따라와요. GPU 사용률은 약 40%였고, Steam 자체는 제한되지 않아요.

다른 게임에서도 써 보셨다면 이슈나 PR로 이 표에 추가해 주세요.

## 설치 (원클릭)

```sh
git clone https://github.com/dngur521/wine-dxgi-framelimiter.git
cd wine-dxgi-framelimiter
./setup.sh                        # 기본 prefix: $WINEPREFIX 또는 ~/.wine
./setup.sh --prefix ~/my-prefix   # 다른 prefix에 설치
```

`setup.sh`가 하는 일:

1. 프록시를 빌드하거나 다운로드해요.
2. prefix에 정말 DXMT가 있는지 확인해요.
3. 원본은 `system32\dxgi_orig.dll`로 두고, `./backup/`에 날짜를 붙인 사본도 남겨요.
4. 프록시를 `system32\dxgi.dll`로 설치해요.

여러 번 실행해도 안전해요.

## 사용법

**1. 게임 exe 이름 확인하기** (예: `BlueArchive.exe`). 게임을 실행한 상태에서:

```sh
ps -axo comm | grep -i '\.exe'
```

**2. Steam을 완전히 종료하세요.** 같은 prefix의 다른 Wine 프로그램도 모두 종료해야 해요. 환경변수는 Wine이 새로 시작될 때만 게임까지 전달됩니다. 이미 실행 중인 Steam으로 게임을 켜면 설정이 적용되지 않아요.

**3. `flwine`으로 실행하기:**

```sh
./flwine --fps 60 --exe BlueArchive.exe "C:\Program Files (x86)\Steam\Steam.exe" -silent
```

또는 기존 실행 스크립트의 `wine` 줄 앞에 아래 내용을 넣어도 돼요.

```sh
export FL_FPS=60
export FL_EXE=BlueArchive.exe
# export DXMT_CONFIG="d3d11.preferredMaxFrameRate=60;"   # 이전 방식: 입력 지연이 생기니 끄기
wine "C:\Program Files (x86)\Steam\Steam.exe" -silent &
```

**4. 게임 설정:** 게임의 프레임 옵션은 최대나 무제한으로, VSync는 꺼 두세요. 게임 자체 제한과 겹치지 않게 하기 위해서예요.

### 예시: macOS 앱으로 만들기 (Automator)

Automator → 새로 만들기 → 응용 프로그램 → *셸 스크립트 실행*:

```sh
export FL_FPS=60
export FL_EXE=BlueArchive.exe
wine "C:\Program Files (x86)\Steam\Steam.exe" -silent > /dev/null 2>&1 &
```

## 잘 모르겠다면: AI 비서에게 맡기기

Claude Code, Codex, Cursor처럼 내 Mac에서 터미널 명령을 실행할 수 있는 AI 에이전트를 쓰고 있다면, 아래 내용을 그대로 붙여 넣으세요. 대괄호 부분만 바꾸면 됩니다.

```text
macOS에서 Wine + DXMT로 실행하는 Windows 게임을 [60]fps로 제한하고 싶어.
https://github.com/dngur521/wine-dxgi-framelimiter 를 사용할 거야. 먼저 README를 읽고 아래 순서로 진행해 줘.
1. 내 Wine prefix와 게임 실행 방법을 찾아줘. $WINEPREFIX나 ~/.wine, 내가 쓰는 실행 앱이나
   스크립트, 실행 중인 프로세스(`ps -axo comm`)를 확인해 줘. 그 prefix의
   system32\dxgi.dll이 DXMT 것인지 확인하고, 아니면 멈추고 알려줘.
2. 게임 exe 이름을 찾아줘. 나한테 게임을 켜 달라고 한 다음, *.exe 프로세스를 확인해 줘.
3. 저장소를 clone하고 ./setup.sh를 실행해 줘 (필요하면 --prefix 사용). 출력은 나한테 보여줘.
4. 실행 스크립트의 wine 명령 앞에 `export FL_FPS=[60]`과 `export FL_EXE=<게임 exe>`를
   추가해 줘. 수정하기 전에 스크립트를 백업하고,
   DXMT_CONFIG="d3d11.preferredMaxFrameRate=..." 줄이 있으면 지우지 말고 주석 처리한 다음
   이유를 설명해 줘.
5. Steam을 완전히 종료하고 다시 실행하라고 안내해 줘. 게임 내 프레임 설정은
   최대/무제한, VSync는 끄라고도 알려줘.
6. FL_LOG를 켜고 한 번 실행해서 로그에 "hooked swapchain"과 fps≈[60]이 찍히는지
   확인해 줘. 확인이 끝나면 로그 끄는 방법도 알려줘.
Wine prefix의 다른 부분은 건드리지 말고, 위험할 수 있는 작업은 하기 전에 먼저 물어봐 줘.
```

채팅만 되는 AI라면 같은 내용에 `./setup.sh status` 출력과 내 실행 스크립트 내용을 함께 붙여 넣고, 정확한 진행 순서를 알려 달라고 하면 돼요.

## 설정 (환경변수)

| 변수 | 기본값 | 의미 |
|---|---|---|
| `FL_FPS` | 없음 | 프레임 제한값. 없거나 `0`이면 프록시가 아무것도 하지 않음 |
| `FL_EXE` | 전체 | 제한할 exe 이름. 쉼표로 여러 개 지정 가능하고 대소문자는 구분하지 않음 (예: `Game.exe,GameLauncher.exe`). 없거나 `*`이면 `FL_FPS`를 받은 모든 DXGI 프로그램에 적용 |
| `FL_LOG` | 없음 | 로그 파일의 Windows 경로 (예: `Z:\Users\you\fl.log`). 후킹 여부와 5초마다의 실측 fps를 기록. `flwine --log`에는 macOS 경로를 넘기면 됨 |
| `FL_SYNC` | `-1` | `-1`은 게임의 SyncInterval 그대로, `0`은 VSync 강제 끄기, `1`~`4`는 강제 지정 |
| `FL_SPIN_US` | `0` | 대기 마감 직전 N마이크로초 동안 바쁜 대기. Apple Silicon에서 정확도 이득이 없어서 기본은 끔 |
| `FL_REAL_DXGI` | `<system32>\dxgi_orig.dll` | 진짜 dxgi 경로 |

## 동작 확인

```sh
./flwine --fps 60 --exe Game.exe --log ~/fl.log "C:\...\Steam.exe"
tail -f ~/fl.log
```

```
[21:19:22.047 pid=2572] active: fps=60 spin_us=0 sync=-1
[21:19:22.398 pid=2572] hooked factory vtbl=... factory2=1
[21:19:22.428 pid=2572] hooked swapchain vtbl=... present1=yes
[21:19:27.430 pid=2572] fps=59.98 (target 60)
```

- `active:` 줄이 없으면 → `FL_FPS`가 게임까지 전달되지 않은 거예요. Steam이 이미 실행 중이었거나, `FL_EXE` 이름이 다를 수 있어요.
- `active:`는 있는데 `hooked swapchain`이 없으면 → 게임이 DXGI로 swapchain을 만들지 않는 경우예요. D3D10/11/12 게임이 아니거나 DXMT를 쓰지 않는 경우입니다.

테스트 프로그램도 있어요. `./build.sh && cd build && ../flwine --fps 60 --exe fltest.exe fltest.exe 5`를 실행하면 실측 fps와 프레임 시간 편차를 출력합니다.

테스트 프로그램 측정값 (DXMT, Apple M5, Wine 11):

| | fps | 프레임 시간 |
|---|---|---|
| 제한 없음 | 약 740 | 1.3ms |
| `FL_FPS=60` | 59.96~60.00 | 16.67ms (편차 약 1.5ms) |
| `FL_FPS=120` | 119.98 | 8.34ms (편차 약 0.9ms) |

## 제거

```sh
./setup.sh uninstall   # dxgi_orig.dll을 다시 dxgi.dll로 되돌림
./setup.sh status
```

DXMT를 업데이트하거나 다시 설치하면 `system32\dxgi.dll`이 덮어써져요. 그 뒤에 `./setup.sh`만 다시 실행하면 됩니다.

## 호환성

- **DXMT**: 테스트 완료 (D3D11)
- **DXVK**: 필요 없어요. DXVK 자체의 `dxgi.maxFrameRate`가 이미 같은 방식으로 동작합니다.
- **Wine 내장 wined3d dxgi**: 동작하지 **않아요**. 내장 d3d11이 dxgi의 비공개 export를 필요로 하기 때문이에요. `setup.sh`는 `--force` 없이는 설치를 거부합니다.
- **GPTK / D3DMetal / CrossOver**: 테스트하지 않음
- **32비트 게임**: 지원 안 함 (프록시가 64비트 전용)

## ⚠️ 안티치트 주의

Wine prefix 안의 Windows DLL을 교체하는 방식이에요. 게임 파일 자체는 건드리지 않지만, 안티치트나 변조 방지 프로그램이 감지할 수 있어요. 경쟁 온라인 게임에서는 쓰지 마세요. 게임이 실행되지 않으면 `./setup.sh uninstall`로 되돌리면 됩니다. 사용에 따른 책임은 본인에게 있어요.

## 빌드

```sh
brew install mingw-w64
./build.sh        # → build/dxgi.dll, build/fltest.exe
```

## 크레딧

- [DXMT](https://github.com/3Shain/dxmt) (3Shain): 이 프록시가 앞에 붙는 Metal 기반 D3D11/D3D10 변환 레이어
- [Aatricks/FrameLimiter](https://github.com/Aatricks/FrameLimiter): macOS 네이티브 Metal 프레임 리미터. 대기 시각 페이싱 아이디어를 참고했어요 (코드는 공유하지 않음)
- 프록시 DLL과 `Present` 후킹은 ReShade, Special K, RTSS가 써 온 고전적인 기법이에요

## 라이선스

MIT

# offsets/

방법 B(엔진 직독)가 쓰는 **구조체 오프셋 표**. 버전마다 한 파일이다.

| 파일 | 내용 |
|---|---|
| `cubrid-<버전>.tbl` | 그 버전에서 추출한 필드 오프셋·크기 |
| `build-offsets.sh` | 소스를 받아 빌드하고 표를 새로 만드는 스크립트 |

## 표가 담는 것

```
#meta version=11.5.0.2374
OFF_PGBUF_NBUF 0
SZ_CSS_CONN_ENTRY 480
```

심볼 이름과 **정수**뿐이다. CUBRID 소스 코드는 들어 있지 않다.

## 왜 소스에서 바로 못 만드나

이 값은 **컴파일러가 정한다.** 구조체 필드의 바이트 오프셋은 패딩·정렬·빌드 플래그(`#ifdef`)에
좌우되므로 헤더 텍스트만 읽어서는 확정할 수 없다. 그래서 해당 버전을 실제로 빌드하고,
생성된 `libcubrid.so` 의 DWARF 에서 뽑는다.

같은 메이저 버전 안에서도 바뀐다 — 11.5.0.2374 와 11.5.0.2548 은 `SZ_CSS_CONN_ENTRY` 가
480 과 488 로 다르다. 표가 버전별로 나뉘어 있는 이유다.

## 어떤 표가 필요한지 확인

```sh
sh tools/check-releases.sh
```

GitHub Releases 에서 각 major.minor 의 최신 태그를 읽어, 그 패치에 해당하는 표가 있는지
본다. 없거나 뒤처지면 **종료 코드 2** 와 함께 필요한 빌드 명령을 출력한다.

```
  10.2     10.2.18          10.2.17.9008  (behind)
  11.1     11.1.0.0441      none
```

표가 없으면 cub_top 은 DWARF 추출이나 앵커 탐사로 **조용히 내려간다** — 실패하지 않으므로
테스트로는 드러나지 않는다. 이 검사가 따로 있는 이유다.

### 공개 빌드에서 뽑기 (권장)

표의 값은 `libcubrid.so` 의 DWARF 에서 나온다. 공식 배포본에는 `.debug_info` 가 들어 있어
**소스 빌드가 필요 없다.** tarball 전체를 풀지 않고 라이브러리만 꺼낸다.

```sh
# 해당 라인의 파일명 확인
curl -fsSL https://ftp.cubrid.org/CUBRID_Engine/11.4.6/ |
  grep -oE 'CUBRID-[0-9.]+-[0-9a-f]+-Linux\.x86_64\.tar\.gz'

# 스트림에서 lib 만 추출 (242MB 중 libcubrid.so 만)
curl -fsSL https://ftp.cubrid.org/CUBRID_Engine/11.4.6/CUBRID-11.4.6.1963-0e7d3c1-Linux.x86_64.tar.gz |
  tar -xz --wildcards '*/lib/libcubrid.so*'

sh tools/collect-offsets.sh -o offsets ./CUBRID
sh tools/build.sh                      # 표를 바이너리에 내장
```

`ftp.cubrid.org` 는 GitHub 릴리스보다 패치 하나쯤 뒤처질 수 있고, 모든 라인이 올라오지도
않는다(11.1 은 없다). 공개 빌드가 없는 버전만 아래 소스 빌드를 쓴다.

## 새 버전 표 만들기

```sh
sh offsets/build-offsets.sh v11.5.0.2548        # 태그 하나
sh offsets/build-offsets.sh v11.4.6.1963 develop # 여러 개
sh offsets/build-offsets.sh -k -w /var/tmp/cub v11.5.0.2548  # 빌드 트리 유지
```

| 옵션 | 뜻 |
|---|---|
| `-r URL` | 소스 저장소 (기본 CUBRID 공식 저장소) |
| `-o DIR` | 출력 디렉터리 (기본 이 디렉터리) |
| `-w DIR` | 작업 디렉터리 (기본 임시, 종료 시 삭제) |
| `-j DIR` | JAVA_HOME — CUBRID `build.sh` 의 `-j` 와 같다 |
| `-k` | 작업 디렉터리 유지 (빌드 재사용) |
| `-f` | **이미 있는 `.tbl` 을 덮어쓴다** |

**기존 표는 기본적으로 건드리지 않는다.** 이미 검증된 표를 빌드 환경 차이로 조용히 갈아치우면,
그 표로 동작하던 환경이 설명 없이 깨질 수 있다. 덮어쓰려면 `-f` 를 명시한다.
태그 이름으로 버전을 예측할 수 있으면 **빌드 전에** 건너뛰므로 수십 분을 아낀다.

### 요구사항

CUBRID 빌드 요구와 같다 — `git`, `cmake`, `gcc/g++`, `ant`, JDK.
빌드는 **반드시 debug 모드**여야 한다(`build.sh -m debug`). release 빌드에는 `.debug_info` 가
없어 추출할 수 없고, 스크립트가 그 사유를 적고 건너뛴다.

빌드에 수십 분이 걸린다. 여러 버전을 만들 때는 `-k` 로 작업 디렉터리를 유지하면
clone 을 다시 하지 않는다.

## 바이너리에 내장된다

`offsets/*.tbl` 은 빌드할 때 [`gen-embedded.sh`](gen-embedded.sh) 가 C 헤더
(`src/offsets_embedded.h`)로 바꿔 바이너리에 넣는다. 그래서 **`cub_top` 파일 하나만 복사해도
10.0~11.5 전 버전에서 방법 B 가 동작한다** — `offsets/` 디렉터리를 같이 옮길 필요가 없다.

표 전체가 약 5KB 라 바이너리 증가는 무시할 만하다.

`tools/build.sh` 가 `.tbl` 이 헤더보다 새로우면 자동으로 다시 만든다. 표를 추가·수정한 뒤에는
그냥 빌드하면 된다.

```sh
sh offsets/gen-embedded.sh        # 수동 생성이 필요할 때
```

표 텍스트를 그대로 넣고, 파싱은 파일 경로와 **같은 함수**(`offs_text_load`)를 쓴다.
숫자만 뽑아 구조체 리터럴로 만들면 파일과 내장이 다르게 해석될 여지가 생기기 때문이다.

## 표가 쓰이는 순서

1. 감지한 버전과 **일치하는 내장 표**(소스 `BTABS[]`)
2. `--offsets` 로 준 파일/디렉터리, 또는 실행파일 옆 `offsets/`
3. **내장된 `.tbl`**(위 "바이너리에 내장된다") — 파일이 없을 때
4. 그 환경 `libcubrid.so` 의 DWARF 에서 직접 추출
5. 앵커 탐사 — **개수만** 확정(크기는 지어내지 않는다)

파일(2)이 내장(3)보다 먼저다. 새 패치 버전이 나왔을 때 바이너리를 다시 빌드하지 않고
표만 건네면 되기 때문이다.

어느 경로로 왔든 채택은 **3중 게이트**(심볼 존재·오프셋 정렬·`paramdump` 교차검증)를
통과해야 한다. 통과하지 못하면 그 기능만 끄고 사유를 화면과 `heapB.reason` 에 남긴다.

1번이 우선이므로, `BTABS[]` 에 있는 버전(현재 11.5)에서는 `--offsets` 로 준 표가 쓰이지 않는다.
`build-offsets.sh` 가 필요한 경우는 **표가 없는 새 버전**이거나 **구조체가 바뀐 패치 버전**이다.

## 기존 수집 방식

이미 설치된 CUBRID 에서 뽑으려면 빌드 없이 `tools/collect-offsets.sh` 를 쓴다.
그쪽은 설치본의 `libcubrid.so` 를 그대로 읽으므로, **배포본에 `.debug_info` 가 있을 때만** 된다.

```sh
sh tools/collect-offsets.sh -o offsets /opt/cubrid /another/CUBRID
```

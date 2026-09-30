# 빌드 (Build)

## 1. 세 가지 경로

| 경로 | 필요한 것 | 쓰는 때 |
|---|---|---|
| **빌드하지 않음** | 없음 | 리포의 `cub_volmap` 을 그대로 복사한다. 완전 정적·의존 0 |
| **헤더 fetch** | `curl`, `g++` | 소스 체크아웃 없이 빌드한다. 버전 지정 가능 |
| **소스 체크아웃** | CUBRID 소스 + CMake 한 번 실행 | 이미 개발 트리가 있을 때 |

## 2. 왜 헤더가 필요한가

volmap 은 온디스크 구조체를 **복사해 두지 않고** 엔진 헤더에서 `#include` 한다.

```c
#include "file_io.h"    /* FILEIO_PAGE_RESERVED - prv 헤더 레이아웃 */
#include "oid.h"
#include "storage_common.h"
#include "storage_ondisk_layout.hpp"
```

오프셋을 컴파일러가 계산하므로 포맷이 바뀌면 **컴파일 단계에서 드러난다**.
손으로 `offset 24` 같은 값을 적어 두는 방식이었다면 소스 없이 빌드되겠지만,
엔진이 바뀐 뒤에도 조용히 틀린 지도를 그리게 된다.

이것이 "오프셋 하드코딩 0" 원칙의 대가이고, 아래 fetch 방식이 그 대가를 실무적으로 줄인다.

## 3. 헤더 fetch 빌드

```sh
sh tools/build_fetch.sh                    # 기본 ref = develop
REF=v11.4.6.1963 sh tools/build_fetch.sh   # 특정 태그
KEEP=1 sh tools/build_fetch.sh             # 받은 헤더를 남긴다(.fetch/)
```

동작:

1. volmap 이 포함하는 헤더 **23개**를 `raw.githubusercontent.com` 에서 받는다
   (버전에 따라 선택 헤더 1개 추가).
2. CMake 생성물인 `config.h`·`version.h` 를 스크립트가 직접 만든다.
   전자는 feature-test 매크로 모음이고, 후자는 volmap 이 쓰지 않는 버전 문자열이다.
3. 구버전 호환 처리(§5)를 적용한 뒤 컴파일한다.

clone 도, cmake 도, 빌드 트리도 필요 없다.

### 받는 헤더

```
include/system.h
src/base/        cubrid_getopt.h  databases_file.h  dynamic_array.h
                 environment_variable.h  error_code.h  memory_alloc.h
                 memory_hash.h  message_catalog.h
                 object_representation_constants.h  porting.h
                 porting_inline.hpp  release_string.h  sha1.h  util_func.h
src/compat/      cache_time.h  dbtype_def.h
src/executables/ utility.h
src/storage/     file_io.h  oid.h  storage_common.h
src/thread/      thread_compat.hpp
src/transaction/ log_lsa.hpp

선택: src/base/memory_cwrapper.h   (11.4+ 에만 존재)
```

목록은 `g++ -MM` 으로 실제 의존을 추출해 확정했다. 늘리거나 줄이면 빌드가 즉시 깨지므로
추측이 섞일 여지가 없다.

## 4. 검증된 버전

전부 빌드하고 실제 볼륨을 열어 동작을 확인했다.

| 태그 | 빌드 | 실행 | 비고 |
|---|---|---|---|
| `v10.2.18` | OK | OK | lzo typedef + pflag 매핑 필요 |
| `v11.0.16` | OK | OK | lz4 스텁 필요 |
| `v11.2.9.0868` | OK | OK | 〃 |
| `v11.3.5.1280` | OK | OK | 〃 |
| `v11.4.6.1963` | OK | OK | 〃 |
| `develop` | OK | OK | 스텁 불요(헤더에서 압축 include 제거됨) |

산출 바이너리의 md5 는 세 묶음으로 나뉜다 — **헤더가 실제로 바뀐 경계**와 일치한다.

| md5 묶음 | 버전 |
|---|---|
| A | 10.2, 11.0 |
| B | 11.2, 11.3 |
| C | 11.4, develop |

## 5. 구버전 호환 — 자동 처리되는 세 가지

### 5.1 `memory_cwrapper.h` 부재 (≤11.3)

11.4 에서 도입된 헤더다. 선택 목록에 두고 404 면 건너뛴다.

### 5.2 압축 헤더 include (10.2~11.4)

`file_io.h` 가 `lz4.h`(11.0~11.4) 또는 `lzo/lzoconf.h`(10.2) 를 include 한다.
volmap 은 **압축 심볼을 하나도 쓰지 않으므로**(열거형 값만 헤더에 함께 선언돼 있다)
빈 스텁으로 충족한다.

10.2 만은 구조체 필드가 LZO 타입이라 typedef 가 필요하다.

```c
typedef unsigned long  lzo_uint;
typedef unsigned char  lzo_byte;
typedef unsigned char *lzo_bytep;
```

### 5.3 TDE 플래그 필드명 (10.2 vs 11.0+)

```c
/* 10.2  */ unsigned char pflag_reserve_1;   /* unused - Reserved field */
/* 11.0+ */ unsigned char pflag;             /* TDE 도입하며 이름 부여 */
```

**레이아웃은 동일**하다 — `prv` 는 양쪽 다 32바이트다. 이름만 다르므로 스크립트가
헤더를 확인해 `-Dpflag=pflag_reserve_1` 을 붙인다. volmap 은 이 바이트를 TDE 페이지
감지에만 쓴다.

## 6. 소스 체크아웃 빌드

```sh
SRC=/path/to/cubrid-src bash tools/build_standalone.sh
BUILD=/path/to/build SRC=/path/to/cubrid-src bash tools/build_standalone.sh
```

`BUILD` 기본값은 `$SRC/build_release`. **CMake 를 한 번은 돌려야 한다** — `config.h` 가
소스 트리에 없고 빌드 디렉터리에만 생성되기 때문이다. 돌리지 않으면 이렇게 끝난다.

```
fatal error: config.h: No such file or directory
```

## 7. 산출물

| 바이너리 | 크기 | 링크 | 용도 |
|---|---:|---|---|
| `cub_volmap` | 1.3MB | 완전 정적 | 기본. 어느 서버에나 파일 하나로 복사 |
| `cub_volmap-dyn` | 0.15MB | glibc 동적 | 런타임 `dlopen` 으로 Pass 2(라이브 오버레이) 가능 |

정적 빌드에서는 `dlopen` 을 쓰지 않는다(`-DVOLMAP_NO_DLOPEN`). 정적 glibc 에 공유 glibc 가
이중 적재되어 segfault 가 난다.

## 8. 남는 컴파일 경고

경고 4건이 남아 있고 전부 `snprintf` 의 `-Wformat-truncation` 이다. 경계가 보장되어
오버플로가 불가능하며, 좁은 박스에서 **의도적으로** 줄이는 자리다(제목·축척 인셋).

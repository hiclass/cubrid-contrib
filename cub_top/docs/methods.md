# 동적 힙 분해 — 방법 A / B (Dynamic Heap Breakdown)

`dynamic-heap`은 cub_server의 익명 힙에서 선할당 버퍼(`data_buffer`, `log_buffer`)와 스레드 스택을 제외한 나머지입니다. 정렬 버퍼·각종 캐시·락 테이블·세션·MVCC 스냅샷·질의 작업 메모리가 여기 섞여 있고, `/proc`만으로는 한 덩어리로만 보입니다.

CUBRID에는 내장 메모리 모니터(`cubrid memmon`)가 있지만 `enable_memory_monitoring=yes` + 재기동이 필요하고 할당마다 태깅 오버헤드가 붙습니다. cub_top은 **서버 변경 없이** 두 방법으로 이 덩어리를 분해합니다.

---

## 방법 B — 루트 전역 순회 (정확)

엔진의 전역 변수를 직접 읽어 서브시스템별 사용량을 얻습니다.

### 원리

```
1. /proc/<pid>/maps 에서 libcubrid.so.11.x 로드 베이스 확보
2. nm 으로 심볼의 링크 주소(st_value) 확보
3. 런타임 주소 = 로드 베이스 + st_value          (PIE 보정)
4. process_vm_readv 로 해당 주소에서 필드 상태 읽기    (정지 없음)
```

### 확보된 루트와 오프셋

CUBRID 11.5 빌드의 DWARF(`readelf --debug-dump=info`)에서 추출한 값입니다.

| 서브시스템 | 심볼 | 필드 | 오프셋 | 산출 |
|---|---|---|---|---|
| 페이지 버퍼 | `_ZL10pgbuf_Pool` | `num_buffers` | 0 | `num_buffers × DB_PAGESIZE` |
| **플랜 캐시(XASL)** | `xcache_Global` | `entry_count` | 440 | 엔트리 수 |
| | | `memory_usage_cache` | 484 | **엔진이 직접 세는 바이트** |
| | | `memory_usage_clone` | 488 | 같음 |
| 결과(list) 캐시 | `_ZL16qfile_List_cache` | `n_entries` | 24 | 엔트리 수 |
| | | `n_pages` | 28 | `n_pages × DB_PAGESIZE` |
| | `_ZL27qfile_List_cache_entry_pool` | `n_entries` | 8 | `× 4096`(고정 슬롯) |
| 필터 술어 캐시 | `_ZL21fpcache_Entry_counter` | (INT32 자체) | 0 | `× 120` |
| 락 매니저 | `lk_Gl` | `config.num_trans` | 0 | `× 152` |
| 세션 테이블 | `_ZL8sessions` | `lf_freelist.alloc_cnt` | 12 | `× 312` |
| 카탈로그 캐시 | `_ZL15catalog_Hashmap` | `lf_freelist.alloc_cnt` | 12 | `× 48` |
| 연결 엔트리 | `_ZL16css_Num_max_conn` | (INT32 자체) | 0 | `× 480` |
| | `_ZL19css_Num_active_conn` | (INT32 자체) | 0 | 활성 수 |
| VACUUM | `_ZL11vacuum_Data` | (구조체 전체) | — | 184B |

### DWARF에서 얻은 sizeof

| 구조체 | 바이트 | 구조체 | 바이트 |
|---|---|---|---|
| `pgbuf_buffer_pool` | 648 | `session_state` | 312 |
| `xasl_cache_ent` | 264 | `catalog_entry` | 48 |
| `qfile_list_cache_entry` | 296 | `lk_entry` | 128 |
| `qfile_list_cache` | 48 | `lk_res` | 120 |
| `fpcache_ent` | 120 | `lk_tran_lock` | 152 |
| `css_conn_entry` | 480 | `mvcc_snapshot` | 80 |
| `vacuum_data` | 184 | `sort_param` | 600 |

### 정확도가 가장 높은 지점

`xcache_Global.memory_usage_cache` / `memory_usage_clone`은 **엔진이 스스로 카운팅하는 바이트 값**입니다. 추정이 아니라 그 값을 그대로 읽으므로 오차가 없습니다.

### 락프리 해시맵 주의

`sessions`, `catalog_Hashmap`, `fpcache_Hashmap`, `lk_Gl.m_obj_hash_table`은 `cubthread::lockfree_hashmap<K,V>`이고, 런타임에 **구현 두 개 중 하나**를 씁니다(`enable_new_lfhash`).

- **OLD 경로**: 임베드된 `lf_freelist`의 `alloc_cnt @12` (현재 구현이 사용)
- **NEW 경로**: `lockfree::freelist`의 `m_alloc_count` (오프셋 다름)

경험적으로 OLD 경로가 활성임을 확인했습니다(`sessions+12 = 102`가 실제 세션 수와 일치). 다른 환경에서는 `m_type`을 먼저 읽어 판정해야 합니다.

### 전역 루트에서 도달 불가한 것

정렬 버퍼(`sort_param.internal_memory`), 해시조인/그룹바이 해시테이블, heap scan cache는 **질의 실행 컨텍스트와 스레드 스택에 매달려** 있어 전역 변수에서 포인터로 도달할 수 없습니다. 방법 B로는 원리적으로 잡히지 않습니다.

---

## 버퍼풀 버퍼 제어블록(BCB) 상태 읽기 (pgbuf) —

방법 B 의 원리(전역 → 포인터 → 구조체 상태 읽기)를 **배열**에 적용해, 버퍼풀의 페이지 단위 상태를 읽는다.
목적은 `cub_volmap` 과의 연속성이다: 볼륨 지도(디스크) 위에 "지금 버퍼에 있는 페이지 / 아직 안 내려간 페이지"를 얹는다.

### 읽는 것

| 단계 | 대상 | 방법 |
|---|---|---|
| 1 | `pgbuf_Pool.num_buffers`, `pgbuf_Pool.BCB_table`(포인터) | 방법 B 와 동일(심볼 + 오프셋 표) |
| 2 | BCB 배열 통째 (`num_buffers × sizeof(pgbuf_bcb)`) | `process_vm_readv` 1회(512MB 버퍼 = 32,768 × 144B ≈ 4.7MB) |
| 3 | BCB 마다 `vpid`, `flags`(bit31 dirty·bit30 flushing·bit16-19 존), `oldest_unflush_lsa`, `iopage_buffer` 포인터 | 배열 안에서 디코드 |
| 4 | 각 iopage 의 페이지 헤더 16B (`prv.lsa`, `pageid`, `volid`) | `process_vm_readv` gather (iov 1024개 × 32회) |
| 5 | `log_Gl.append.nxio_lsa`(디스크 반영 경계), `log_Gl.hdr.append_lsa`(꼬리), `hdr.eof_lsa` | 8B × 3 |

### 오프셋 (dwoff 추출 — 버전마다 다르다)

| 키 | 11.5.0 | 11.4.5 | 10.2.17 | 의미 |
|---|---|---|---|---|
| `OFF_PGBUF_BCBTAB` | 8 | 8 | 8 | `pgbuf_Pool.BCB_table` |
| `SZ_PGBUF_BCB` | **144** | **128** | 128 | `sizeof(pgbuf_bcb)` — 배열 stride |
| `OFF_BCB_VPID` | 44 | 44 | 44 | `vpid` (pageid 4B + volid 2B) |
| `OFF_BCB_FLAGS` | **64** | **60** | 60 | `flags` |
| `OFF_BCB_OLDEST_LSA` | 128 | 112 | 112 | `oldest_unflush_lsa` |
| `OFF_BCB_IOPAGE` | 136 | 120 | 120 | `iopage_buffer` 포인터 |
| `OFF_IOBUF_IOPAGE` | 8 | 8 | 8 | `pgbuf_iopage_buffer.iopage` |
| `OFF_LOG_NXIO_LSA` | 48 | 48 | 48 | `log_Gl.append.nxio_lsa` |
| `OFF_LOG_APPEND_LSA` | 280 | 280 | 264 | `log_Gl.hdr.append_lsa` |
| `OFF_LOG_EOF_LSA` | 488 | 488 | 472 | `log_Gl.hdr.eof_lsa` |

`pgbuf_bcb` 는 변수 루트가 없는 타입(포인터 너머)이라 dwoff 에 **type+member** 형(`{"OFF_BCB_FLAGS", NULL, "flags", "pgbuf_bcb"}`)을 추가했다.
`log_Gl` 은 헤더 선언이 CU 마다 반복되는 extern 전역이라 후보 DIE 가 수십 개다 — "정의가 정확히 1개" 또는 "전 후보가 같은 (타입명, sizeof) 로 해소"일 때만 채택하도록 다후보 규칙을 완화했다(동명 정적 변수는 여전히 거부).

LSA 는 `struct log_lsa { int64 pageid:48; int64 offset:16; }` 8바이트다(리틀엔디안 비트필드: 하위 48비트 pageid, 상위 16비트 offset). NULL = 전 비트 1.

### 자기검증 (틀린 오프셋으로 그림을 그리지 않기 위한 장치)

| 검사 | 임계 | 위반 시 |
|---|---|---|
| L4 pgbuf 게이트 (기존) | `num_buffers == paramdump data_buffer_pages` | 방법 B 전체 비활성 → pgbuf 도 비활성 |
| VPID 위생 | volid ∉ [-1, 255] 또는 pageid < -1 인 BCB 가 1% 초과 | pgbuf 비활성 + 사유 |
| **페이지 헤더 대조** | iopage 헤더의 (pageid, volid) ≠ BCB.vpid 가 5% 초과(100개 이상일 때) | pgbuf 비활성 + 사유. 소량 불일치는 교체 중인 페이지(정상) |
| 로그 위생 | append ≥ nxio, pageid ≥ 0 | 로그 항목만 생략 |

실측(11.5, 32,768 BCB): 위생 위반 0, 헤더 불일치 0, 읽기 실패 0, 소요 8~17ms.

### 비용과 희석
- 512MB 버퍼: 프레임당 약 4.7MB 읽기 + gather 32 syscall = 8~17ms (0.5초 프레임의 2~3%).
- 262,144 BCB(4GB) 초과 시 `ceil(nbuf/262144)` 프레임마다 1회로 자동 희석하고 사이 프레임은 직전 스냅샷을 유지한다.
- 래치를 잡지 않는 스냅샷이다. 틱 사이의 변동은 놓치며, 등급은 ◌(추정)로 표기한다.

### `--bcb-dump FILE` 스냅샷 파일 (cub_volmap 과의 계약)

`FILE.tmp` 에 쓰고 `rename` 으로 원자 교체한다. 라이브는 매 프레임, 1회 실행은 1회.

```
헤더 112B (LE):  magic "CBCBMAP1" | u32 version=1 | u32 reclen=32 | i64 ts_sec | i64 ts_nsec
                 | i32 nbuf | i32 nrec | i32 dirty | i32 pagesize
                 | u64 log_append | u64 log_nxio | u64 log_eof | u64 oldest_dirty  (raw LSA, NULL=~0)
                 | char db[32]
레코드 32B × nrec: i16 volid | i16 zone(1/2/3 LRU, 4 invalid, 8 void) | i32 pageid | u32 flags
                 | i32 hdr_ok | u64 page_lsa(메모리 페이지 헤더) | u64 oldest_lsa(BCB)
```

`cub_volmap --bufmap FILE` 은 db 이름이 다르면 거부하고, 레코드를 볼륨별 페이지 배열로 펴서 배경색 층(청록=상주, 자주=dirty)과
페이지 박스 제목(`buf:DIRTY unflushed` = 메모리 LSA > 디스크 LSA)에 쓴다.

---

## 방법 A — glibc 청크 히스토그램 (추정)

메인 힙과 스레드 아레나를 청크 단위로 순회해 **크기 분포**를 만들고, 알려진 `sizeof`와 매칭해 라벨을 붙입니다.

### 원리

```
1. /proc/<pid>/maps 에서 [heap] + 익명 rw 영역 수집
   - 단, data/log 버퍼로 매칭된 매핑은 제외 — pgbuf 풀도 내부적으로 malloc 청크라
     걷히기 때문에, 제외하지 않으면 region 귀속과 이중 계상됨
2. 각 영역의 앞 128K 만 읽어 유효한 청크 체인의 시작점 탐색(휴리스틱 lock-on)
   - 시작점이 없는 영역(대부분)은 여기서 끝 — 영역 전체를 복사하지 않음
3. 발견 시 64K 읽기 창을 청크 경계 따라 이동(헤더-홉)하며 size 수집
   - 필요한 바이트는 청크당 16B 뿐 → 수백 MB 영역도 복사가 창 몇 개로 끝남
   - glibc 청크 헤더: [prev_size(8B)][size(8B)|플래그 3비트]
   - 하위 3비트 = PREV_INUSE / IS_MMAPPED / NON_MAIN_ARENA → 마스킹
4. 크기 클래스별 합계 → 카테고리 라벨
```

실측(C): 벌크 선복사 593MB 피크 → **14MB**.

**큰 청크에서는 헤더만 집어 읽는다.** 직전 청크가 창(64KB)보다 크면 다음 헤더 **16바이트만**
읽습니다. 청크가 클 때(부하 후 평균 26KB) 창 방식은 걷은 힙 전체를 복사하므로, 6.8GB 힙에서
창 방식 1,617ms 대비 **165ms**(범위 58~508ms)입니다 — 청크 수와 합계 바이트는 완전히 동일합니다. 반대로 창을 키우면(256KB~4MB) 복사량이 늘어 **더 느려집니다**.

### 크기 라벨

| 청크 크기 | 사용자 데이터 | 라벨 | 근거 |
|---|---|---|---|
| 65552 | 65536 (64K) | `db_private heap base` | 스레드별 dlmalloc mspace base — lea가 `USE_MALLOC_INSTEAD=1`이라 glibc 청크로 노출 |
| 32784 | 32768 (32K) | `page-pair block` | 2 × DB 페이지(16K). 부하 시 지배적 |
| 96 | 80 | `MVCC snapshot` | `sizeof(mvcc_snapshot)=80` |
| ≥1M | — | `large work block` | 정렬·해시 작업 영역 추정 |
| 64K~1M | — | `query working mem` | 질의 작업 버퍼 |
| 8K~64K | — | `working buffer` | |
| <8K | — | `small/misc` | 개별 malloc |

### 32KB 블록 — 검증된 사례

부하 중 힙의 최대 소비자로 32KB 청크 약 2만 개(625MB)가 관측되었습니다. 허상이 아님을 세 가지로 확인했습니다.

1. **진짜 청크 헤더** — `size=0x8015`, 하위 3비트는 `PREV_INUSE|NON_MAIN_ARENA` 플래그이고 마스킹하면 정확히 `0x8010 = 32784`.
2. **실제 상주** — 해당 64MB 아레나가 `Rss 65,508KB`로 전량 상주.
3. **실제 데이터 보유** — 블록 내용에 벤치마크 데이터셋의 UTF-8 행 값이 담겨 있음.

크기가 DB 페이지(16K)의 정확히 2배이고 스레드별 아레나에 있으므로, 워커가 질의 처리 중 잡는 페이지 배수 작업 버퍼로 해석합니다.

### 방법 A의 구조적 과대계상

`A total`이 `dynamic-heap` 상주보다 큽니다(실측 예: 67.2M vs 44.5M, overcount 22.7M). 화면에 `overcount`로 표기하며 원인은 둘입니다.

1. **free 청크 포함** — 워커가 in-use 비트를 검사하지 않아 해제된 청크도 셈
2. **비상주 페이지 포함** — 청크 크기는 매핑 기준이라 아직 터치되지 않은 부분 포함

data/log 버퍼 매핑은 청크 워크에서 제외하므로, 버퍼를 힙으로 이중 계상하지는 않습니다.

따라서 A는 **분포와 상대 비중을 보는 용도**이며, 절대량은 상한으로 읽어야 합니다.

### freelist 블록 할당은 A로 안 보임

`session_state`(312B)는 A 히스토그램에서 **0개**로 나옵니다. 세션·카탈로그·캐시 엔트리는 freelist가 블록 단위로 미리 잡아두므로 개별 청크로 존재하지 않습니다. 이들은 **방법 B로만** 정확히 잡힙니다.

---

## 왜 하이브리드인가

두 방법은 경쟁 관계가 아니라 상호보완입니다.

| 대상 | A | B |
|---|---|---|
| 플랜/list/필터 캐시 | ✗ (블록 할당) | ✓ 정확 |
| 락 테이블·세션·카탈로그·연결 | ✗ (블록 할당) | ✓ 정확 |
| 정렬·해시조인·scan cache | △ 크기 클래스만 | ✗ (전역 루트 도달 불가) |
| db_private 계층 | ✓ 64K base 라벨 | ✗ |
| glibc 오버헤드·단편화 | ✓ | ✗ |
| 속도 | 수 초 (힙 크기 비례) | µs |
| 버전 종속성 | 낮음(glibc 포맷) | 높음(구조체 오프셋) |

화면에서는 좌우로 나란히 놓아 **순서상 우열이 없음**을 드러내고, A 박스 하단에 `A total`과 `overcount/unexplained` 차이를 함께 표시합니다.

## 재생 시의 복원 (`rp_apply`)

재생은 측정이 아니라 **복원**이다. 한 프레임의 JSON 한 줄에서 `proc_t`·`mem_t`·`tier_t`·`iod_t`
와 영역 배열·버퍼풀·용량 축·힙 항목을 되살려, 라이브와 같은 렌더러에 넘긴다.

| 복원 대상 | 키 | 비고 |
|---|---|---|
| OS·프로세스·티어 | `os.*` `server.*` `tier.*` | 1:1 대응 |
| 영역 | `region.<슬러그>.*` | 슬러그→표기명 역매핑(`thread_stacks` → `thread stacks`) |
| 버퍼풀 | `pgbuf.*` | 존·볼륨별 상주는 있는 키만 채움 |
| 용량 축 | `cap.*` | 상한은 **칸별 매트릭스** 전체 |
| 동적 메모리 B | `heapB.<슬러그>_bytes` | 슬러그→항목명 역매핑 |

슬러그 역매핑이 필요한 이유는 terse 키가 화면 표기명을 `slugify()` 한 것이기 때문이다
(화면은 한/영이 바뀌지만 키는 고정 ASCII여야 모니터링이 깨지지 않는다 — `render_terse()` 주석 참조).
역매핑 표에 없는 항목은 **복원하지 않는다**. 이름을 추측해 만들어내지 않는다.

측정에 쓰이는 값 중 **재계산이 가능한 것은 저장값을 쓰지 않고 다시 계산한다.** 프로파일 칸이
그 예로, `req_kb`/`read_ratio` 에서 `cap_bs_idx()`/`cap_rw_idx()` 로 구한다.


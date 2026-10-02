# 온디스크 포맷 지식 (On-Disk Format Notes)

볼륨 바이너리를 직접 해석하는 데 필요한 포맷 지식을 정리한다. 구조체 정의 자체는
`src/storage_ondisk_layout.hpp`가 원본이며(오프셋은 컴파일러가 계산), 이 문서는
그 배경 지식과 실측으로 확정한 사실을 기록한다. 파일 매니저 재설계(10.1, CBRD-20185)
이후 이 레이아웃은 불변이라 10.0~11.5 전 버전에 적용된다.

## 1. 페이지 공통 구조

```
┌─ FILEIO_PAGE_RESERVED (prv, 32B) ─┬─ 사용자 영역 (user_size) ─┬─ watermark ─┐
│ lsa(8) pageid@8(i32) volid@12(i16)│                           │    (8B)     │
│ ptype@14(u8) pflag@15(u8) ...     │                           │             │
└───────────────────────────────────┴───────────────────────────┴─────────────┘
user_size = iopagesize - sizeof(prv) - sizeof(watermark)   (16KB 페이지 → 16,344B)
```

- `prv.pageid == 실제 페이지 위치` 자기검증으로 미기록/재사용 페이지를 걸러낸다.
- `ptype`: PAGE_FTAB(1)·PAGE_HEAP(2)·PAGE_BTREE(10) 등 — `PAGE_TYPE` enum(storage_common.h).
- `pflag & 0x3`: TDE 암호화(AES=0x1, ARIA=0x2). 셋이면 내용 해석 불가 — 감지만 한다.

## 2. 볼륨 헤더와 섹터 할당 테이블

- `DISK_VOLUME_HEADER`는 magic 문자열("CUBRID/Volume")로 검증. 섹터=64페이지=1MB.
- 섹터 할당 테이블(stab): 1비트/섹터, LSB-first, `stab_first_page`부터 `stab_npages`.
  12GB 볼륨 기준 16KB 남짓 — 이것이 "읽기 최소"의 근거.
- 임시 볼륨(`<db>_t<N>`)은 `*_vinf`에 등재되지 않는다 — 디렉터리 패턴 스캔으로 자동 포함.

## 3. FILE_HEADER와 파일 자기발견 (실측 확정 오프셋, x86-64/prv=32)

| 필드 | 위치 (user 영역 기준) |
|---|---|
| self VFID | @8 — int32 fileid + int16 volid |
| n_page_total..n_sector_empty + ftype + flags | int32×11 @104 |
| volid_last / offset_to_partial / full / user_page_table | int16×4 @148 |
| 파일 디스크립터 (힙=class OID, 인덱스=BTID) | @40 |
| vpid_sticky_first (인덱스 루트 페이지) | — 인덱스명 해석에 사용 |

자기발견 절차: 예약 섹터의 **첫 페이지**만 프로브(prv.ptype==PAGE_FTAB) →
FILE_HEADER 파싱 → **self VFID == 자기 VPID** 일치 시 파일 확정. `file_create`의 할당
순서상 파일 헤더는 자기 첫 예약 섹터의 첫 페이지에 위치하므로 섹터당 1페이지 프로브로
충분하다 (`--full-sweep`은 예외 대비 안전망).

## 4. 파일 섹터 테이블 (extdata 체인)

- `FILE_EXTENSIBLE_DATA` 헤더 16B(next VPID + int16×3). **체인은 볼륨 경계를 넘는다** —
  next VPID의 볼륨으로 따라가야 한다 (놓치면 소유자 미상 섹터가 생긴다).
- `FILE_PARTIAL_SECTOR` 16B = VSID(8) + UINT64 페이지 비트맵. full 테이블 항목은 VSID 8B.
- 이 비트맵이 "예약 대비 실할당"(reserve waste)과 페이지 단위 지도(L1)의 데이터 원천.

## 5. 슬롯 페이지 (SPAGE)

```
SPAGE_HEADER: num_slots(i16) num_records(i16) anchor(i16) alignment(u16)
              total_free(i32) cont_free(i32) offset_to_free_area(i32) ...
SPAGE_SLOT (4B, 페이지 끝에서 역방향):
              offset_to_record:14 | record_length:14 | record_type:4
```

- total_free/num_records가 페이지 선두에 영속되므로 레코드 밀도는 헤더만 읽어 얻는다.
- record_type: HOME(2) NEWHOME(3) RELOCATION(4) BIGONE(5) MARKDELETED(6) DELETED_WILL_REUSE(7).
- 삭제 슬롯 판정: type 6/7 또는 offset_to_record==0.

## 6. OR 레코드와 이름 해석

- **OR 정수는 네트워크 바이트오더**(`or_put_int`=htonl) — 파싱 시 bswap 필수.
- 클래스 레코드의 **첫 가변속성이 클래스명**(`or_class_name`과 동일 지식).
  가변 오프셋 테이블은 repid 상위 비트의 offset-size 플래그(1/2/4B)를 따른다.
- `REC_RELOCATION`의 포워딩 목적지는 `REC_NEWHOME`; 대형 레코드는 `REC_BIGONE` →
  오버플로 first-part(next_vpid+length 뒤 data)에서 이어붙여 파싱한다.
- 인덱스명: 클래스 레코드 property list에 BTID가 `"vol|file|rootpage"` 문자열로 저장된다.
  파일의 VFID+vpid_sticky_first로 needle을 만들어 찾고, 직전의 길이-접두 식별자 문자열이
  인덱스명이다 (≤512B 역방향 탐색).

## 7. MVCC 레코드 헤더 (dead version 판정)

레코드 선두 int32 `repid_and_flags`(빅엔디안)의 상위 5비트(비트 24–28)가 MVCC 플래그다:

| 플래그 | 값 | 의미 |
|---|---|---|
| OR_MVCC_FLAG_VALID_INSID | 0x01 | insert MVCCID 보유 |
| OR_MVCC_FLAG_VALID_DELID | 0x02 | **delete MVCCID 보유 = 미바큠 dead version** |
| OR_MVCC_FLAG_VALID_PREV_VERSION | 0x04 | 이전 버전 LSA 보유 |

vacuum이 수행되면 DELID가 제거(레코드 삭제 또는 헤더 정리)되므로, DELID가 남아 있는
HOME/NEWHOME 레코드 수 = "미바큠 dead version 수"다. PAGE_HEAP에만 적용한다
(BTREE 레코드는 레이아웃이 다르다). BIGONE의 MVCC 헤더는 오버플로 페이지에 있어 제외.

## 8. 버전별 볼륨 포맷 변천 (10.0 → 11.5)

volmap이 파싱하는 볼륨 포맷의 성립·변화 이력. 근거는 CUBRID 소스 git 태그 간
구조체 직접 diff다 (`DISK_VOLUME_HEADER` / `FILE_HEADER` / `FILEIO_PAGE_RESERVED`).

| 버전 | 볼륨 포맷 변화 | volmap 영향 |
|---|---|---|
| ~10.0 (9.x 포함) | **구포맷** — 페이지 할당 비트테이블(`page_alloctb`), 볼륨 purpose 4종(DATA/INDEX/GENERIC/TEMP), `used_data/index_npages` 카운터, 파일은 allocset 체인(extdata 없음) | **미지원.** 자기검증이 거부 — 신포맷의 `sect_npgs`(=64) 오프셋이 구포맷에선 `purpose`(0~3)라 값 검사에서 탈락 |
| **10.1** (2016-11) | **현행 포맷 성립** — 디스크/파일 매니저 재설계(CBRD-20185, cc368329d). 섹터(64페이지=1MB) 예약 비트테이블 `stab`, purpose 2종(PERMANENT/TEMPORARY), `FILE_HEADER`+extdata 파일 구조. 페이지 prv 끝 8B는 미사용 reserved | 지도·요약·자기발견·--check 전부 동작하는 최소 버전 |
| **10.2** | **페이지 워터마크 도입** — 페이지 끝에 `FILEIO_PAGE_WATERMARK` 8B 추가(CBRD-22231, d81b071e8). 사용자 영역이 그만큼 줄어든다 | 실측 검증 버전 |
| **11.0** | **TDE 도입** — prv의 reserved 1B가 `pflag`(암호화 비트)로, reserved 8B가 `tde_nonce`로 전환 (**prv 32B 크기 불변**). 워터마크는 이미 10.2 에 있다 → **사용자 영역 8B 감소**(`iopagesize − prv − 8`) | `E` 표기·TDE 소견의 근거. 사용자 영역 크기 기준 변화(아래 캐비엇) |
| 11.2 / 11.3 | 변화 없음 | 11.3 실측 검증 |
| **11.4** | 볼륨 헤더에 `vol_creation`(INT64) 추가 — CBRD-25365(44c022c31), `db_creation` 뒤 삽입 | **무영향** — volmap 이 읽는 필드는 그 앞에 있다 | 11.4 / 11.4.5 실측 검증 |
| 11.5 | 변화 없음 | 11.5 실측 검증 | <!-- (iopagesize/volid/purpose/sect_npgs/nsect_total/stab_*/sys_lastpage)는 전부 삽입점 앞 |

`FILE_HEADER` 는 10.1 → 11.5 구조체 diff 가 동일하다 — 파일 자기발견·이름 해석·temp 생성
시각이 그 구간에서 같은 코드로 동작하는 근거.

> **develop 은 다르다.** CBRD-27308(1ea077d85)이 `FILE_HEADER` 를, CBRD-26176(e84a7f6dc)이
> `SPAGE_HEADER` 를 바꿨다. 사본(`storage_ondisk_layout.hpp`)을 쓰는 부분은 이 변경이
> **컴파일로 드러나지 않으므로**, develop 볼륨을 다룰 때는 이 표를 먼저 확인해야 한다.

**캐비엇 — 10.1 볼륨의 슬롯 단위 기능**: volmap은 사용자 영역 크기를 10.2+ 기준
(`iopagesize − prv − 8`, 워터마크 포함)으로 고정한다. 워터마크가 없는 **10.1** 볼륨은
슬롯 배열 끝 기준이 8B 어긋날 수 있어, **슬롯 뷰·`--deep` 밀도·del/dead·오프라인 이름
해석**은 10.1 에서 부정확할 수 있다(지도·요약·자기발견·--check는 슬롯 배열을 쓰지 않아
무관). 볼륨 헤더에 버전 필드가 없어(magic은 "CUBRID/Volume" 고정) 자동 판별은 불가 —
향후 과제: 슬롯 헤더 sanity 실패 시 −8 없이 재시도하는 폴백 휴리스틱.

## 9. 유틸리티 등록 계약 (CUBRID 트리)

`ua_Utility_Map`(util_admin.c)은 **enum 값 = 배열 인덱스** 계약이다 — 중간 삽입은
`util_get_utility_index`의 인덱스 참조를 어긋나게 해 segfault를 유발한다.
유틸리티 추가는 반드시 배열 끝 + enum의 대응 위치에.

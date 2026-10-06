# 사용법 (Usage)

## 1. 옵션 레퍼런스

```
cubrid volmap [옵션] <database-name | vinf-경로>
cub_volmap [옵션] <database-name | vinf-경로>

  -w, --width=N        그리드 가로 칸 (배치 출력용; 기본 자동, 최대 240, 8칸 단위 스냅)
  -r, --rows=N         볼륨당 줄 수 (배치 출력용; 기본 20)
      --wide           136칸 고정 (140을 8칸 스냅한 값; 파이프·파일 출력용)

> `-w`·`-r`·`--wide` 는 배치 출력을 위한 것이다. 인터랙티브(`-i`)에서 함께 주더라도
> 오류로 막지는 않으며, 화면 크기에 맞춘 자동 배치가 우선한다.
  -V, --volume=N[,N]   지정 볼륨만 출력
  -f, --full           1칸=1페이지(16KB), 줄 수 무제한
  -m, --residency      배경색 = OS 페이지캐시 상주 (mmap+mincore)
  -B, --bufmap=FILE    배경색 = cub_server 버퍼풀 상주(청록)/dirty(자주) — FILE 은 `cub_top --bcb-dump FILE` 스냅샷.
                       인터랙티브는 b 토글, 틱(--tick)·r 마다 파일이 바뀌었으면 자동 재읽기. db 이름 불일치 스냅샷은 거부
      --deep           전수 스캔: 레코드 밀도·포워딩 비율 (+ TUI 패널 L0 del/dead 집계).
                       스캔이 끌어들인 페이지 캐시는 종료 시 OS에 자동 반납(FADV_DONTNEED) —
                       스캔 전 mincore 스냅샷과 대조해 4KB 단위로 원래 캐시에 없던 구간만 반납하므로
                       서버가 데워둔 캐시는 건드리지 않는다. deep 요약줄에 returned/kept로 표기
      --check          무결성 소견(findings) 리포트 — 소견 존재 시 종료코드 2 (CI 연동)
      --format=json    JSON 출력 (volumes/files/findings)
      --full-sweep     파일 자기발견 2차 전 페이지 스윕 (안전망)
      --plain          ANSI 없이 ASCII
      --tick=SEC       인터랙티브 자동 새로고침 주기 (기본 2초)
      --warn-idle=PCT  볼륨 idle 공간이 PCT% 이상이면 소견(finding)으로 보고 — 종료코드 2 (용량 경보)
  -o, --output-file=F  파일 저장 (자동 plain)
  -i, --interactive    전체화면 브라우저
```

용어: **idle** = reserved − allocated 페이지(예약만 하고 아직 쓰지 않은 공간),
**owner switches** = 지도에서 톤이 반전된 횟수 = 파일 교대 배치(조각화) 지표.
소형 DB에서는 파일이 1MB 섹터 단위로 올림 예약되므로 idle%가 높게 나오는 것이 정상이다
(상단 요약에 자동 안내가 붙는다).

`database-name`은 `databases.txt`(`$CUBRID_DATABASES`)로 해석하며, 등록되지 않은 DB나
백업 볼륨은 `*_vinf` 파일 경로를 직접 지정한다. 임시 볼륨(`<db>_t<N>`)은 자동 포함된다.
셀(1칸) 크기는 항상 **섹터(64페이지=1MB)의 정수배로 스냅**되어 1.0MB/4.0MB처럼 딱 떨어진다(-f 페이지 모드 제외). `-w`/`-r`/`--wide`는 배치 출력 전용이며, 인터랙티브(-i)는 터미널 크기를 실시간 감지한다.

## 2. 인터랙티브 모드 (-i)

화면 구성·오버레이 각 박스의 해설은 [views.md](views.md) 참조. 하단 키 목록은
`[sp/rtn/tab]drill-down [1-3]box [f]iles [bksp]up [<>]volfile [r]efresh [a]uto [m]resid [l]한글 [h]elp [q]uit` 순이다
(`b` = 버퍼풀 층 토글 `--bufmap` 지정 시, `p` = 논리 체인 뷰, `g` = ASCII 테두리 — 키바에는 안 나오지만 동작한다).

`[r]` 는 지도를 다시 읽을 뿐 아니라 **볼륨 목록도 재스캔**한다. temp 볼륨
(`<db>_t<NNNNN>`)은 쿼리가 spill 하는 동안만 존재하므로, 새로 생긴 것은 목록에 넣고
사라진 것은 뺀다. 영구 볼륨(vinf 등재분)은 건드리지 않는다.

화면: 1행 범례 / 2행 `[n/N]` 볼륨 헤더 / 3행 공백 / 4행~ **라운드 프레임 지도** — 상단 테두리 우측에 할당 통계(pages/idle/owner switches), 하단 테두리 우측에 볼륨 파일 경로가 inset으로 / 하단 인스펙션 2줄 + 키 목록 /
하단 상태바 3줄. 한 볼륨 = 한 화면.

점진 스캔 순서: **보고 있는 볼륨 → 나머지는 양끝 교차**(첫 볼륨, 마지막 볼륨, 둘째, 뒤에서 둘째, …).
최신 데이터가 몰리는 마지막 볼륨이 두 번째로 완성되므로 `>`로 끝 볼륨에 바로 가도 곧 채워진다.
볼륨 내부는 순차 유지(리드어헤드 run 병합 효율).

**혼잡 자동 양보**: 백업·loaddb 등 대량 I/O와 겹치면 스캔이 스스로 감지해 물러난다(헤더에
`scanning N% (yielding)`) — 백업 처리량 보호가 스캔 속도보다 우선한다.

**3스레드 구조**: 스캔·새로고침(r/자동)·상주 재읽기는 배치 워커에서, 콜드 섹터의 오버레이 1MB
읽기는 저지연 워커의 캐시 워밍으로 돈다 — 콜드 디스크에서도 키 입력·화면은 수십 ms 안에
반응한다(콜드 새로고침 중에도 ≤0.1s 실측). 'r'을 누르면 "refreshing in background..." 후
백그라운드로 완료되며, 지도는 완료 시점에 갱신된다.

| 입력 | 동작 |
|---|---|
| 마우스 클릭 · 화살표 | 셀 인스펙션(하단 2줄) — 단일 클릭은 오버레이를 열지 않는다(**더블클릭 = space/enter와 동일한 한 단계 드릴다운**) — 페이지 범위·유형·객체명(**종류 배지 접두** ` H `schema.table / ` I `schema.table 인덱스명 (한 글자 색 칩) (배지 배경=범례 종류색: 힙=파랑·인덱스=초록, [g]는 테두리에만 관여(배지는 ASCII라 모드 무관)), 오프라인 해석)·VFID·파일 페이지 통계. temp 파일은 생성 시각 표시. **슬롯 박스에서는 선택 슬롯의 레코드 OID(`vol|page|slot`)·유형·길이·오프셋**, 볼륨 헤더 페이지(p0)에서는 **헤더 필드 디코드**(iopg/purpose/nsect/stab/chkpt_lsa/생성시각 — spacedb·서버 없이 헤더 실물 확인) |
| Enter | Space와 완전 동일 — 지도에서도, 오버레이 어느 뎁스에서도 한 단계 깊이(디스패치 전 space로 정규화) |
| Backspace | 한 단계 위로 |
| `<` `>` | 볼륨 이동 (임시 볼륨 포함, 순환; -V 지정 시 선택 볼륨만) |
| Tab | **드릴다운 오버레이** 열기/닫기 — 지도 위에 겹치는 **4박스 스택**(위=섹터 그리드, 중간=페이지 바이트 분포, 아래=슬롯). 닫으면 지도 복원 |
| Space | **한 단계 깊이**: 지도 → 오버레이(섹터 그리드) → 페이지 바이트 → 슬롯 → (한 번 더) 지도 복귀(순환). 각 단계에서 화살표로 선택을 움직이면 상태줄에 인스펙션(페이지=객체명·슬롯통계 / 바이트 셀=~오프셋·용도 / 슬롯=OID·유형·길이 / p0=볼륨 헤더 디코드). Bksp = 한 단계 위로 |
| `1` `2` `3` | 박스 포커스 직행 — 1=섹터 그리드(페이지 선택, 경계에서 이웃 섹터로 넘어감) / 2=페이지 바이트 셀 / 3=슬롯 |
| Enter / Bksp (포커스 중) | Enter = Space와 동일(전진) / Bksp = 후퇴. **슬롯(최심부)에서는 화살표=선택뿐이고 space/bksp 어느 쪽이든 지도 복귀** |
| `[` `]` | (볼륨 맵에서) 패널 하단에 표시할 페이지를 커서 섹터의 할당 페이지 사이에서 순회 |
| `f` | **파일 뷰** — 볼륨의 파일 목록 오버레이(이름·섹터 수). 위/아래로 고르면 **그 파일 소유 셀은 남색 배경으로 마킹, 나머지는 회색**(volume→file→sector 역방향 탐색). 열 때 선택 중인 것의 파일이 자동 선택되고(지도=커서 셀, **드릴다운 중=앵커 페이지의 파일**) 지도 커서 이동을 따라간다. 목록 행은 표준 종류 배지( H / I 색 칩)로 구분. space/enter=첫 섹터로 커서 점프, f=닫기. 하단바에 파일 상세(VFID·유형·섹터/페이지 통계) |
| `r` | 즉시 새로고침 (증분 — 90GB DB 기준 1회 pread 128회) |
| `a` | 자동 새로고침 토글(주기 `--tick=SEC`, 기본 2초) — 라이브 모니터 |
| `m` | 캐시 상주 배경색 토글 — **전 뎁스 적용**: 지도 + 오버레이 섹터 그리드 배경(≥80% 밝은 회색/≥30% 어두운 회색), 페이지 박스는 제목에 `res n/4` 표기. 반복 압축은 상주 음영이 다른 행을 서로 접지 않는다 |
| `h` | 화면 해석 가이드 — 지도/패널/키 설명 + **[용어 5초 사전]**(페이지·섹터·슬롯·heap·idle·dead 등을 일상 비유로) |
| `l` | **한/영 UI 전환** — 범례·키 목록·상태줄·인스펙션 문구가 한글로 (배치 출력은 영문 고정) |
| `q` / ESC | 종료 — 마지막 화면이 터미널에 그대로 남음 (모드만 복구) |

첫 실행 시 하단 상태줄이 `new here? h = guide | l = 한글`로 시작해 도움말과 한글 전환의 입구를 알려준다.
슬롯 인스펙션의 `free`/`len`/`off`는 바이트 단위(`B`)를 병기한다.
| 한글 자판 폴백 | IME 전환을 잊고 눌러도 동작: `ㅂ`=q `ㄱ`=r `ㅁ`=a `ㅡ`=m `ㅗ`=h `ㅣ`=l (두벌식 키 위치 역매핑, 그 외 한글은 무시) |

드릴다운 오버레이는 **지도 위에 겹치는 세로 4박스 스택**(폭 36, 내용 32칸=8배수)이다 — 탭 전환 없이
[sector 그리드 32×2(--deep 시 제목에 섹터 del/vac 집계)] → [page 바이트 분포(del/vac 푸터·범례 2열 정렬)] → [slots] → [slots 박스 하단에 슬롯 문자 범례] → **[info 박스]**(뎁스별 선택의 **간결한 정체 정보** live — F1 객체부/F2 클래스+~bytes/F3 OID+객체명) 네 박스가 **동시에** 보인다. **하단 상태바는 모든 뎁스에서 풀 상세 인스펙션**(크럼·객체명·슬롯/파일 통계·OID 유형/길이/오프셋)을 커서 이동마다 갱신한다, 위 박스의 선택이 아래
박스들을 실시간 갱신한다. 포커스된 박스는 제목이 굵은 시안으로 표시된다. 섹터 그리드에서 좌우 끝을
넘으면 같은 지도 셀의 이웃 섹터로 넘어가므로 멀티섹터 셀도 전부 순회된다. 바이트 셀의 `~bytes` 오프셋은
근사치다(분포가 클래스별 행 정렬이라 빈 칸이 끼는 경우). 색 계약·상주 음영·샌드위치 접기는 지도와 동일.
테두리 박스 문자는 프레임 전용(데이터 글리프는 확정폭 유지).

슬롯 뷰 문자: `H`=HOME `N`=NEWHOME `R`=RELOCATION `B`=BIGONE `D`=MARKDELETED
`d`=DELETED_WILL_REUSE `A`=ASSIGN_ADDRESS `-`=해제 슬롯.

## 3. --check 소견 리포트

```
FINDINGS
  [vol 0] reserved sectors with no owning file: 2 (first sectid 2185) - tracker/page mismatch?
  [vol 32765] allocated pages exceed reserved pages - live-volume snapshot skew (rerun/refresh)
```

| 소견 | 의미 |
|---|---|
| unknown_owner_sectors | 예약·사용 중인데 소유 파일 미발견(볼륨 메타 영역 제외 — 지도·요약·JSON 모두 동일 정의) — 트래커/페이지 불일치 의심 |
| alloc_exceeds_reserved | 라이브 볼륨 스냅샷 시차 — 재실행/새로고침 권고 |
| tde_encrypted_pages | TDE 암호 페이지 존재 — 키 없이 내용 해석 불가 |
| stale_file_entry | 헤더는 있으나 섹터 대응 실패 — 스캔 중 drop/재사용된 파일 |
| idle_over_threshold | `--warn-idle=PCT` 지정 시: 볼륨 idle 공간이 임계 이상 — 회수 후보 (지정만으로 소견 리포트+종료코드 2 계약 활성) |
| bufmap_snapshot_mismatch | `--bufmap` 스냅샷의 버퍼 페이지 중 90% 이상이 이 볼륨에 미할당 — 오래됐거나 다른 DB 의 스냅샷. 그 미만의 미할당분은 정상(해제 페이지가 BCB 에 남음)이라 볼륨 헤더 `freed N` 으로만 표기 |

## 4. --format json

```bash
volmap --format json cbench | python3 -m json.tool
```

최상위에 `db`(이름)·`timestamp`(생성 시각) 메타, `volumes[]`(볼륨별 예약/할당, `purpose`=permanent/temporary, `tde_pages_probed`=프로브된 페이지 중 TDE 암호 페이지 수,
`--bufmap` 시 `buffered_pages`/`dirty_pages`/`buffered_freed_pages`, 없으면 -1), `files[]`(VFID·유형·**해석된 테이블/인덱스명**·페이지 통계),
`findings[]`, 그리고 `--bufmap` 시 `bufmap{snapshot_epoch, num_buffers, resident, dirty, log_append_lsa, log_flushed_lsa, log_eof_lsa, oldest_dirty_lsa}` 를 담는다. 모니터링·CI 연동용.

## 5. 활용 예

```bash
cubrid volmap cbench                       # 전체 지도 + 서버 Δ 오버레이
cubrid volmap -m cbench                    # 어느 영역이 메모리에 올라와 있나
cubrid volmap -i cbench                    # a+m 켜고 loaddb/백업/스필이 볼륨·캐시를 채우는 모습 관찰
cubrid volmap -i --deep cbench             # 패널에서 섹터 단위 del/dead 집계까지
cubrid volmap --deep -V 0 cbench           # vol0 레코드 밀도·포워딩 비율 (재배치 대상 선정)
cubrid volmap --check cbench               # 무결성 소견
cubrid volmap --warn-idle=30 cbench        # idle 30%↑ 볼륨을 소견+종료코드 2로 (cron 용량 경보)
cubrid volmap --format json cbench > m.json
cubrid volmap -f -o map.txt cbench         # 페이지 단위 전량 덤프
cub_top -b --bcb-dump /tmp/cbench.bcb cbench &          # (터미널 1) 버퍼풀 스냅샷을 매 프레임 갱신
./cub_volmap -i -m -B /tmp/cbench.bcb cbench            # (터미널 2) 페이지캐시(-m)+버퍼풀(-B) 층을 한 지도에
./cub_volmap --plain --check -B /tmp/cbench.bcb cbench  # 볼륨별 buf/dirty/freed + 스냅샷 정합 소견
./cub_volmap /backup/olddb_vinf     # 서버 없는 장비에서 10.x 백업 볼륨 분석
```

## 6. 화면이 깨질 때 (Troubleshooting)

지도 글리프는 전부 반각 확정폭 문자(6점 점자 U+2800대 + 블록 요소 + ASCII)만 사용하지만,
터미널 폰트에 점자 글리프가 없거나 폰트 폴백이 전각으로 그리면 격자가 어긋나 보일 수 있다.

1. `archive/colortest.sh` 실행 — 색 256종과 글리프가 등폭으로 정렬되는지 먼저 확인
   (진단 보조 스크립트. 빌드에는 쓰이지 않는다)
2. 폰트를 점자 블록을 포함한 고정폭 폰트(D2Coding, Noto Sans Mono, JetBrains Mono 등)로 변경
3. 원격 터미널(PuTTY 등)은 문자셋을 UTF-8로 설정
4. 그래도 깨지면 `--plain` — ASCII 전용 출력으로 어떤 터미널에서도 안전

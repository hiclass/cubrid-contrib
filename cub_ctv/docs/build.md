# 빌드 (Build)

```sh
./build.sh          # src/ 에서 gcc -O2 -Wall -Wextra -Wshadow -I. -static <build.sh 의 소스 목록> -o ../cub_ctv
```

| 항목 | 내용 |
|---|---|
| 요구 | gcc, glibc-static (정적 빌드). 외부 라이브러리 없음. 테스트: bash, python3. 서버 시험: CUBRID 설치본·csql |
| 폴백 | glibc-static 이 없으면 동적 링크로 빌드하고 `full-static unavailable - building dynamic (libc only)` 출력 |
| 동적 빌드 | `./build.sh --dynamic` — glibc 를 동적 링크(재배포 시 LGPL 의무 회피, NOTICE 참고) |
| 산출물 | `./cub_ctv` 약 1.2MB, `env -i ./cub_ctv` 실행 가능 |
| 기타 인자 | `./build.sh --list-sources` — 소스 경로 목록(테스트용). 그 밖의 인자는 usage 출력 후 종료 코드 2 |
| 커밋 | 바이너리는 `.gitignore` 로 제외 |

## 테스트

| 스크립트 | 내용 | 서버 |
|---|---|---|
| `tests/t5_replay.sh` | 기록 픽스처(`*.rec`)를 보기 6종으로 재생 == 골든(바이트), JSON 유효성, 한국어는 UTF-8 로캘에서만(그 밖은 ASCII 영문 + 경고) | 불필요 |
| `tests/t6_view_roundtrip.sh` | 대기 체인·특수문자 이름·풀을 주입한 프레임 기록→재생 동일 (`ld --wrap`) | 불필요 |
| `tests/t11_ident_delta.sh` | 신원 변화분 기록(`K`/`X`) 재생 = 전체 기록 재생, 구간 이동 신원 = 순차 | 불필요 |
| `tests/t12_field_sched.sh` | FIELD·SCHED 규칙 단위 시험 | 불필요 |
| `tests/t13_verdict.sh` | 판정 수치 골든 비교 | 불필요 |
| `tests/t14_slot_reuse.sh` | 구간 중 슬롯 재사용(`J` 신원·`W`/`S`/`C` cpid 주입): 소유자 블록·대기 홉·세션 패널이 슬롯의 현재 클라이언트가 아닌 목격된 클라이언트를 `disconnected` 로 표시 | 불필요 |
| `tests/t15_screen_fit.py` | 요약 화면을 80×24·80×40·100×30·120×30·200×50·60×20 가상 터미널에서 한/영으로 재생: 줄바꿈까지 센 화면 줄 수가 높이 − 1 이하(스크롤 없음), 80칸 이상이면 폭을 넘는 줄 없음, 넓은 터미널은 `보호 대상` 열 유지 | 불필요 |
| `tests/t16_act_samples.sh` | 샘플러 활동 표: tran 미상(−1) 대기자 둘·엔진 스레드 둘·클라이언트 둘이 각각 따로 세어져 어느 활동도 구간 샘플 수를 넘지 않고, 그 라이브 기록이 재생됨 | 불필요 |
| `tests/t17_target_pick.sh` | 대상 선택: DB 이름 우선(숫자 이름이 다른 서버 pid 와 같아도), 같은 이름 서버 여럿은 거부하고 후보별 `--pid` 안내, DB 이름이 없는 숫자는 pid, `--pid` 는 pid 로만 (대기만 하는 `cub_server` 대체 프로세스 사용, gcc 필요) | 불필요 |
| `tests/t18_demote_holder.sh` | 샘플러에 writer 3·강등 92·비움 5 샘플을 넣어 강등 구간이 owner 의 보유로 세어지고 판정의 데몬 보유가 점유와 같은지, 일반 reader 는 보유자가 아닌지 | 불필요 |
| `tests/t19_readers_most.sh` | 보유자는 writer 전체의 보유가 점유의 절반 이상일 때만 지목: reader 94% + writer 1% → `reader 다수`·세션 조치 없음, writer 60/80 → 그 세션 지목, writer 넷이 20씩 나눠 쥔(reader 없음) CS 와 writer 40·reader 40 인 CS → reader 위주 아님 (판정 값과 pty 재생 화면) | 불필요 |
| `tests/t20_utf8_ranges.sh` | RFC 3629 둘째 바이트 범위: overlong(`E0 80..9F`, `F0 80..8F`)·서로게이트(`ED A0..BF`)·U+10FFFF 초과(`F4 90..`)는 깨짐, 경계 안쪽은 유지; `text_sanitize`·`json_fstr` 결과 JSON 이 유효 | 불필요 |
| `tests/t21_clock_use.sh` | 벽시계(`time(NULL)` 등) 사용이 `view.ts` 2곳·녹화 파일명 1곳뿐인지 소스 검사(나머지 간격·일정은 `mono_s`) | 불필요 |
| `tests/t22_os_per_cs.sh` | 스레드 10 이 LOG 를 20표본(D)·CS 0 을 79표본(R), 스레드 11 이 LOG 를 75표본(D)·CS 0 을 1표본 쥔 구간: OS 항목 4개로 나뉘고 procfs 판독 99/75 회(한 표본에 두 CS 를 쥐어도 1회), 판정 CS 0 = tran 5·스레드 10 의 CS 0 항목·R 100%. 그 밖에 100 Hz 판독 블록(20표본 → 4회, 읽기 실패도), 구간 초기화, 두 CS 를 쥔 표본의 동일 집계, reader 다수·보유 스레드 미상이면 상태 없음, 보유자 판독이 0회(읽기 전부 실패, 또는 100 Hz 에서 판독 표본에 한 번도 안 걸림)여도 항목을 `판독 0회` 로 보임·6회면 판단하지 않고 개수만 표시(화면 문구 포함)·7회면 판단, 근거 칸의 `구간이 짧아 7회 불가` 는 100 Hz 표본 20·26개와 50 Hz 16개에는 붙고 100 Hz 27개와 50 Hz 20개에는 붙지 않음(기록 헤더의 hz 로 계산), 판독 스레드 64 초과, 손상된 `O` 줄 거부(CS 범위 밖, 음수 개수, D·R·그 외의 합이 판독 수와 다름, 넘침으로만 합이 맞음, 판독 수·각 상태 개수가 구간 표본 수 초과, 관측 구간 음수; 틱 토큰은 극단값도 넘침 없이 재생)와 범위 밖 헤더 hz 거부 (`ld --wrap` 으로 대상 판독 대체) | 불필요 |
| `tests/t23_read_timing.sh` | 보유 초반은 R·나머지는 D 인 스레드를 100 Hz 로 400 구간: 판독의 D 비율이 쥔 표본의 D 비율 ±5%p 안인지 — 보유 50 ms(R 10·D 40, 쥔 표본의 80% 가 D)를 110 ms 주기와 쉼 30~70 ms 무작위로, 그리고 50 ms 의 배수 주기 셋(50 ms: 보유 30 = R 10·D 20, 66.7%; 100 ms: 보유 50, 80%; 200 ms: 보유 100 = R 20·D 80, 80%)을 블록 시작에서 0~40 ms 어긋난 다섯 위치마다. 항상 쥔 스레드는 블록(표본 5개)마다 정확히 1회 읽히고 블록 안 다섯 자리가 고르게 뽑히는지, 같은 씨앗은 같은 판독·다른 씨앗은 다른 판독, 20 Hz 이하는 모든 표본 판독, 판독 7회가 들어가는 가장 짧은 구간(100·50·20 Hz 에서 표본 27·17·7개: 한 표본 짧으면 최대 6회, 그 길이면 7회 가능)과 내내 쥔 스레드에 7회가 보장되는 가장 짧은 구간(39·23·7개: 한 표본 짧으면 6회인 구간이 있음) (`ld --wrap` 으로 대상 판독 대체, 씨앗 고정) | 불필요 |
| `tests/t7_record_equiv.sh` | 실서버에서 라이브 화면 = 기록 재생 화면, 디테일 덤프 JSON·재생 거부 | 필요 |
| `tests/t8_replay_keys.py` | pty 에서 재생 키(일시정지·구간·속도·종료), 요약 화면 세션 강조, 색상·`--plain` | 불필요 |
| `tests/t10_os_probe.sh` | OS tid·스레드 이름·대기 사유를 `/proc` 과 대조 (I-17/I-18) | 필요 |
| `tests/t9_quick_trace.sh` | 빠른 확인: 부하 중 라이브 요약 1구간 + 기록, 판정·대기 세션 검사, 라이브 `r` 녹화 시작·중지 (usage.md §3) | 필요 |
| `tests/t24_replay_inputs.sh` | `tests/inputs/*.rec`(도구가 쓰는 정상 기록 가운데 픽스처가 지나지 않는 경로: 클라이언트가 없는 유휴 서버, 신원이 바뀌지 않아 변화분이 빈 프레임)를 여섯 가지 보기로 재생해 종료 코드와 프레임 수 확인. `sanitize.sh` 에서 돌면 그 경로의 정의되지 않은 동작을 잡는다 | 불필요 |
| `tests/check_msg_pairs.py` | 한/영 메시지 쌍의 printf 지시자 동일성 | 불필요 |
| `tests/sanitize.sh` | 서버가 필요 없는 테스트 전부(t5·t6·t8·t11~t20·t22…)를 AddressSanitizer + UndefinedBehaviorSanitizer 로 빌드한 실행 파일과 테스트 프로그램으로 다시 실행(`CUB_CTV_TOOL`·`CTV_TEST_CFLAGS` 로 넘김, 첫 지적에서 중단). 종료 코드 0 = 전부 통과, 1 = 실패, 2 = 런타임(libasan·libubsan)이 없어 실행 못 함. 커밋마다 통과해야 한다 | 불필요 |
| `tests/t2_show_compare.py` | `SHOW CRITICAL SECTIONS` 브래킷 비교 + 자기영향 0 (설치 경로 자동, `CUB=`·`CUB_CTV_TOOL=` 로 지정 가능) | 필요 |
| `tests/t4_trans_check.sh` | 해제 슬롯 미표시, 라이브 trid, `cubrid tranlist` 대조 | 필요 |
| `tests/demo_contention.sh` | CS 인덱스(0–17)별 표적 경합 / 스큐 부하, 전후 증분 판정 | 필요 |

픽스처 갱신: 경합 부하(`tests/demo_contention.sh <db> 45 6 1,2,9`) 중에 `cub_ctv <db> --record tests/fixtures/demodb-snapshot.rec`, `cub_ctv <db> -i --interval 0.5 --record tests/fixtures/demodb-live.rec`(약 5초 후 Ctrl-C) 로 기록한 뒤 `tests/t5_replay.sh --update` 와 `tests/t13_verdict.sh --update` 로 골든을 다시 만든다. 기록에는 클라이언트 사용자·호스트·IP 가 들어 있으므로 공개 가능한 서버에서 기록한다.

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
| `tests/t13_verdict.sh` | 판정 수치 골든 비교. 구간 증가량: 어느 CS 의 진입 수나 대기 수가 4,096 넘게 준 프레임(리셋, 대기 없던 CS 의 리셋, 32bit 넘침, 4,097 감소)은 모든 CS 가 0, 4,096 이하 감소(1, 4,096)는 그 값만 0, 통계를 못 읽은 행은 재설정 근거가 아님, 재설정 프레임의 스크롤 행과 판정 줄(손실 0, 표본만으로 순위), 누적 시간만 준 행은 손실 0. 정체가 풀린 구간의 표식(`carried`): 진입 0·손실 600 s/s 는 1, `dt × 진입 수` 경계의 안쪽 0·바깥쪽 1, 손실 0.05 s/s 미만과 재설정 프레임은 0, 0.052·0.1 s/s 는 1, 요약 화면 문구(영/한, 2위가 있을 때와 없을 때) | 불필요 |
| `tests/t14_slot_reuse.sh` | 구간 중 슬롯 재사용(`J` 신원·`W`/`S`/`C` cpid 주입): 소유자 블록·대기 홉·세션 패널이 슬롯의 현재 클라이언트가 아닌 목격된 클라이언트를 `disconnected` 로 표시 | 불필요 |
| `tests/t15_screen_fit.py` | 요약 화면을 80×24·80×40·100×30·120×30·200×50·60×20 가상 터미널에서 한/영으로 재생: 줄바꿈까지 센 화면 줄 수가 높이 − 1 이하(스크롤 없음), 80칸 이상이면 폭을 넘는 줄 없음, 넓은 터미널은 `보호 대상` 열 유지 | 불필요 |
| `tests/t16_act_samples.sh` | 샘플러 활동 표: tran 미상(−1) 대기자 둘·엔진 스레드 둘·클라이언트 둘이 각각 따로 세어져 어느 활동도 구간 샘플 수를 넘지 않고, 그 라이브 기록이 재생됨 | 불필요 |
| `tests/t17_target_pick.sh` | 대상 선택: DB 이름 우선(숫자 이름이 다른 서버 pid 와 같아도), 같은 이름 서버 여럿은 거부하고 후보별 `--pid` 안내, DB 이름이 없는 숫자는 pid, `--pid` 는 pid 로만 (대기만 하는 `cub_server` 대체 프로세스 사용, gcc 필요) | 불필요 |
| `tests/t18_demote_holder.sh` | 샘플러에 writer 3·강등 92·비움 5 샘플을 넣어 강등 구간이 owner 의 보유로 세어지고 판정의 데몬 보유가 점유와 같은지, 일반 reader 는 보유자가 아닌지 | 불필요 |
| `tests/t19_readers_most.sh` | 보유자는 writer 전체의 보유가 점유의 절반 이상일 때만 지목: reader 94% + writer 1% → `reader 다수`·세션 조치 없음, writer 60/80 → 그 세션 지목, writer 넷이 20씩 나눠 쥔(reader 없음) CS 와 writer 40·reader 40 인 CS → reader 위주 아님 (판정 값과 pty 재생 화면) | 불필요 |
| `tests/t20_utf8_ranges.sh` | RFC 3629 둘째 바이트 범위: overlong(`E0 80..9F`, `F0 80..8F`)·서로게이트(`ED A0..BF`)·U+10FFFF 초과(`F4 90..`)는 깨짐, 경계 안쪽은 유지; `text_sanitize`·`json_fstr` 결과 JSON 이 유효 | 불필요 |
| `tests/t21_clock_use.sh` | 벽시계(`time(NULL)` 등) 사용이 `view.ts` 2곳·녹화 파일명 1곳뿐인지 소스 검사(나머지 간격·일정은 `mono_s`) | 불필요 |
| `tests/t22_os_per_cs.sh` | 스레드 10 이 LOG 를 20표본(D)·CS 0 을 79표본(R), 스레드 11 이 LOG 를 75표본(D)·CS 0 을 1표본 쥔 구간: OS 항목 4개로 나뉘고 procfs 판독 99/75 회(한 표본에 두 CS 를 쥐어도 1회), 판정 CS 0 = tran 5·스레드 10 의 CS 0 항목·R 100%. 그 밖에 100 Hz 판독 블록(20표본 → 4회, 읽기 실패도), 구간 초기화, 두 CS 를 쥔 표본의 동일 집계, reader 다수·보유 스레드 미상이면 상태 없음, 보유자 판독이 0회(읽기 전부 실패, 또는 100 Hz 에서 판독 표본에 한 번도 안 걸림)여도 항목을 `판독 0회` 로 보임·6회면 판단하지 않고 개수만 표시(화면 문구 포함)·7회면 판단, 근거 칸의 `구간이 짧아 7회 불가` 는 100 Hz 표본 20·26개와 50 Hz 16개에는 붙고 100 Hz 27개와 50 Hz 20개에는 붙지 않음(기록 헤더의 hz 로 계산), 판독 스레드 64 초과, 손상된 `O` 줄 거부(CS 범위 밖, 음수 개수, D·R·그 외의 합이 판독 수와 다름, 넘침으로만 합이 맞음, 판독 수·각 상태 개수가 구간 표본 수 초과, 관측 구간 음수; 틱 토큰은 극단값도 넘침 없이 재생)와 범위 밖 헤더 hz 거부 (`ld --wrap` 으로 대상 판독 대체) | 불필요 |
| `tests/t23_read_timing.sh` | 보유 초반은 R·나머지는 D 인 스레드를 100 Hz 로 400 구간: 판독의 D 비율이 쥔 표본의 D 비율 ±5%p 안인지 — 보유 50 ms(R 10·D 40, 쥔 표본의 80% 가 D)를 110 ms 주기와 쉼 30~70 ms 무작위로, 그리고 50 ms 의 배수 주기 셋(50 ms: 보유 30 = R 10·D 20, 66.7%; 100 ms: 보유 50, 80%; 200 ms: 보유 100 = R 20·D 80, 80%)을 블록 시작에서 0~40 ms 어긋난 다섯 위치마다. 항상 쥔 스레드는 블록(표본 5개)마다 정확히 1회 읽히고 블록 안 다섯 자리가 고르게 뽑히는지, 같은 씨앗은 같은 판독·다른 씨앗은 다른 판독, 20 Hz 이하는 모든 표본 판독, 판독 7회가 들어가는 가장 짧은 구간(100·50·20 Hz 에서 표본 27·17·7개: 한 표본 짧으면 최대 6회, 그 길이면 7회 가능)과 내내 쥔 스레드에 7회가 보장되는 가장 짧은 구간(39·23·7개: 한 표본 짧으면 6회인 구간이 있음) (`ld --wrap` 으로 대상 판독 대체, 씨앗 고정); 보유가 판독 전에 끝난 판독(owner 칸이 비었거나 다른 스레드이거나 읽지 못함)은 그 CS 항목에 세지 않고, 여전히 쥔 다른 CS 항목에는 세며, owner 재확인은 (스레드, CS) 판독마다 1회·판독 표본에서만 | 불필요 |
| `tests/t7_record_equiv.sh` | 실서버에서 라이브 화면 = 기록 재생 화면, 디테일 덤프 JSON·재생 거부 | 필요 |
| `tests/t8_replay_keys.py` | pty 에서 재생 키(일시정지·구간·속도·종료), 요약 화면 세션 강조, 색상·`--plain` | 불필요 |
| `tests/t10_os_probe.sh` | OS tid·스레드 이름·대기 사유를 `/proc` 과 대조 (I-17/I-18) | 필요 |
| `tests/t9_quick_trace.sh` | 빠른 확인: 부하 중 라이브 요약 1구간 + 기록, 판정·대기 세션 검사, 라이브 `r` 녹화 시작·중지 (usage.md §3) | 필요 |
| `tests/t24_replay_inputs.sh` | `tests/inputs/*.rec`(픽스처가 지나지 않는 판독 경로를 지나도록 손으로 만든 최소 기록: 클라이언트가 없는 유휴 서버, 신원이 바뀌지 않아 변화분이 빈 프레임, 명세 05 §10.2 의 예시 기록. 줄 모양은 도구가 쓰는 것과 같고 값은 실행에서 얻은 것이 아니다)를 여섯 가지 보기로 재생해 종료 코드 0 과 보기마다 프레임 수 확인. `sanitize.sh` 에서 돌면 그 경로의 정의되지 않은 동작을 잡는다 | 불필요 |
| `tests/t25_fuzz_replay.sh` | 손상된 기록은 거부되거나 재생되어야 하고, 크래시·멈춤·종료 코드 0/1 밖·(sanitizer 빌드에서) 목록에 없는 지적이 없어야 한다. `tests/inputs/damaged/MANIFEST` 의 고정 입력을 먼저 돌리고, 씨앗을 고정한 변형 기록 12,000개를 돌린다(`tests/fuzz_replay.py`, 일반 빌드 약 12초·매번 같은 결과). MANIFEST 의 `open` 행은 판독부가 아직 거부하지 않는 손상 입력(부호 있는 정수 넘침)으로, sanitizer 빌드에서 그 함수의 부호 있는 정수 넘침 지적이 **나와야** 통과한다 — 고쳐서 지적이 사라지면 실패하므로 그 행을 `fixed` 로 옮긴다 | 불필요 |
| `tests/t26_torn_time.sh` | 통계 블록의 누적 시간이 엔진의 저장 사이에서 읽힌 경우(`tv_usec ≥ 1,000,000`: 올림 전, 올림 뒤, 정확히 1e6): 다시 읽어 온전한 블록을 통째로 쓰고, 계속 그대로면 3회 뒤 합이 가장 작은 판독(올림 전에 읽었다가 올림 뒤로 바뀐 경우는 처음 값), 다시 읽기가 실패하면 읽은 값. 온전한 블록(`tv_usec` 999,999 포함)은 다시 읽지 않고, 다시 읽는 주소는 그 행의 통계 블록 (`ld --wrap`) | 불필요 |
| `tests/t27_engine_holder.py` | 요약 화면 `누가`·`처리량` 행: 쥔 엔진 스레드의 이름이 있으면 `엔진 스레드 <이름>`, 이름을 읽지 못했거나(그 스레드의 상태 판독 없음) 스레드를 특정하지 못하면(tran 0 만 앎) `엔진 데몬 (system tran)` — LOG 여도 로그 플러시 데몬이라 적지 않음; 스레드를 특정하지 못하면 `처리량` 행 없음(영/한) | 불필요 |
| `tests/t28_quit_restore.py` | pty 에서 대화형 재생 중 Ctrl-\ 를 보내면 터미널 모드(ICANON·ECHO)가 돌아오고, 대체 화면을 나오고(`\033[?1049l`), 커서가 보이고(`\033[?25h`), 시그널로 죽지 않고 131 로 끝남(코어 없음). Ctrl-C 는 130. SIGQUIT·SIGINT 를 무시로 넘겨받으면 Ctrl-\·Ctrl-C 로 끝나지 않고 q 로 끝남 | 불필요 |
| `tests/t29_exit_keeps_frame.py` | pty 에서 대화형 재생을 q, Ctrl-C, Ctrl-\ 로 끝냄(영/한): 대체 화면을 나온 뒤 마지막에 보이던 프레임이 글자 그대로 주 화면에 한 번 더 나오고, 그 안에 지우기·커서 이동이 없고, 마지막 글자 속성이 해제(`\033[0m`)이고 그 뒤에 글자가 없음, 커서 표시, ICANON·ECHO 복귀, 종료 코드 q 는 0·Ctrl-C 는 130·Ctrl-\ 는 131 | 불필요 |
| `tests/fuzz_full.sh` | 퍼저 전체 실행: ASan + UBSan 빌드로 씨앗 3개 × 60,000개, pty 화면 포함(8개 동시 실행으로 약 20분). 회귀에는 넣지 않고 **푸시 전**과 **기록 판독부(`src/ctv_rec.c`)나 기록 값을 쓰는 보기를 바꾸는 커밋**에서 돌린다. 새로 찾은 입력은 `tests/inputs/damaged/` 에 넣고 MANIFEST 에 행을 더한다. 종료 코드 0 = 새 지적 없음, 1 = 있음, 2 = sanitizer 빌드 실패 | 불필요 |
| `tests/check_msg_pairs.py` | 한/영 메시지 쌍의 printf 지시자 동일성 | 불필요 |
| `tests/sanitize.sh` | 서버가 필요 없는 테스트 전부(t5·t6·t8·t11~t20·t22…)를 AddressSanitizer + UndefinedBehaviorSanitizer 로 빌드한 실행 파일과 테스트 프로그램으로 다시 실행(`CUB_CTV_TOOL`·`CTV_TEST_CFLAGS` 로 넘김, 첫 지적에서 중단). 종료 코드 0 = 전부 통과, 1 = 실패, 2 = 런타임(libasan·libubsan)이 없어 실행 못 함. 커밋마다 통과해야 한다 | 불필요 |
| `tests/t2_show_compare.py` | `SHOW CRITICAL SECTIONS` 브래킷 비교 + 자기영향 0 (설치 경로 자동, `CUB=`·`CUB_CTV_TOOL=` 로 지정 가능) | 필요 |
| `tests/t4_trans_check.sh` | 해제 슬롯 미표시, 라이브 trid, `cubrid tranlist` 대조 | 필요 |
| `tests/demo_contention.sh` | CS 인덱스(0–17)별 표적 경합 / 스큐 부하, 전후 증분 판정 | 필요 |

픽스처 갱신: 경합 부하(`tests/demo_contention.sh <db> 45 6 1,2,9`) 중에 `cub_ctv <db> --record tests/fixtures/demodb-snapshot.rec`, `cub_ctv <db> -i --interval 0.5 --record tests/fixtures/demodb-live.rec`(약 5초 후 Ctrl-C) 로 기록한 뒤 `tests/t5_replay.sh --update` 와 `tests/t13_verdict.sh --update` 로 골든을 다시 만든다. 기록에는 클라이언트 사용자·호스트·IP 가 들어 있으므로 공개 가능한 서버에서 기록한다.

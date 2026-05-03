# MP3 Test Bench — STATUS

자체 검증 인프라 구현 진행 현황. Plan: `~/.claude/plans/greedy-scribbling-willow.md`.

## Goals

- 작년 본인 그룹 (G13) 의 AG가 검사하던 **38개 case** (`tmp/grade_report_cp{1,2}.md`) 자체 재현
- S-mode + U-mode trap 기반 fault recovery (NULL deref / page fault / illegal instr 시 다음 test 로 graceful 회복)
- Register clobber (callee-saved violation) 검출
- AG report 포맷과 흡사한 출력 → 작년 결과 (CP1 18/30 + CP2 7.7/40 = **25.7/70**) 와 직접 비교
- Pristine starter (mp2 reuse + locks 만 동작) → 모든 case 가 descriptive reason 으로 FAIL → 학생이 채워가면서 단계적으로 PASS 로 전환

## Phase progress

- [x] **A. Skeleton + mp2 import + locks** — `test/` dir, mp2 인프라 4개 파일 복사, mp2 reuse 파일 7개 import (plic/uart/rtc/viorng/timer/thread/thrasm), thread.c 에 lock_*/process glue 추가, test_main + tests_locks (1pt)
- [x] **B. memio + elf + cache** (12pt)
- [x] **C. vioblk** (7pt)
- [x] **D. ktfs read-only** (10pt)
- [x] **E. VM** (10pt)
- [x] **F. syscall + ktfsrw** (30pt) — 9/10 syscall PASS (sys_exit 의도적 skip), 3/3 ktfsrw PASS
- [x] **G. Verification & polish** — mutation testing 통과, CP2 demo 검증 완료

## CP2 implementation status

- CP2.1 memory.c — 6/6 VM PASS
- CP2.2 process.c — process_exec/exit working, sys_exec PASS
- CP2.3 syscall.c — 9/10 syscall PASS
- CP2.4 ktfs.c write — 3/3 ktfsrw PASS (writeat persist / create persist / delete)
- CP2 demo — `make run` 으로 trek_cp2 가 U-mode 에서 동작 (banner + interactive prompt 확인)

**Total: 69/70** (test_sys_exit skipped — process_exit halts test runner.)

## Phase G mutation testing (2026-04-30)

작년 G13 의 fail 패턴을 의도적으로 재주입해서 test bench 가 catch 하는지 검증.

| Mutation | Injected | Caught by | Score impact |
| --- | --- | --- | --- |
| **resolve_block indirect off-by-one** | `arr[logical_blk + 1]` 대신 `arr[logical_blk]` | `test_ktfs_readat_indirect`, `_dindirect`, `_direct_to_indirect` (3 cases) | 69 → 66 |
| **vioblk_writeat queue_reset assertion** | 첫 write 이후 `-EIO` 반환 | `test_vioblk_writeat_simple_1`, `_2` (2 cases) | 69 → 66 |

작년 G13 의 ktfs PANIC + writeat fail 시그너처와 정확히 일치 → test bench 가 채점 인프라로서 의미 있다는 것 확인.

## File inventory (current)

| File | Notes |
| --- | --- |
| `test_framework.h` | shared types + recovery state externs + suite registry decls |
| `excp_replace.c` | swaps `excp.o` `handle_smode_exception`; SIE re-enable + longjmp (mp2 hotfix 포함) |
| `setjmp.S` | RV64 setjmp/longjmp (mp2 그대로) |
| `clobber.S` | 6-arg trampoline + sentinel + good/bad self-test (mp2 그대로) |
| `test_runner.c` | run_test/run_test_group, AG-style printer |
| `test_main.c` | banner + kernel init + suite registry + summary |
| `tests_locks.c` | 1 case: recursive ownership + count semantics |

## Verification log

### 2026-04-30 — Phase A baseline

- `make test.elf` → 빌드 성공, `dev/vioblk.o` 만 link 제외 (virtio.c 의 weak attach 사용)
- `make run-test` (또는 직접 QEMU):
  - Banner 출력
  - Recovery smoke: NULL deref → S-mode trap → SIE re-enable + longjmp → recover, cause/sepc/stval 정확히 캡처
  - Clobber smoke: good fn mask=0x0, bad_s0 fn mask=0x1
  - Locks tests: 1/1 PASS (lock_init zero-init, recursive acquire 누적, partial release 유지, full release 후 재취득)
  - Functionality: 1/1, Penalties none, Total 1/1
- Pristine baseline 미러: 학생 작업 영역 미구현이라 다른 카테고리 0/0 (skeleton)

## Known caveats

- `dev/vioblk.c` starter 가 변수 선언 누락된 broken state — 빌드 못 함. virtio.c 의 weak `vioblk_attach` 가 대신 사용됨. Phase C 진입 시 vioblk.c 를 recover (학생이 작성).
- `cache.c` 가 starter 에 없음 → 우리가 stub (모두 -ENOTSUP) 만 추가. 학생이 Phase B 에서 진짜 구현.
- `heap0.c` 가 `alloc_phys_page` 호출 → memory.h include 추가. 학생 영역이지만 기존 starter 의 누락분.
- `ktfs.c`, `elf.c`, `memory.c`, `process.c`, `syscall.c`, `io.c` 모두 컴파일은 가능 (stub 함수들이 0/-ENOTSUP 반환). 진짜 동작은 학생 작업 후.

## Sign-off criteria

- [x] Phase A: skeleton + locks (1pt) PASS
- [ ] Phase B-F: 38 case 모두 코드 작성, pristine 에서 적절히 FAIL
- [ ] Phase G: pristine output 캡처 + mutation 검증

## Usage

```sh
cd redo/mp3/src/sys
touch ktfs.raw                    # QEMU drive option needs the file to exist
make test.elf                     # build (works on pristine starter + cache stub)
make run-test                     # run, prints AG-style report
make debug-test                   # qemu -S -s for gdb attach
```

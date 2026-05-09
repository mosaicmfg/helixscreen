# Crash 3XNZQB2R — investigation notes

Bundle: `/tmp/debug-bundle-3XNZQB2R.json` (also stored locally only)
Version: v0.99.58, pi32, host **Enderwire** (Pi 4 / 838 MB / 92 MB RSS)
Printer: Voron 2.4 + Klipper `v0.13.0-628-g373f200c-dirty`
Crashed: 2026-05-09 15:48:44, **third helixscreen crash on this device in 20 min** (15:30, 15:36, 15:48 watchdog log).

## What we know

### Pinpointed crash site: `lv_event.c:209`

```
PC      = 0x18bc5c4    (heap, non-executable, fault SEGV_ACCERR)
R3      = 0x18bc5c4    (== PC — indirect call via R3)
LR      = 0xecc899     (in .rodata between glyph_dsc and glyph_bitmap — stale)
event_target = 0x23042e0
event_code   = 10 (LV_EVENT_CLICKED)
queue_prev   = TSM::update_subjects (red herring — see project_temp_subject_atomic_crash.md)
queue_prev2  = HardwareHealthOverlay::rebuild
```

`R3 == PC` is a `blx r3` after `ldr r3, [...]`. The only place in LVGL that loads a callback into a register and calls it indirectly while an `LV_EVENT_CLICKED` is in flight on `event_target` is `lib/lvgl/src/misc/lv_event.c:209`:

```c
for(uint32_t i = 0; i < size && !e->deleted; i++) {
    lv_event_dsc_t * dsc = *event_array_at(list, i);
    if(dsc->cb == NULL) continue;
    ...
    dsc->cb(e);   // ← line 209: this is the call that crashed
}
```

So the **`dsc->cb` slot was corrupted** to `0x18bc5c4` (a heap address) and dispatched. None of the v0.99.55+ structural guards (`event_head_stale_leak`, `event_slot_misaligned`, `event_slot_stack_oob`, `obj_event_null_obj`, `obj_event_null_dsc`) fired in this bundle — confirming the array-stack fix is holding and the corruption surface has shifted to the **per-widget event handler list** (`obj->spec_attr->event_list.array` of `lv_event_dsc_t*`).

### Pre-crash crumb timeline

The user spent ~14 min navigating Settings, then crashed during Hardware Health teardown:

```
overlay Settings Panel → System → back → Advanced Panel → Settings → Display & Sound → back
  → Safety & Notifications → back → System → ... → Security Settings → back
  → Network Settings → back → overlay Hardware Health
  → 17 × async_d lv_obj    ← Hardware Health overlay teardown
  → nav Home Panel 0       ← ??? back from Hardware Health went straight to Home Panel
  → async_d lv_image, async_d lv_obj (last crumb 746801)
  → CRASH inside lv_event_send dispatching LV_EVENT_CLICKED to widget 0x23042e0
```

Earlier in the session there were **2 separate `tear_down_printer_state` cycles** (`fct stop_a/b 0` → sync_d keyboard/lv_obj/lv_dropdown-list → `modal+ klipper_recovery_dialog`) and **5 separate Klipper discovery cycles** (`disc cb_begin 1..5..7`). Klipper had been crash-looping; user did 2 soft restarts via switch_printer / wizard. By the time of the final click, the system had churned through a lot of teardown/init.

### Cluster classification

This fits **L081 / cluster:pstat-async-delete Mechanism C** (cross-thread LVGL mutation racing the main thread) but surfacing in the **per-widget event-dsc array** instead of the global event_stack. v0.99.58's #933 fix (commit `c3856d171`) only marshaled 3 specific Moonraker callbacks; the audit of "39 raw async-API callsites" recorded in `project_l081_recurrence_post_840.md:15` is unfinished. Many `tok.expired()`-then-inline-LVGL sites remain (belt_tension_calibrator, AMS backends, sensor_state, multiple panels).

## What we DON'T know — and what we'd need to learn

| Unknown | What would tell us |
|---|---|
| **What widget is `0x23042e0`?** Class? Name? Was it visible/alive? | Snapshot widget fields at crash time: `class_p->name`, `obj->spec_attr->name`, parent chain. |
| **Was `dsc` itself corrupt or just `dsc->cb`?** | 64-byte hex dump of `dsc` (read from `event_array_at(list, i)` immediately before the call) in the crash report. |
| **What was the LAST valid `dsc->cb` before this one?** Identifies the writer-vs-reader race. | Per-callback dispatch crumb: `dispatch_cb <obj_ptr> <cb_ptr> <i>` before each `dsc->cb(e)`, paired with a "post" crumb when the cb returns. Crash mid-dispatch leaves a `pre` without `post`. |
| **What was clicked?** Recovery dialog button? Hardware Health row Ignore/Save? Home button? | Click crumb in our shared `ui_button` click trampoline, naming the button. |
| **Why did `nav Home Panel 0` follow Hardware Health teardown?** Hardware Health → Home isn't the normal back stack. | NavigationManager push/pop crumbs with the actual stack depth + previous panel. |
| **Was `HardwareHealthOverlay::rebuild` running concurrently with the click?** | Enter/exit crumbs in `populate_hardware_issues` and the deferred lambda. |
| **Did a BG-thread callback touch this widget's event list?** | Anomaly crumb when `LifetimeToken::expired()` is called from a non-main thread. |

## Instrumentation proposals — ranked by leverage / effort

### Tier 1 (cheap, high-leverage, ship now if user agrees)

**A. Per-callback dispatch crumb (`lvgl/src/misc/lv_event.c`)**
   - Add `helix_crash_note_event_cb(obj, dsc, cb, i)` at line 209 BEFORE `dsc->cb(e)` and a paired `_done` after.
   - Cost: 2 crumbs per dispatched cb (high frequency — most clicks dispatch 1–3 cbs). Ring is 256 slots, so a noisy click could rotate everything else out. Mitigation: gate the crumb by event filter (only `_PRESSED/_RELEASED/_CLICKED/_VALUE_CHANGED/_DELETE`) so mouse-pressing/dragging doesn't drown the ring.
   - Diagnostic value: **the next bundle in this signature would name the cb pointer being called and the dsc, distinguishing "dsc itself corrupt" from "dsc->cb slot corrupt"**.

**B. Widget identity in `helix_crash_note_event`**
   - Extend `set_current_event` to also save `class_p->name` (read at hook time, not crash time — class_p is unlikely to be the corrupted field) and `obj->spec_attr->name` if present.
   - Cost: ~16 bytes of static; one strncpy at hook time.
   - Diagnostic value: **changes "0x23042e0 (?)" into "lv_button (recovery_dismiss_btn)" or similar**.

**C. Click-button crumb in shared `ui_button` trampoline**
   - Find the central click handler (search for the path used by `recovery_dismiss_btn` etc.) and emit `crumb("click", obj_name, 0)`.
   - Cost: 1 crumb per actual user click. Low.
   - Diagnostic value: tells us "user clicked X" right before the dispatch crumb.

### Tier 2 (medium effort, high value for confirming Mechanism C)

**D. BG-thread `tok.expired()` anomaly crumb**
   - In `LifetimeToken::expired()`, check if `pthread_self() == g_main_thread_id`; if not, emit `crumb("tok_bg", "expired_call", reinterpret_cast<long>(this))`.
   - Doesn't fire in production for code that uses `tok.defer(...)` correctly (defer dispatches the body to the main thread before reading `expired()`). Fires only for the anti-pattern.
   - Diagnostic value: **catches the anti-pattern at runtime in a way the audit can't, especially across plugin/backend code that's hard to grep**.

**E. 64-byte snapshot of `event_target` at crash time**
   - In SIGSEGV handler, after current `event_target` printing, dump 64 bytes from `event_target` (with `process_vm_readv` or careful read inside the signal handler — needs care to not crash the crash handler).
   - Diagnostic value: lets us see the vtable / class_p / spec_attr fields at crash time. If they look like ASCII or random data, the widget memory was reused.

### Tier 3 (more invasive, save for later)

**F. PC/LR ring at instrumentation points**
   - Save (PC, LR) at queue_update entry, observer fire, lv_event_send entry. When call frame is corrupted, this ring gives historical context that LR alone can't.
   - Cost: ~1 KB static + extra writes per instrumented point.

**G. HardwareHealthOverlay rebuild lifecycle crumbs**
   - `populate_hardware_issues` enter/exit, `safe_clean_children` pre/post.
   - Bundle this with similar crumbs in any panel that does deferred-rebuild-after-click (Settings sub-overlays, AMS panels, etc.).

## Why this likely won't repro on Pi-aarch64+ASAN

Per `project_l081_recurrence_post_840.md`, Pi-aarch64 + ASAN can't repro the cluster after ~10K stress cycles. This bundle continues that pattern: **pi32 (32-bit ARM)** is the dominant production surface; the bug needs production-specific conditions (memory layout, allocator behavior, slow tick cadence) we can't model on Pi-aarch64.

Strategy continues as recorded in the L081 doc: structural fixes (✓ event-stack array, ✓ #933 marshaling) + telemetry waiting. This bundle confirms a *new* surface (per-widget event-dsc array) for the same underlying corruption; field instrumentation is the only viable path to localize the writer.

## Open questions for Preston

1. **Want to ship Tier 1 (A+B+C) now?** They're cheap and would name the cb pointer + widget on the next field bundle in this signature.
2. **Tier 2 D (BG-thread expired check) seems high-value** — would be a runtime detector for the unfinished 39-callsite audit. Worth the work?
3. **Tier 2 E (memory snapshot) is the riskiest** — signal-handler-safe read of `event_target` could itself fault if `event_target` is in unmapped memory. Worth using `mincore` to gate it?
4. **Should we file a GitHub issue** for the recurrence on v0.99.58 with a "watch" status, or fold it into existing #933 / cluster:pstat-async-delete? The signature differs (per-widget array vs global stack) — might warrant its own canonical issue.

## Bundle inventory addition

Add to `project_l081_recurrence_post_840.md` "Bundle inventory" section:
```
- v0.99.58 / pi32 / Voron 2.4: 3XNZQB2R (this bundle, 2026-05-09).
  Sub-signature: per-widget event-dsc array corruption (lv_event.c:209), not global event_stack.
  Trigger context: 5 disc cycles + 2 tear_down + Hardware Health teardown + nav Home + click in flight.
  No structural-fix anomaly markers fired.
```

---

## What shipped (2026-05-09)

Decided on **Tier 1A + 1B + 2D**, dropping Tier 1C as redundant once 1A+1B both fire (the cb pointer + widget identity together identify the click).

**Tier 1A — dispatch cb-bounds gate** (`patches/lvgl_event_dispatch_cb_guard.patch`, hunk in `lib/lvgl/src/misc/lv_event.c` inside `lv_event_send`'s dispatch loop):
- Before `dsc->cb(e)`, snapshot the cb pointer (`cb_snap = dsc->cb`) and check it falls within `[text_lo, text_hi)` via `helix_get_text_bounds()`.
- Out-of-text → `helix_lvgl_anomaly("event_dsc_cb_oob", "obj=… cb=… dsc=… i=… code=…")` and `continue`.
- Snapshot closes the TOCTOU window where a bg-thread overwrite could land between the bounds check and the call.
- Best-effort: bounds returns 0 during very early init → falls back to "no check". Production bug is a runtime mutation, so this is fine.

**Tier 1B — widget identity capture** (same patch, `lib/lvgl/src/core/lv_obj_event.c` event_send_core):
- Pairs the existing `helix_crash_note_event(target, …)` hook with `helix_crash_note_event_target_id(class_p->name, spec_attr->name)`.
- crash_handler stores them in `s_current_event_target_class` (const ref into `.rodata`) and a 32-byte fixed buffer `s_current_event_target_name`.
- Crash dump now writes `event_target_class:` and `event_target_name:` lines so the next bundle reads "lv_button (recovery_dismiss_btn)" instead of "0x23042e0".

**Tier 2D — bg-thread `tok.expired()` detector** (`include/async_lifetime_guard.h`, `src/system/async_lifetime_guard.cpp`, `src/main.cpp`):
- `LifetimeToken::expired()` is `[[gnu::always_inline]]`. After computing the boolean, if `!is_expired && !on_main_thread()`, calls a `[[gnu::noinline]]` trampoline `report_bg_expired_check()`.
- The trampoline captures `__builtin_return_address(0)` *inside the noinline*, which after expired() is inlined into caller F gives the LR within F at the inlined call site — the user's source-level `tok.expired()` location. (Capturing the LR up in the inline path would have given F's caller's LR, which is the wrong frame — review caught this as B1.)
- Per-thread first-fire-only via TLS 64-slot seen-set; same (thread, LR) fires once per session. Different threads each get one report per LR.
- Main thread id captured at entry to `main()` via `helix::internal::set_main_thread_id()`. `on_main_thread()` returns true conservatively while unset (init window) so early callbacks don't false-positive.
- Anomaly emitted via `helix_lvgl_anomaly("bg_tok_expired_check", "lr=… tid=…")` — same telemetry channel as the existing event-stack guards.

**Stubs for splash/watchdog** (`tools/helix_lvgl_anomaly_stub.cpp`): added no-op stubs for `helix_crash_note_event_target_id` and `helix_get_text_bounds` so non-helix-screen binaries link cleanly without relying on weak-undef-resolves-to-NULL toolchain behavior (review caught this as B2).

**Tests** (`tests/unit/test_async_lifetime_guard.cpp`): 6 new `[bg_detector]` cases covering main-thread silence (alive + expired tokens), bg-thread correctness invariants (alive returns false, expired returns true), the tight-loop no-crash path, and `on_main_thread()` reflecting the calling thread. Anomaly fire is observable in the test log warns; no production-test coupling added.

**Code review** (general-purpose agent, 2026-05-09):
- Verdict: ship-with-fixes
- B1: noinline-trampoline LR capture — **fixed**
- B2: splash/watchdog stubs — **fixed**
- N1: TOCTOU snapshot of `dsc->cb` — **fixed**
- N2 (perf cache), N3 (saturation marker), N8 (anomaly-fire test assert) — deferred, non-blocking

**Build status:**
- Native: clean.
- pi32-docker: ARM 32-bit `pthread_t`/`uintptr_t` cast issue caught and fixed (memcpy into `uint64_t`).
- AD5M smoke deploy: pending.

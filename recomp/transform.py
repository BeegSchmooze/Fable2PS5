#!/usr/bin/env python3
"""Turn freshly generated Fable 2 recompiled sources into the exact sources this port compiles.

  transform.py GENERATED_DIR OUT_DIR

GENERATED_DIR is ReXGlue's codegen output for the player's own default.xex
(Fable2Recomp/generated/default). OUT_DIR receives every fable_2_recomp.N.cpp this port
changes; the build compiles those instead of the generated ones. Each stage below is one
change from the development history (build numbers in the names), applied in the original
order, each to the output of the stages before it:

  30/31  SpinLock_Acquire_822D7408: the guest lock's reservation load/compare-and-swap runs as a
         native atomic instead of under the runtime-wide mutex (+ LOCK-PERF probe).
  32     three reference-count routines, same change (+ REF-PERF probe).
  33     every other vetted single-CAS reservation block in the game, same change.
  54     the probes read a calibrated TSC instead of steady_clock (~1 us cheaper per read).
  62     fix for the story-moment hangs: the main thread releases critical section 0x83499138
         while it waits for the frame worker, which needs that lock to finish.
  71     frame-wait and frame-worker timers (FRAMEWAIT71 / WORKER71 log lines).
  75     menu music request logging (MENU75).
  113    main-menu music gain fix: the menu track's resolved sample gain 0.02 -> 1.0.

The script contains patterns and guest addresses only, no game code. It refuses to run
(assertion) if the generated code does not look like the version this port was made from.
Generated from the original build scripts by recomp/build_transform.py.
"""
import re
import sys
from pathlib import Path

LOCK30_HELPER = '\n#include <chrono>\n#include <cstdio>\n#include <rex/system/xthread.h>\nnamespace {\nusing SpinClock30=std::chrono::steady_clock;\nstruct SpinTotals30 { unsigned long long calls=0, ns=0, mutex_ns=0, attempts=0, max_ns=0; unsigned worst=0; SpinClock30::time_point report=SpinClock30::now(); };\nstatic thread_local SpinTotals30 spin30;\nstruct SpinProbe30 {\n SpinClock30::time_point begin=SpinClock30::now(); unsigned address;\n explicit SpinProbe30(unsigned a):address(a){}\n ~SpinProbe30(){auto now=SpinClock30::now(); auto ns=(unsigned long long)std::chrono::duration_cast<std::chrono::nanoseconds>(now-begin).count(); auto& t=spin30;\n ++t.calls;t.ns+=ns;if(ns>t.max_ns){t.max_ns=ns;t.worst=address;}\n if(now-t.report>=std::chrono::seconds(10)){\n std::fprintf(stderr,"[FABLE2-LOCK-PERF] tid=%u calls=%llu total_ms=%.3f mutex_ms=%.3f attempts=%llu max_ms=%.3f worst=0x%08x\\n",rex::system::XThread::GetCurrentThreadId(),t.calls,t.ns/1e6,t.mutex_ns/1e6,t.attempts,t.max_ns/1e6,t.worst);std::fflush(stderr);t=SpinTotals30{};}\n }\n};\n}\n'
REF32_HELPER = '\n#include <chrono>\n#include <cstdio>\n#include <rex/system/xthread.h>\nnamespace {\ntemplate<unsigned Tag> struct AtomicProbe32 {\n using Clock=std::chrono::steady_clock;\n Clock::time_point start=Clock::now();\n ~AtomicProbe32(){static thread_local unsigned long long calls=0,ns=0;static thread_local auto report=Clock::now();auto now=Clock::now();++calls;ns+=std::chrono::duration_cast<std::chrono::nanoseconds>(now-start).count();if(now-report>=std::chrono::seconds(10)){std::fprintf(stderr,"[FABLE2-REF-PERF] tid=%u routine=0x%08x calls=%llu atomic_ms=%.3f\\n",rex::system::XThread::GetCurrentThreadId(),Tag,calls,ns/1e6);std::fflush(stderr);calls=ns=0;report=now;}}\n};\n}\n'
TSC54 = '#include <chrono>\n#include <x86intrin.h>\nnamespace {\n// Build 54: steady_clock-compatible clock on the invariant TSC, calibrated\n// once per translation unit against steady_clock (2 ms, first use only).\nstruct TscClock54 {\n  using duration = std::chrono::nanoseconds;\n  using rep = duration::rep;\n  using period = duration::period;\n  using time_point = std::chrono::time_point<TscClock54>;\n  static constexpr bool is_steady = true;\n  static double ns_per_tick() {\n    static const double value = [] {\n      const auto s0 = std::chrono::steady_clock::now();\n      const unsigned long long t0 = __rdtsc();\n      while (std::chrono::steady_clock::now() - s0 < std::chrono::milliseconds(2)) {\n      }\n      const auto s1 = std::chrono::steady_clock::now();\n      const unsigned long long t1 = __rdtsc();\n      return std::chrono::duration<double, std::nano>(s1 - s0).count() / double(t1 - t0);\n    }();\n    return value;\n  }\n  static time_point now() noexcept {\n    return time_point(duration(rep(double(__rdtsc()) * ns_per_tick())));\n  }\n};\n}  // namespace\n'
DEADLOCK62_OLD = '\tctx.lr = 0x822BA584;\n\tKeWaitForMultipleObjects_NoApcRetry(ctx, base);\n'
DEADLOCK62_NEW = '\tctx.lr = 0x822BA584;\n\t{\n\t\t// Build 62: release CS 0x83499138 if this thread holds it while waiting\n\t\t// for the frame worker, which needs that lock to finish (see notes).\n\t\tconst auto saved_r3 = ctx.r3, saved_r4 = ctx.r4, saved_r5 = ctx.r5, saved_r6 = ctx.r6;\n\t\tuint32_t reenter62 = 0;\n\t\tctx.r3.u64 = 0x83499138;\n\t\t__imp__RtlTryEnterCriticalSection(ctx, base);\n\t\tif (ctx.r3.u32) {\n\t\t\tconst uint32_t depth62 = uint32_t(REX_LOAD_U32(0x83499138 + 0x14));\n\t\t\treenter62 = depth62 ? depth62 - 1 : 0;\n\t\t\tfor (uint32_t i62 = 0; i62 < depth62; ++i62) {\n\t\t\t\tctx.r3.u64 = 0x83499138;\n\t\t\t\t__imp__RtlLeaveCriticalSection(ctx, base);\n\t\t\t}\n\t\t}\n\t\tif (reenter62) {\n\t\t\tstatic uint32_t releases62 = 0;\n\t\t\t++releases62;\n\t\t\tif ((releases62 & (releases62 - 1)) == 0) {\n\t\t\t\tstd::fprintf(stderr, "[FABLE2-DEADLOCK62] released cs=0x83499138 depth=%u count=%u\\n",\n\t\t\t\t             reenter62, releases62);\n\t\t\t\tstd::fflush(stderr);\n\t\t\t}\n\t\t}\n\t\tctx.r3 = saved_r3;\n\t\tctx.r4 = saved_r4;\n\t\tctx.r5 = saved_r5;\n\t\tctx.r6 = saved_r6;\n\t\tctx.lr = 0x822BA584;\n\t\tKeWaitForMultipleObjects_NoApcRetry(ctx, base);\n\t\tif (reenter62) {\n\t\t\tconst auto result_r3 = ctx.r3;\n\t\t\tfor (uint32_t i62 = 0; i62 < reenter62; ++i62) {\n\t\t\t\tctx.r3.u64 = 0x83499138;\n\t\t\t\t__imp__RtlEnterCriticalSection(ctx, base);\n\t\t\t}\n\t\t\tctx.r3 = result_r3;\n\t\t}\n\t}\n'
TIMER71 = '\nnamespace {\nstruct Timer71 {\n  const char* tag;\n  unsigned long long count = 0, total_ns = 0, max_ns = 0;\n  std::chrono::steady_clock::time_point report = std::chrono::steady_clock::now();\n  void add(std::chrono::steady_clock::time_point begin) {\n    const auto now = std::chrono::steady_clock::now();\n    const unsigned long long ns = (unsigned long long)std::chrono::duration_cast<std::chrono::nanoseconds>(now - begin).count();\n    ++count;\n    total_ns += ns;\n    if (ns > max_ns) max_ns = ns;\n    if (now - report >= std::chrono::seconds(10)) {\n      std::fprintf(stderr, "[%s] count=%llu total_ms=%.3f avg_ms=%.3f max_ms=%.3f\\n", tag, count,\n                   total_ns / 1e6, count ? total_ns / 1e6 / count : 0.0, max_ns / 1e6);\n      std::fflush(stderr);\n      count = total_ns = max_ns = 0;\n      report = now;\n    }\n  }\n};\n}  // namespace\n'
MENU113_EXTRA = '\n // Narrow experiment: only MUSIC_MENU\'s resolved sample, preserving other audio.\n {const uint32_t def111=ctx.r5.u32;if(def111 && REX_LOAD_U32(def111+4)==0xa1faf72b && REX_LOAD_U32(def111+8)==0x16a7e22e && REX_LOAD_U32(def111+28)==0x3ca3d70a){REX_STORE_U32(ctx.r3.u32+96,0x3f800000);std::fprintf(stderr,"[FABLE2-MENU113] menu sample gain 0.02 -> 1.0\\n");std::fflush(stderr);}}\n'

ATOMIC33 = re.compile(r'ctx\.r(\d+)\.u64 = REX_CHECK_GLOBAL_LOCK\(\);(?P<mid>(?:(?!REX_CHECK_GLOBAL_LOCK|REX_ENTER_GLOBAL_LOCK|REX_LEAVE_GLOBAL_LOCK).)*?)REX_ENTER_GLOBAL_LOCK\(\);(?P<body>.*?)REX_LEAVE_GLOBAL_LOCK\(\);', re.S)
ATOMIC_LOAD = '__atomic_load_n(reinterpret_cast<uint32_t*>(REX_RAW_ADDR(ea)), __ATOMIC_SEQ_CST)'


def replace_once(text, old, new):
    assert text.count(old) == 1, old[:100]
    return text.replace(old, new)


def function_span(source, name):
    a = source.index('DEFINE_REX_FUNC(' + name + ') {')
    b = source.index('\nDEFINE_REX_FUNC(', a + 1)
    return a, b


class Units:
    def __init__(self, generated):
        self.generated = Path(generated)
        self.changed = {}

    def get(self, name):
        return self.changed[name] if name in self.changed else (self.generated / name).read_text()

    def put(self, name, text):
        self.changed[name] = text


def stage30(u):
    name = 'fable_2_recomp.166.cpp'
    source = u.get(name)
    a, b = function_span(source, 'SpinLock_Acquire_822D7408')
    fn = source[a:b]
    fn = fn.replace('REX_FUNC_PROLOGUE();', 'REX_FUNC_PROLOGUE();\n SpinProbe30 probe30(ctx.r3.u32);', 1)
    fn = fn.replace('ctx.r7.u64 = REX_CHECK_GLOBAL_LOCK();', '{ auto begin30=SpinClock30::now(); ctx.r7.u64 = REX_CHECK_GLOBAL_LOCK(); spin30.mutex_ns+=std::chrono::duration_cast<std::chrono::nanoseconds>(SpinClock30::now()-begin30).count(); }')
    fn = fn.replace('REX_ENTER_GLOBAL_LOCK();', '{ auto begin30=SpinClock30::now(); REX_ENTER_GLOBAL_LOCK(); spin30.mutex_ns+=std::chrono::duration_cast<std::chrono::nanoseconds>(SpinClock30::now()-begin30).count(); ++spin30.attempts; }')
    assert fn.count('++spin30.attempts') == 2
    u.put(name, LOCK30_HELPER + source[:a] + fn + source[b:])


def stage31(u):
    name = 'fable_2_recomp.166.cpp'
    source = u.get(name)
    a, b = function_span(source, 'SpinLock_Acquire_822D7408')
    fn = source[a:b]
    old = '{ auto begin30=SpinClock30::now(); ctx.r7.u64 = REX_CHECK_GLOBAL_LOCK(); spin30.mutex_ns+=std::chrono::duration_cast<std::chrono::nanoseconds>(SpinClock30::now()-begin30).count(); }'
    assert fn.count(old) == 2
    fn = fn.replace(old, 'ctx.r7.u64 = ctx.msr & 0x8000u;')
    old = '{ auto begin30=SpinClock30::now(); REX_ENTER_GLOBAL_LOCK(); spin30.mutex_ns+=std::chrono::duration_cast<std::chrono::nanoseconds>(SpinClock30::now()-begin30).count(); ++spin30.attempts; }'
    assert fn.count(old) == 2
    fn = fn.replace(old, '++spin30.attempts;')
    assert fn.count('REX_LEAVE_GLOBAL_LOCK();') == 4
    fn = fn.replace('REX_LEAVE_GLOBAL_LOCK();', '// Reservation atomic completed; no global mutex was acquired.')
    fn = fn.replace('*(uint32_t*)REX_RAW_ADDR(ea)', ATOMIC_LOAD)
    fn = fn.replace('loc_822D7450:', 'loc_822D7450:\n __builtin_ia32_pause();\n if ((ctx.r8.u32 & 255u) == 255u) (void)::sched_yield();')
    assert 'GLOBAL_LOCK' not in fn
    u.put(name, '#include <sched.h>\n' + source[:a] + fn + source[b:])


def stage32(u):
    names = {'Release_RefCounted_829FF648', 'Release_RefCounted_821C67D8', 'IncrementRefcountAtomic_821FC048'}
    found = []
    for p in sorted(u.generated.glob('fable_2_recomp.*.cpp')):
        source = p.read_text()  # this stage started from the generated code
        edits = []
        for n in names:
            mark = 'DEFINE_REX_FUNC(' + n + ') {'
            if mark not in source:
                continue
            a = source.index(mark)
            b = source.find('\nDEFINE_REX_FUNC(', a + 1)
            assert b > 0
            fn = source[a:b]
            assert fn.count('REX_ENTER_GLOBAL_LOCK();') == fn.count('REX_LEAVE_GLOBAL_LOCK();') == fn.count('REX_CHECK_GLOBAL_LOCK()') == 1
            block = fn.split('REX_ENTER_GLOBAL_LOCK();')[1].split('REX_LEAVE_GLOBAL_LOCK();')[0]
            assert '__sync_bool_compare_and_swap' in block and 'REX_STORE_' not in block and 'ctx.lr =' not in block and '(ctx, base)' not in block
            tag = '0x' + n.rsplit('_', 1)[-1]
            fn = fn.replace('ctx.r9.u64 = REX_CHECK_GLOBAL_LOCK();', '{ AtomicProbe32<' + tag + '> probe32; ctx.r9.u64 = ctx.msr & 0x8000u;')
            fn = fn.replace('REX_ENTER_GLOBAL_LOCK();', '// Native atomic reservation; no runtime-wide mutex needed.')
            fn = fn.replace('REX_LEAVE_GLOBAL_LOCK();', '}')
            fn = fn.replace('*(uint32_t*)REX_RAW_ADDR(ea)', ATOMIC_LOAD)
            assert 'GLOBAL_LOCK' not in fn
            edits.append((a, b, fn))
            found.append(n)
        if not edits:
            continue
        for a, b, fn in sorted(edits, reverse=True):
            source = source[:a] + fn + source[b:]
        u.put(p.name, REF32_HELPER + source)
    assert set(found) == names


def stage33(u):
    blocks = 0
    for p in sorted(u.generated.glob('fable_2_recomp.*.cpp')):
        source = u.get(p.name)
        details = []

        def patch(m):
            b = m.group('body')
            mid = m.group('mid')
            ok = (b.count('__sync_bool_compare_and_swap') == 1 and b.count('ctx.reserved.u32 = *(uint32_t*)REX_RAW_ADDR(ea);') == 1
                  and len(b) < 2400 and not any(x in b + mid for x in ['REX_STORE_', 'REX_CALL', 'ctx.lr =', '(ctx, base)', 'goto ', 'return;', 'REX_LOAD_']))
            if not ok:
                return m.group(0)
            assert 'ctx.msr =' in mid
            b = b.replace('*(uint32_t*)REX_RAW_ADDR(ea)', ATOMIC_LOAD)
            details.append(m.group(1))
            return 'ctx.r' + m.group(1) + '.u64 = ctx.msr & 0x8000u;' + mid + '/* single native CAS: no global mutex */' + b

        changed = ATOMIC33.sub(patch, source)
        if not details:
            continue
        assert changed.count('__sync_bool_compare_and_swap') == source.count('__sync_bool_compare_and_swap')
        assert source.count('REX_ENTER_GLOBAL_LOCK();') - changed.count('REX_ENTER_GLOBAL_LOCK();') == len(details)
        assert source.count('REX_LEAVE_GLOBAL_LOCK();') - changed.count('REX_LEAVE_GLOBAL_LOCK();') == len(details)
        u.put(p.name, changed)
        blocks += len(details)
    return blocks


def stage54(u):
    guest = {166: 'using SpinClock30=std::chrono::steady_clock;',
             179: 'using Clock=std::chrono::steady_clock;',
             187: 'using Clock=std::chrono::steady_clock;',
             68: 'using Clock=std::chrono::steady_clock;'}
    for n, old in guest.items():
        name = f'fable_2_recomp.{n}.cpp'
        assert name in u.changed, name
        t = u.get(name)
        assert t.count(old) >= 1
        u.put(name, TSC54 + t.replace(old, old.replace('std::chrono::steady_clock', 'TscClock54')))


def stage62(u):
    name = 'fable_2_recomp.14.cpp'
    assert name in u.changed
    patched = replace_once(u.get(name), DEADLOCK62_OLD, DEADLOCK62_NEW)
    include = '#include "fable_2_funcs.14.h"\n'
    assert patched.count(include) == 1
    patched = patched.replace(include, include + 'DECLARE_REX_FUNC(__imp__RtlTryEnterCriticalSection);\n', 1)
    if '#include <cstdio>' not in patched:
        patched = '#include <cstdio>\n' + patched
    u.put(name, patched)


def stage71(u):
    n14 = 'fable_2_recomp.14.cpp'
    t14 = replace_once(u.get(n14), '''		ctx.lr = 0x822BA584;
		KeWaitForMultipleObjects_NoApcRetry(ctx, base);
		if (reenter62) {''', '''		ctx.lr = 0x822BA584;
		static Timer71 frame_wait71{"FABLE2-FRAMEWAIT71"};
		const auto wait_begin71 = std::chrono::steady_clock::now();
		KeWaitForMultipleObjects_NoApcRetry(ctx, base);
		frame_wait71.add(wait_begin71);
		if (reenter62) {''')
    inc14 = '#include "fable_2_funcs.14.h"\n'
    u.put(n14, replace_once(t14, inc14, '#include <chrono>\n' + inc14 + TIMER71))
    n98 = 'fable_2_recomp.98.cpp'
    assert n98 in u.changed
    t98 = replace_once(u.get(n98), '''	ctx.lr = 0x8262DE18;
	sub_8262DE70(ctx, base);
''', '''	ctx.lr = 0x8262DE18;
	{
		static Timer71 worker_job71{"FABLE2-WORKER71"};
		const auto job_begin71 = std::chrono::steady_clock::now();
		sub_8262DE70(ctx, base);
		worker_job71.add(job_begin71);
	}
''')
    inc98 = '#include "fable_2_funcs.98.h"\n'
    u.put(n98, replace_once(t98, inc98, '#include <chrono>\n#include <cstdio>\n' + inc98 + TIMER71))


def stage75(u):
    for unit in (186, 200, 251):
        name = f'fable_2_recomp.{unit}.cpp'
        t = '#include <cstdio>\n' + u.get(name)
        if unit in (186, 200):
            # These two call sites pass the MUSIC_MENU string.
            lr = '0x823795D4' if unit == 186 else '0x82336D58'
            needle = f'ctx.lr = {lr};\n\tsub_829F7ED8(ctx, base);'
            assert t.count(needle) == 1
            label = 'main' if unit == 186 else 'screen'
            t = t.replace(needle, f'''ctx.lr = {lr};
    std::fprintf(stderr,"[FABLE2-MENU75] {label} request manager=0x%08x backend=0x%08x priority=%u\\n",ctx.r3.u32,REX_LOAD_U32(ctx.r3.u32+16),ctx.r4.u32);std::fflush(stderr);
    sub_829F7ED8(ctx, base);
    std::fprintf(stderr,"[FABLE2-MENU75] {label} result=0x%08x\\n",ctx.r3.u32);std::fflush(stderr);''')
        else:
            needle = 'DEFINE_REX_FUNC(sub_829F9F60) {\n\tREX_FUNC_PROLOGUE();'
            assert t.count(needle) == 1
            t = t.replace(needle, needle + '''
    static thread_local unsigned logs75=0;
    if(logs75++<60){std::fprintf(stderr,"[FABLE2-MENU75] dispatch priority=%u active_priority=%u target=0x%08x caller=0x%08x\\n",ctx.r4.u32,REX_LOAD_U32(ctx.r3.u32+24),REX_LOAD_U32(ctx.r3.u32+ctx.r4.u32*4),uint32_t(ctx.lr));std::fflush(stderr);}
''')
            for lr in ('0x829F9FE4', '0x829FA014'):
                needle = f'ctx.lr = {lr};\n\tREX_CALL_INDIRECT_FUNC(ctx.ctr.u32);'
                assert t.count(needle) == 1
                t = t.replace(needle, needle + '''\n    if(logs75<61){std::fprintf(stderr,"[FABLE2-MENU75] event_result=0x%08x\\n",ctx.r3.u32);std::fflush(stderr);}
''')
        u.put(name, t)


def stage113(u):
    name = 'fable_2_recomp.31.cpp'
    t = '#include <cstdio>\n' + u.get(name)
    a, b = function_span(t, 'ProcessAndProcessAndProcess2320_82C4F240')
    f = t[a:b]
    needle = 'REX_FUNC_PROLOGUE();'
    assert f.count(needle) == 1
    u.put(name, t[:a] + f.replace(needle, needle + MENU113_EXTRA) + t[b:])


def fix_pch(generated):
    # The patched ReXGlue template emits this line itself; an unpatched codegen does not.
    pch = generated / 'fable_2_pch.h'
    old = '#if REX_PLATFORM_WIN32 || (REX_PLATFORM_MAC && REX_ARCH_ARM64)\n'
    new = '#if REX_PLATFORM_WIN32 || (REX_PLATFORM_MAC && REX_ARCH_ARM64) || defined(REX_PLATFORM_PS5)\n'
    text = pch.read_text()
    if new not in text:
        pch.write_text(replace_once(text, old, new))


def main():
    generated, out = Path(sys.argv[1]), Path(sys.argv[2])
    fix_pch(generated)
    u = Units(generated)
    stage30(u)
    stage31(u)
    stage32(u)
    blocks = stage33(u)
    stage54(u)
    stage62(u)
    stage71(u)
    stage75(u)
    stage113(u)
    out.mkdir(parents=True, exist_ok=True)
    for name, text in sorted(u.changed.items()):
        (out / name).write_text(text)
    print(f'transform: {len(u.changed)} units changed ({blocks} native-atomic blocks) -> {out}')


if __name__ == '__main__':
    main()

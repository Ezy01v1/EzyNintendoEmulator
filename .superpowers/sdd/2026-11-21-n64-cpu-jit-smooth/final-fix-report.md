# Final fix report

1. Doc "Veredicto": recomputed from build/vo/ft_new_1..5.csv. Slow fields (>16.7 ms) with compile_ms<2: 26/36/15/23/48 (of 34/40/20/28/53). Non-compile maxima 22.3/26.2/22.2/25.6/32.5 ms. All are odd fields (1545-1893). Doc corrected (not noise; cause unverified: RDP, interpreted CPU after resume skips, FPU); next-round proposal now says measure those fields first.
2. x64_backend.cpp compile(): catches std::logic_error from RegAlloc/Emitter -> CompileError::Unsupported. Vr4300Jit::compile already handles Unsupported: block.fn = nullptr, block cached, interpreter runs that pc (confirmed in code, vr4300_jit.cpp ~410). No test added: both throws are unreachable through the public API (IR builder rejects use-before-def; ops have <=3 operands vs >=? registers), so a test would be contrived.
3. Stats: added compile_max_ns and code_bytes (at the end of Vr4300Jit::Stats; const BlockCache::code() added), printed at the end of the `CPU JIT:` line. Measurement (Release, usual command): blocks 13445, compile ms 535.6, compile max ms 1.2832, code bytes 13828337 (~1028 B/block). Added to the doc.
4. pendientes/recordatorios.md: GC/Wii design note added next to the phase-4 entry.

Tests: Debug ctest 11/11; Release peterlemon|systemtest 183/183; MK64 SHA-1 4a4c0711b75a574a8f50bb2b49934bb6a494d518 (matches).
Files: docs/performance/2026-11-21-n64-cpu-jit-smooth.md, pendientes/recordatorios.md, src/core/jit/{block_cache.hpp,x64_backend.cpp}, src/core/n64/cpu/vr4300_jit.{hpp,cpp}, tools/n64_runner/main.cpp.
Concerns: none. Not pushed.

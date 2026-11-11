#include "n64/rsp_jit.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <unordered_map>
#include <utility>

#if EZY_N64_RSP_JIT
#include <asmjit/x86.h>
#endif

namespace ezy::n64 {

struct RspJit::Impl {
    Stats stats;
    bool enabled = false;
    std::size_t budget = 0, bytes = 0;
#if EZY_N64_RSP_JIT
    using Function = void (*)(RspJitView*, RspJitRun*);
    struct Entry { Function function = nullptr; bool attempted = false; };
    asmjit::JitRuntime runtime;
    std::array<Entry, 1024> entries{};
    // PC is part of the identity because native blocks materialize PC/next-PC.
    // Keep all bytes the compiler could inspect, including the terminating opcode.
    struct Key {
        unsigned pc = 0;
        std::array<std::uint8_t, 128> code{};
        bool operator==(const Key&) const = default;
    };
    struct Hash {
        std::size_t operator()(const Key& key) const noexcept {
            std::size_t hash = 14695981039346656037ULL;
            for (auto byte : key.code) hash = (hash ^ byte) * 1099511628211ULL;
            return (hash ^ key.pc) * 1099511628211ULL;
        }
    };
    std::unordered_map<Key, Function, Hash> translations;

    struct Errors final : asmjit::ErrorHandler {
        bool failed = false;
        void handle_error(asmjit::Error, const char*, asmjit::BaseEmitter*) override { failed = true; }
    };

    void flush() {
        // The pinned allocator's soft reset can retain tree links from freed
        // executable blocks. A full flush must discard allocator metadata too.
        runtime.reset(asmjit::ResetPolicy::kHard);
        entries = {};
        translations.clear();
        bytes = 0;
        ++stats.invalidations;
        ++stats.cache_flushes;
    }

    static bool supported(std::uint32_t word) {
        const auto op = word >> 26;
        if (op == 0) {
            switch (word & 63) {
                case 0: case 2: case 3: case 4: case 6: case 7:
                case 0x20: case 0x21: case 0x22: case 0x23: case 0x24:
                case 0x25: case 0x26: case 0x27: case 0x2A: case 0x2B: return true;
                default: return false;
            }
        }
        return (op >= 8 && op <= 15) || op == 0x12 ||
               (op >= 0x20 && op <= 0x25 && op != 0x22) || op == 0x27 ||
               op == 0x28 || op == 0x29 || op == 0x2B || op == 0x32 || op == 0x3A;
    }

    Function compile(unsigned pc, std::span<const std::uint8_t,4096> imem) {
        using namespace asmjit;
        const auto word_at = [&](unsigned at) {
            return (std::uint32_t{imem[at]} << 24) | (imem[at+1] << 16) | (imem[at+2] << 8) | imem[at+3];
        };
        if (!supported(word_at(pc))) return nullptr;
        const auto started = std::chrono::steady_clock::now();
        CodeHolder code;
        Errors errors;
        if (static_cast<unsigned>(code.init(runtime.environment(), runtime.cpu_features())) != 0) {
            enabled = false; return nullptr;
        }
        code.set_error_handler(&errors);
        x86::Compiler cc(&code);
        auto* function = cc.add_func(FuncSignature::build<void,RspJitView*,RspJitRun*>());
        if (!function) { enabled = false; return nullptr; }
        auto view = cc.new_gp_ptr(), run = cc.new_gp_ptr();
        function->set_arg(0,view); function->set_arg(1,run);
        auto gpr = cc.new_gp_ptr(), dmem = cc.new_gp_ptr();
        auto pc_ptr = cc.new_gp_ptr(), next_ptr = cc.new_gp_ptr(), phase_ptr = cc.new_gp_ptr();
        cc.mov(gpr,x86::qword_ptr(view,offsetof(RspJitView,gpr)));
        cc.mov(dmem,x86::qword_ptr(view,offsetof(RspJitView,dmem)));
        cc.mov(pc_ptr,x86::qword_ptr(view,offsetof(RspJitView,pc)));
        cc.mov(next_ptr,x86::qword_ptr(view,offsetof(RspJitView,next_pc)));
        cc.mov(phase_ptr,x86::qword_ptr(view,offsetof(RspJitView,phase)));
        auto when = cc.new_gp64(), limit = cc.new_gp64();
        auto retired = cc.new_gp32(), maximum = cc.new_gp32(), phase = cc.new_gp32();
        cc.mov(when,x86::qword_ptr(run,offsetof(RspJitRun,when)));
        cc.mov(limit,x86::qword_ptr(run,offsetof(RspJitRun,limit)));
        cc.mov(retired,x86::dword_ptr(run,offsetof(RspJitRun,retired)));
        cc.mov(maximum,x86::dword_ptr(run,offsetof(RspJitRun,maximum)));
        cc.movzx(phase,x86::byte_ptr(phase_ptr));
        const auto exit = cc.new_label();
        const auto load = [&](unsigned reg) {
            auto value = cc.new_gp32();
            if (reg == 0) cc.xor_(value,value);
            else cc.mov(value,x86::dword_ptr(gpr,reg*4));
            return value;
        };
        const auto store = [&](unsigned reg, const x86::Gp& value) {
            if (reg != 0) cc.mov(x86::dword_ptr(gpr,reg*4),value);
        };
        for (unsigned n=0, at=pc; n<32 && at<4096; ++n,at+=4) {
            const auto word = word_at(at);
            if (!supported(word)) break;
            cc.cmp(when,limit); cc.jae(exit);
            cc.cmp(retired,maximum); cc.jae(exit);
            const unsigned op=word>>26, rs=(word>>21)&31, rt=(word>>16)&31;
            const auto immediate=static_cast<std::int32_t>(static_cast<std::int16_t>(word));
            if (op==0x12 || op==0x32 || op==0x3A) {
                auto helper=cc.new_gp_ptr(), opaque=cc.new_gp_ptr();
                cc.mov(helper,x86::qword_ptr(view,offsetof(RspJitView,vector_helper)));
                // A missing thunk is a safe early exit, never a null native call.
                cc.test(helper,helper); cc.jz(exit);
                cc.mov(opaque,x86::qword_ptr(view,offsetof(RspJitView,opaque)));
                InvokeNode* call = nullptr;
                cc.invoke(Out(call),helper,FuncSignature::build<void,void*,std::uint32_t>());
                if (!call) { enabled=false; return nullptr; }
                call->set_arg(0,opaque); call->set_arg(1,Imm(word));
                cc.mov(limit,x86::qword_ptr(run,offsetof(RspJitRun,limit)));
                cc.mov(maximum,x86::dword_ptr(run,offsetof(RspJitRun,maximum)));
                cc.movzx(phase,x86::byte_ptr(phase_ptr));
            } else if (op==0) {
                const unsigned fn=word&63, rd=(word>>11)&31, shift=(word>>6)&31;
                auto a=load(rs), b=load(rt);
                if (fn<=7) {
                    if (fn==0) cc.shl(b,shift);
                    if (fn==2) cc.shr(b,shift);
                    if (fn==3) cc.sar(b,shift);
                    if (fn==4) cc.shl(b,a);
                    if (fn==6) cc.shr(b,a);
                    if (fn==7) cc.sar(b,a);
                    store(rd,b);
                } else {
                    switch(fn) {
                        case 0x20: case 0x21: cc.add(a,b); break;
                        case 0x22: case 0x23: cc.sub(a,b); break;
                        case 0x24: cc.and_(a,b); break;
                        case 0x25: cc.or_(a,b); break;
                        case 0x26: cc.xor_(a,b); break;
                        case 0x27: cc.or_(a,b); cc.not_(a); break;
                        case 0x2A: cc.cmp(a,b); cc.setl(a.r8()); cc.movzx(a,a.r8()); break;
                        case 0x2B: cc.cmp(a,b); cc.setb(a.r8()); cc.movzx(a,a.r8()); break;
                        default: break;
                    }
                    store(rd,a);
                }
            } else if (op>=8 && op<=15) {
                auto a=load(rs);
                switch(op) {
                    case 8: case 9: cc.add(a,immediate); break;
                    case 10: cc.cmp(a,immediate); cc.setl(a.r8()); cc.movzx(a,a.r8()); break;
                    case 11: cc.cmp(a,immediate); cc.setb(a.r8()); cc.movzx(a,a.r8()); break;
                    case 12: cc.and_(a,word&0xFFFF); break;
                    case 13: cc.or_(a,word&0xFFFF); break;
                    case 14: cc.xor_(a,word&0xFFFF); break;
                    case 15: cc.mov(a,(word&0xFFFF)<<16); break;
                    default: break;
                }
                store(rt,a);
            } else {
                const bool writing=op==0x28 || op==0x29 || op==0x2B;
                const unsigned width=(op==0x20 || op==0x24 || op==0x28) ? 1
                                    : (op==0x21 || op==0x25 || op==0x29) ? 2 : 4;
                auto address=load(rs), value=writing ? load(rt) : cc.new_gp32();
                cc.add(address,immediate);
                if (!writing) cc.xor_(value,value);
                for(unsigned byte=0;byte<width;++byte) {
                    auto index=cc.new_gp32(), part=cc.new_gp32();
                    cc.mov(index,address); if(byte) cc.add(index,byte); cc.and_(index,4095);
                    if(writing) {
                        cc.mov(part,value);
                        if(width-1-byte) cc.shr(part,(width-1-byte)*8);
                        cc.mov(x86::byte_ptr(dmem,index.r64()),part.r8());
                    } else {
                        if(byte) cc.shl(value,8);
                        cc.movzx(part,x86::byte_ptr(dmem,index.r64())); cc.or_(value,part);
                    }
                }
                if (!writing) {
                    if(op==0x20) cc.movsx(value,value.r8());
                    if(op==0x21) cc.movsx(value,value.r16());
                    store(rt,value);
                }
            }
            cc.mov(x86::dword_ptr(gpr),0);
            cc.mov(x86::dword_ptr(pc_ptr),(at+4)&0xFFC);
            cc.mov(x86::dword_ptr(next_ptr),(at+8)&0xFFC);
            auto delta=cc.new_gp32(); cc.mov(delta,2); cc.sub(delta,phase);
            cc.add(when,delta.r64()); cc.xor_(phase,1); cc.inc(retired);
            cc.mov(x86::byte_ptr(phase_ptr),phase.r8());
            cc.mov(x86::qword_ptr(run,offsetof(RspJitRun,when)),when);
            cc.mov(x86::dword_ptr(run,offsetof(RspJitRun,retired)),retired);
        }
        cc.bind(exit); cc.end_func();
        const auto error=cc.finalize();
        if(errors.failed || static_cast<unsigned>(error)!=0) { enabled=false; return nullptr; }
        const auto size=code.code_size();
        if(size>budget) { enabled=false; return nullptr; }
        if(bytes+size>budget) flush();
        Function result=nullptr;
        if(static_cast<unsigned>(runtime.add(&result,&code))!=0) { enabled=false; return nullptr; }
        bytes+=size;
        ++stats.compiled_blocks;
        stats.compile_ns+=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now()-started).count());
        return result;
    }
#endif
};

RspJit::RspJit(bool deny_execution,std::size_t code_budget) : impl_(std::make_unique<Impl>()) {
    impl_->budget=code_budget;
#if EZY_N64_RSP_JIT
    impl_->enabled=!deny_execution;
#else
    (void)deny_execution;
#endif
}
RspJit::~RspJit()=default;
bool RspJit::available() const { return impl_->enabled; }
void RspJit::invalidate() {
#if EZY_N64_RSP_JIT
    // Detach this generation's fast lookup table. Historical translations are
    // selected again by exact content, never by a stale PC or hash alone.
    impl_->entries = {};
    ++impl_->stats.invalidations;
#endif
}
void RspJit::record_fallback() { ++impl_->stats.fallback_instructions; }
RspJit::Stats RspJit::stats() const { return impl_->stats; }
void RspJit::run(RspJitView& view,std::span<const std::uint8_t,4096> imem,RspJitRun& run) {
    run.retired=0;
#if EZY_N64_RSP_JIT
    if(!available() || *view.pc>=4096 || (*view.pc&3)!=0 || run.when>=run.limit || run.maximum==0) return;
    Impl::Function function=nullptr;
    try {
        const auto index=*view.pc/4;
        if(!impl_->entries[index].attempted) {
            Impl::Key key;
            key.pc=*view.pc;
            std::copy_n(imem.begin()+key.pc,std::min<std::size_t>(key.code.size(),4096-key.pc),key.code.begin());
            Impl::Function compiled=nullptr;
            if(const auto found=impl_->translations.find(key); found!=impl_->translations.end()) {
                compiled=found->second;
                ++impl_->stats.reused_blocks;
            } else {
                compiled=impl_->compile(*view.pc,imem);
                // Unsupported entry opcodes stay in the per-generation table;
                // don't accumulate unbounded metadata without executable code.
                if(compiled) impl_->translations.emplace(std::move(key),compiled);
            }
            // compile may flush the table; assign only after it returns.
            impl_->entries[index]={compiled,true};
        }
        function=impl_->entries[index].function;
    } catch (...) {
        impl_->enabled=false;
        impl_->flush();
        return;
    }
    if(function) function(&view,&run);
    impl_->stats.native_instructions+=run.retired;
#else
    (void)view; (void)imem;
#endif
}

} // namespace ezy::n64
